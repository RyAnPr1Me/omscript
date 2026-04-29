/// @file const_fold_pass.cpp
/// @brief Constant Folding (ConstFold) pass implementation.
///
/// See const_fold_pass.h for the full design rationale.
///
/// Implementation overview
/// ========================
/// The pass is structured as a recursive bottom-up tree rewriter.
/// `foldExpr(expr)` rewrites a `unique_ptr<Expression>` in place:
///   1. Recurse into all child sub-expressions first (bottom-up).
///   2. After children have been simplified, check whether THIS node has
///      become a foldable literal operation.
///   3. If so, evaluate it and replace the node with a fresh LiteralExpr.
///   4. Increment the fold counter.
///
/// The walk function `foldInStmt` descends into every statement kind that
/// may contain expressions (VarDecl, ExprStmt, ReturnStmt, if/while/for/…).
///
/// Division and modulo by zero are always skipped (the node is left as-is).
/// This is correct because the program may intentionally divide-by-zero and
/// we should not silently change the behaviour.

#include "const_fold_pass.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Literal helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Return the integer value of an INTEGER LiteralExpr, or false if the
/// expression is not an integer literal.
static bool asInt(const Expression* e, int64_t& out) {
    if (!e || e->type != ASTNodeType::LITERAL_EXPR) return false;
    const auto* lit = static_cast<const LiteralExpr*>(e);
    if (lit->literalType != LiteralExpr::LiteralType::INTEGER) return false;
    out = lit->intValue;
    return true;
}

/// Make a fresh INTEGER literal node with value @p v.
static std::unique_ptr<Expression> makeLit(int64_t v) {
    return std::make_unique<LiteralExpr>(static_cast<long long>(v));
}

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

static unsigned foldExpr(std::unique_ptr<Expression>& expr);
static unsigned foldInStmt(Statement* stmt);

// ─────────────────────────────────────────────────────────────────────────────
// Helper predicates
// ─────────────────────────────────────────────────────────────────────────────

/// True when @p count is a valid bit-shift amount for a 64-bit integer:
/// non-negative and strictly less than 64.
static bool isValidShiftCount(int64_t count) noexcept {
    return count >= 0 && count < 64;
}

// ─────────────────────────────────────────────────────────────────────────────
// foldExpr — bottom-up literal folding for one expression node
// ─────────────────────────────────────────────────────────────────────────────

static unsigned foldExpr(std::unique_ptr<Expression>& expr) {
    if (!expr) return 0;
    unsigned count = 0;

    switch (expr->type) {

    // ── Recurse into binary expressions ──────────────────────────────────
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<BinaryExpr*>(expr.get());
        count += foldExpr(bin->left);
        count += foldExpr(bin->right);

        int64_t lv = 0, rv = 0;
        if (!asInt(bin->left.get(), lv) || !asInt(bin->right.get(), rv))
            break; // Not both integer literals — leave as-is.

        const std::string& op = bin->op;
        int64_t result = 0;
        bool    valid  = true;

        // ── Arithmetic ────────────────────────────────────────────────────
        if      (op == "+")  result = lv + rv;
        else if (op == "-")  result = lv - rv;
        else if (op == "*")  result = lv * rv;
        else if (op == "/") {
            if (rv == 0) { valid = false; } // skip: div-by-zero
            else result = lv / rv;
        }
        else if (op == "%") {
            if (rv == 0) { valid = false; } // skip: mod-by-zero
            else result = lv % rv;
        }
        else if (op == "**") {
            // Only fold small non-negative exponents to avoid overflow.
            // rv == 63 would produce 2^63 for base=2, which overflows int64_t
            // (INT64_MIN), so the bound is rv < 63 (i.e. at most 2^62).
            if (rv < 0 || rv >= 63) { valid = false; }
            else {
                result = 1;
                for (int64_t i = 0; i < rv; ++i) result *= lv;
            }
        }
        // ── Bitwise ───────────────────────────────────────────────────────
        else if (op == "&")  result = lv & rv;
        else if (op == "|")  result = lv | rv;
        else if (op == "^")  result = lv ^ rv;
        else if (op == "<<") {
            if (!isValidShiftCount(rv)) { valid = false; }
            else result = static_cast<int64_t>(static_cast<uint64_t>(lv) << rv);
        }
        else if (op == ">>") {
            if (!isValidShiftCount(rv)) { valid = false; }
            else result = lv >> rv; // arithmetic right-shift
        }
        // ── Comparisons ───────────────────────────────────────────────────
        else if (op == "==") result = (lv == rv) ? 1 : 0;
        else if (op == "!=") result = (lv != rv) ? 1 : 0;
        else if (op == "<")  result = (lv  < rv) ? 1 : 0;
        else if (op == "<=") result = (lv <= rv) ? 1 : 0;
        else if (op == ">")  result = (lv  > rv) ? 1 : 0;
        else if (op == ">=") result = (lv >= rv) ? 1 : 0;
        // ── Boolean (treat integer 0 = false, non-zero = true) ────────────
        else if (op == "&&") result = (lv && rv) ? 1 : 0;
        else if (op == "||") result = (lv || rv) ? 1 : 0;
        else valid = false; // unknown operator — leave as-is

        if (valid) {
            expr = makeLit(result);
            ++count;
        }
        break;
    }

    // ── Recurse into unary expressions ───────────────────────────────────
    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<UnaryExpr*>(expr.get());
        count += foldExpr(un->operand);

        int64_t v = 0;
        if (!asInt(un->operand.get(), v)) break;

        const std::string& op = un->op;
        int64_t result = 0;
        bool    valid  = true;

        if      (op == "-") result = -v;
        else if (op == "~") result = ~v;
        else if (op == "!") result = v ? 0 : 1;
        else valid = false;

        if (valid) {
            expr = makeLit(result);
            ++count;
        }
        break;
    }

    // ── Recurse into other composite expressions ──────────────────────────
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<TernaryExpr*>(expr.get());
        count += foldExpr(tern->condition);
        count += foldExpr(tern->thenExpr);
        count += foldExpr(tern->elseExpr);

        // If the condition is a known integer literal we can select a branch.
        int64_t cv = 0;
        if (asInt(tern->condition.get(), cv)) {
            expr = cv ? std::move(tern->thenExpr) : std::move(tern->elseExpr);
            ++count;
        }
        break;
    }

    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<CallExpr*>(expr.get());
        for (auto& arg : call->arguments) count += foldExpr(arg);
        break;
    }

    case ASTNodeType::ASSIGN_EXPR:
        count += foldExpr(static_cast<AssignExpr*>(expr.get())->value);
        break;

    case ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<IndexExpr*>(expr.get());
        count += foldExpr(idx->array);
        count += foldExpr(idx->index);
        break;
    }

    case ASTNodeType::INDEX_ASSIGN_EXPR: {
        auto* ia = static_cast<IndexAssignExpr*>(expr.get());
        count += foldExpr(ia->array);
        count += foldExpr(ia->index);
        count += foldExpr(ia->value);
        break;
    }

    case ASTNodeType::POSTFIX_EXPR:
        count += foldExpr(static_cast<PostfixExpr*>(expr.get())->operand);
        break;

    case ASTNodeType::PREFIX_EXPR:
        count += foldExpr(static_cast<PrefixExpr*>(expr.get())->operand);
        break;

    default:
        break; // LITERAL_EXPR, IDENTIFIER_EXPR, etc. — nothing to fold
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// foldInStmt — apply foldExpr to all expressions in a statement
// ─────────────────────────────────────────────────────────────────────────────

