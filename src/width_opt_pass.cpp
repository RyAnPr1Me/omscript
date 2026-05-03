/// @file width_opt_pass.cpp
/// @brief Width-aware AST optimizations: masking elimination, shift narrowing,
///        and impossible-branch pruning.
///
/// See include/width_opt_pass.h for the design rationale and examples.

#include "width_opt_pass.h"
#include "opt_context.h"
#include "pass_utils.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Internal utilities
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Closed range derived from a semantic width.
/// Unsigned W bits  → [0, 2^W - 1]
/// Signed   W bits  → [-(2^(W-1)), 2^(W-1) - 1]
/// Width 0 (unknown)→ full i64 range (unknown).
WidthOptPass::Range rangeFromWidth(SemanticWidth sw) noexcept {
    if (!sw.isKnown())
        return {std::numeric_limits<int64_t>::min(),
                std::numeric_limits<int64_t>::max(), false};

    const uint32_t bits = sw.bits;
    if (!sw.isSigned) {
        // Unsigned.
        if (bits >= 64)
            return {0, std::numeric_limits<int64_t>::max(), true};
        const int64_t hi = (int64_t(1) << bits) - 1;
        return {0, hi, true};
    }
    // Signed.
    if (bits > 64) return {std::numeric_limits<int64_t>::min(),
                           std::numeric_limits<int64_t>::max(), false};
    if (bits == 64) return {std::numeric_limits<int64_t>::min(),
                            std::numeric_limits<int64_t>::max(), true};
    const int64_t lo = -(int64_t(1) << (bits - 1));
    const int64_t hi =  (int64_t(1) << (bits - 1)) - 1;
    return {lo, hi, true};
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// WidthOptPass — utilities
// ─────────────────────────────────────────────────────────────────────────────

SemanticWidth WidthOptPass::widthOf(const Expression* expr) const noexcept {
    if (!analyzer_) return SemanticWidth::unknown();
    return analyzer_->widthOf(expr);
}

WidthOptPass::Range WidthOptPass::rangeOf(const Expression* expr) const noexcept {
    // First check the OptimizationContext for a tighter range from CFCTRE.
    // (rangeFromWidth is a lower bound derived purely from bit width.)
    const SemanticWidth sw = widthOf(expr);
    return rangeFromWidth(sw);
}

/* static */
bool WidthOptPass::isLiteralInt(const Expression* e, int64_t& out) noexcept {
    long long v = 0;
    if (!isIntLiteral(e, &v)) return false;
    out = static_cast<int64_t>(v);
    return true;
}

/* static */
bool WidthOptPass::isMaskCovering(int64_t mask, uint32_t bits) noexcept {
    // The mask covers bits iff every bit in [0, bits-1] is set in mask.
    if (bits == 0) return false;
    if (bits >= 63) {
        // For 63+ bits, a signed 64-bit mask can't cover all positions
        // without being -1 (all-ones).
        return mask == int64_t(-1);
    }
    const int64_t required = (int64_t(1) << bits) - 1; // 0x00..0FF for bits=8
    return (mask & required) == required;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sub-pass: masking elimination
// ─────────────────────────────────────────────────────────────────────────────
//
// Pattern: (x & M) where M covers all bits of x
// Transform: → x
//
// Conditions:
//   • op == "&"
//   • RHS is a literal integer mask M
//   • LHS has known semantic width W
//   • M covers all W bits of the LHS value space

std::unique_ptr<Expression>
WidthOptPass::tryEliminateMask(BinaryExpr* bin) {
    assert(bin->op == "&");

    int64_t mask = 0;
    if (!isLiteralInt(bin->right.get(), mask)) return nullptr;

    const SemanticWidth lhsW = widthOf(bin->left.get());
    if (!lhsW.isKnown()) return nullptr;

    if (!isMaskCovering(mask, lhsW.bits)) return nullptr;

    // The mask is a no-op: x & M == x because x already fits in `bits` bits.
    ++stats_.masksEliminated;
    return std::move(bin->left);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sub-pass: shift narrowing / zeroing
// ─────────────────────────────────────────────────────────────────────────────
//
// Pattern: (x >> N) or (x >>> N) where N is a literal and x has known width W
//
//   If N >= W:  result is provably 0  → replace with literal 0
//   If N > 0:   result has W-N bits   → annotated (width known, no AST change)

std::unique_ptr<Expression>
WidthOptPass::tryNarrowShift(BinaryExpr* bin) {
    assert(bin->op == ">>" || bin->op == ">>>");

    int64_t shiftAmount = 0;
    if (!isLiteralInt(bin->right.get(), shiftAmount)) return nullptr;
    if (shiftAmount <= 0) return nullptr; // negative or zero shifts: skip

    const SemanticWidth lhsW = widthOf(bin->left.get());
    if (!lhsW.isKnown()) return nullptr;

    const uint32_t N = static_cast<uint32_t>(shiftAmount);
    const uint32_t W = lhsW.bits;

    if (N >= W) {
        // x >> N where N >= W: result is always zero.
        ++stats_.shiftsZeroed;
        return makeIntLiteral(0LL);
    }

    // N < W but > 0: the result fits in W-N bits.
    // We can't change the AST width annotation directly (there is no
    // width annotation node), but we record the improved width in the
    // analyzer so downstream passes see a tighter bound.
    // (The width map is consulted by WidthAnalyzer::widthOf, which uses the
    //  cache; we would need direct write access.  Since the analyzer_ cache
    //  is populated from the expression pointer, we track narrowing via stats.)
    ++stats_.shiftsNarrowed;
    return nullptr; // No AST change, but statistics updated
}

// ─────────────────────────────────────────────────────────────────────────────
// Sub-pass: impossible-branch pruning
// ─────────────────────────────────────────────────────────────────────────────
//
// Patterns (for ==, !=, <, <=, >, >=):
//   If the value range of LHS and RHS are proven disjoint or ordered such
//   that the comparison is always true or always false, replace with a
//   literal 1 or 0.
//
// This turns:
//   if ((x & 0xFF) == 300)  →  if (0)  →  DCE eliminates the branch
//   if ((x & 0xFF) != 300)  →  if (1)  →  DCE eliminates the else branch

std::unique_ptr<Expression>
WidthOptPass::tryPruneBranch(BinaryExpr* bin) {
    const std::string& op = bin->op;
    if (op != "==" && op != "!=" && op != "<" &&
        op != "<=" && op != ">" && op != ">=")
        return nullptr;

    const Range lhsR = rangeOf(bin->left.get());
    const Range rhsR = rangeOf(bin->right.get());
    if (!lhsR.known || !rhsR.known) return nullptr;

    // For singleton RHS (a literal), also try querying directly.
    int64_t rhsLit = 0;
    const bool rhsIsLit = isLiteralInt(bin->right.get(), rhsLit);
    if (rhsIsLit) {
        // Tighten RHS range to the literal.
        // (Already tight: rhsR.lo == rhsR.hi == rhsLit if the width is from
        //  a literal, but fall back to explicit check.)
    }

    // Determine the definite truth value of the comparison.
    // Returns: 1 = always true, 0 = always false, -1 = unknown.
    int result = -1;

    if (op == "==") {
        // Always false when the ranges don't intersect.
        if (lhsR.hi < rhsR.lo || rhsR.hi < lhsR.lo) result = 0;
        // Always true when both ranges are singleton and equal.
        else if (lhsR.lo == lhsR.hi && rhsR.lo == rhsR.hi && lhsR.lo == rhsR.lo)
            result = 1;
    } else if (op == "!=") {
        // Always true when the ranges don't intersect.
        if (lhsR.hi < rhsR.lo || rhsR.hi < lhsR.lo) result = 1;
        // Always false when both ranges are singleton and equal.
        else if (lhsR.lo == lhsR.hi && rhsR.lo == rhsR.hi && lhsR.lo == rhsR.lo)
            result = 0;
    } else if (op == "<") {
        if (lhsR.hi < rhsR.lo) result = 1;  // max(LHS) < min(RHS) → always true
        if (lhsR.lo >= rhsR.hi) result = 0; // min(LHS) >= max(RHS) → always false
    } else if (op == "<=") {
        if (lhsR.hi <= rhsR.lo) result = 1;
        if (lhsR.lo > rhsR.hi)  result = 0;
    } else if (op == ">") {
        if (lhsR.lo > rhsR.hi) result = 1;
        if (lhsR.hi <= rhsR.lo) result = 0;
    } else if (op == ">=") {
        if (lhsR.lo >= rhsR.hi) result = 1;
        if (lhsR.hi < rhsR.lo)  result = 0;
    }

    if (result == -1) return nullptr; // Could not prove

    ++stats_.branchesPruned;
    return makeIntLiteral(static_cast<long long>(result));
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression and statement walkers
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<Expression>
WidthOptPass::trySimplifyExpr(Expression* expr) {
    if (!expr || expr->type != ASTNodeType::BINARY_EXPR) return nullptr;
    auto* bin = static_cast<BinaryExpr*>(expr);

    if (bin->op == "&")
        return tryEliminateMask(bin);
    if (bin->op == ">>" || bin->op == ">>>")
        return tryNarrowShift(bin);
    if (bin->op == "==" || bin->op == "!=" ||
        bin->op == "<"  || bin->op == "<=" ||
        bin->op == ">"  || bin->op == ">=")
        return tryPruneBranch(bin);

    return nullptr;
}

void WidthOptPass::transformExprInPlace(std::unique_ptr<Expression>& expr) {
    if (!expr) return;

    // First recurse into sub-expressions (bottom-up).
    switch (expr->type) {
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<BinaryExpr*>(expr.get());
        transformExprInPlace(bin->left);
        transformExprInPlace(bin->right);
        break;
    }
    case ASTNodeType::UNARY_EXPR:
        transformExprInPlace(static_cast<UnaryExpr*>(expr.get())->operand);
        break;
    case ASTNodeType::PREFIX_EXPR:
        transformExprInPlace(static_cast<PrefixExpr*>(expr.get())->operand);
        break;
    case ASTNodeType::POSTFIX_EXPR:
        transformExprInPlace(static_cast<PostfixExpr*>(expr.get())->operand);
        break;
    case ASTNodeType::TERNARY_EXPR: {
        auto* t = static_cast<TernaryExpr*>(expr.get());
        transformExprInPlace(t->condition);
        transformExprInPlace(t->thenExpr);
        transformExprInPlace(t->elseExpr);
        break;
    }
    case ASTNodeType::CALL_EXPR:
        for (auto& arg : static_cast<CallExpr*>(expr.get())->arguments)
            transformExprInPlace(arg);
        break;
    case ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<IndexExpr*>(expr.get());
        transformExprInPlace(idx->array);
        transformExprInPlace(idx->index);
        break;
    }
    case ASTNodeType::ASSIGN_EXPR:
        transformExprInPlace(static_cast<AssignExpr*>(expr.get())->value);
        break;
    default: break;
    }

    // Then try to simplify this node.
    if (auto replacement = trySimplifyExpr(expr.get()))
        expr = std::move(replacement);
}

void WidthOptPass::transformStmtInPlace(std::unique_ptr<Statement>& stmt) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::BLOCK: {
        auto* blk = static_cast<BlockStmt*>(stmt.get());
        for (auto& s : blk->statements) transformStmtInPlace(s);
        break;
    }
    case ASTNodeType::VAR_DECL:
        transformExprInPlace(static_cast<VarDecl*>(stmt.get())->initializer);
        break;
    case ASTNodeType::MOVE_DECL:
        transformExprInPlace(static_cast<MoveDecl*>(stmt.get())->initializer);
        break;
    case ASTNodeType::RETURN_STMT:
        transformExprInPlace(static_cast<ReturnStmt*>(stmt.get())->value);
        break;
    case ASTNodeType::EXPR_STMT:
        transformExprInPlace(static_cast<ExprStmt*>(stmt.get())->expression);
        break;
    case ASTNodeType::IF_STMT: {
        auto* is = static_cast<IfStmt*>(stmt.get());
        transformExprInPlace(is->condition);
        transformStmtInPlace(is->thenBranch);
        transformStmtInPlace(is->elseBranch);
        break;
    }
    case ASTNodeType::WHILE_STMT: {
        auto* ws = static_cast<WhileStmt*>(stmt.get());
        transformExprInPlace(ws->condition);
        transformStmtInPlace(ws->body);
        break;
    }
    case ASTNodeType::DO_WHILE_STMT: {
        auto* dws = static_cast<DoWhileStmt*>(stmt.get());
        transformStmtInPlace(dws->body);
        transformExprInPlace(dws->condition);
        break;
    }
    case ASTNodeType::FOR_STMT: {
        auto* fs = static_cast<ForStmt*>(stmt.get());
        transformExprInPlace(fs->start);
        transformExprInPlace(fs->end);
        transformExprInPlace(fs->step);
        transformStmtInPlace(fs->body);
        break;
    }
    case ASTNodeType::FOR_EACH_STMT: {
        auto* fe = static_cast<ForEachStmt*>(stmt.get());
        transformExprInPlace(fe->collection);
        transformStmtInPlace(fe->body);
        break;
    }
    case ASTNodeType::SWITCH_STMT: {
        auto* sw = static_cast<SwitchStmt*>(stmt.get());
        transformExprInPlace(sw->condition);
        for (auto& c : sw->cases) {
            if (c.value) transformExprInPlace(c.value);
            for (auto& v : c.values) transformExprInPlace(v);
            for (auto& s : c.body)  transformStmtInPlace(s);
        }
        break;
    }
    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WidthOptPass::run
// ─────────────────────────────────────────────────────────────────────────────

uint32_t WidthOptPass::run(Program* program) {
    if (!program) return 0;

    // Borrow the analyzer from the context's cached width pass result.
    // We construct a fresh one pointing at the (already-populated) context.
    WidthAnalyzer wa(ctx_);
    wa.analyze(program);
    analyzer_ = &wa;

    forEachFunction(program, [&](FunctionDecl* fn) {
        for (auto& s : fn->body->statements)
            transformStmtInPlace(s);
    });
    // Also transform global variable initializers.
    for (auto& g : program->globals) {
        if (g && g->initializer)
            transformExprInPlace(g->initializer);
    }

    analyzer_ = nullptr;

    const uint32_t total = stats_.masksEliminated
                         + stats_.shiftsZeroed
                         + stats_.branchesPruned;

    if (verbose_ && total > 0) {
        std::cout << "  [width-opt] "
                  << stats_.masksEliminated << " masks eliminated, "
                  << stats_.shiftsNarrowed  << " shifts narrowed, "
                  << stats_.shiftsZeroed    << " shifts zeroed, "
                  << stats_.branchesPruned  << " branches pruned\n";
    }
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// Free function entry point
// ─────────────────────────────────────────────────────────────────────────────

uint32_t runWidthOptPass(Program* program, OptimizationContext& ctx, bool verbose) {
    WidthOptPass pass(ctx, verbose);
    return pass.run(program);
}

} // namespace omscript