static unsigned foldInStmt(Statement* stmt) {
    if (!stmt) return 0;
    unsigned count = 0;

    switch (stmt->type) {

    case ASTNodeType::VAR_DECL:
        count += foldExpr(static_cast<VarDecl*>(stmt)->initializer);
        break;

    case ASTNodeType::EXPR_STMT:
        count += foldExpr(static_cast<ExprStmt*>(stmt)->expression);
        break;

    case ASTNodeType::RETURN_STMT:
        count += foldExpr(static_cast<ReturnStmt*>(stmt)->value);
        break;

    case ASTNodeType::IF_STMT: {
        auto* ifS = static_cast<IfStmt*>(stmt);
        count += foldExpr(ifS->condition);
        if (ifS->thenBranch) count += foldInStmt(ifS->thenBranch.get());
        if (ifS->elseBranch) count += foldInStmt(ifS->elseBranch.get());
        break;
    }

    case ASTNodeType::WHILE_STMT: {
        auto* ws = static_cast<WhileStmt*>(stmt);
        count += foldExpr(ws->condition);
        if (ws->body) count += foldInStmt(ws->body.get());
        break;
    }

    case ASTNodeType::DO_WHILE_STMT: {
        auto* dw = static_cast<DoWhileStmt*>(stmt);
        if (dw->body) count += foldInStmt(dw->body.get());
        count += foldExpr(dw->condition);
        break;
    }

    case ASTNodeType::FOR_STMT: {
        auto* fs = static_cast<ForStmt*>(stmt);
        count += foldExpr(fs->start);
        count += foldExpr(fs->end);
        count += foldExpr(fs->step);
        if (fs->body) count += foldInStmt(fs->body.get());
        break;
    }

    case ASTNodeType::FOR_EACH_STMT: {
        auto* fe = static_cast<ForEachStmt*>(stmt);
        count += foldExpr(fe->collection);
        if (fe->body) count += foldInStmt(fe->body.get());
        break;
    }

    case ASTNodeType::BLOCK: {
        auto* blk = static_cast<BlockStmt*>(stmt);
        for (auto& s : blk->statements) count += foldInStmt(s.get());
        break;
    }

    case ASTNodeType::ASSUME_STMT:
        count += foldExpr(static_cast<AssumeStmt*>(stmt)->condition);
        break;

    case ASTNodeType::THROW_STMT:
        count += foldExpr(static_cast<ThrowStmt*>(stmt)->value);
        break;

    default:
        break;
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

ConstFoldStats runConstFoldPass(Program* program, bool verbose) {
    ConstFoldStats total;
    if (!program) return total;

    for (auto& node : program->functions) {
        auto* fn = static_cast<FunctionDecl*>(node.get());
        if (!fn || !fn->body) continue;

        const unsigned before = total.expressionsFolded;
        for (auto& stmt : fn->body->statements)
            total.expressionsFolded += foldInStmt(stmt.get());

        const unsigned applied = total.expressionsFolded - before;
        if (verbose && applied > 0) {
            std::cerr << "[ConstFold] " << fn->name
                      << ": " << applied << " expression(s) folded\n";
        }
    }
    return total;
}

} // namespace omscript
