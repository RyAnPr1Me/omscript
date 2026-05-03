/// @file width_legalization.cpp
/// @brief Semantic width tracking and hardware-friendly legalization.
///
/// See include/width_legalization.h for the design rationale.

#include "width_legalization.h"
#include "ast.h"
#include "opt_context.h"
#include "pass_utils.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Minimum bits to represent an unsigned integer value.
/// 0 requires 1 bit.
inline uint32_t minBitsUnsigned(uint64_t v) noexcept {
    if (v == 0) return 1;
    uint32_t n = 0;
    while (v > 0) { v >>= 1; ++n; }
    return n;
}

/// Minimum bits to represent a signed integer value (two's complement).
inline uint32_t minBitsSigned(int64_t v) noexcept {
    if (v == 0 || v == -1) return 1;
    if (v > 0) return minBitsUnsigned(static_cast<uint64_t>(v)) + 1; // +1 for sign
    // For negative: find the smallest N such that -(2^(N-1)) <= v
    int64_t mag = -(v + 1); // avoid overflow for INT64_MIN
    return minBitsUnsigned(static_cast<uint64_t>(mag)) + 1;
}

/// Parse a numeric suffix from a string (e.g., "32" from "i32").
/// Returns 0 on failure.
inline uint32_t parseWidthSuffix(const std::string& s, std::size_t offset) noexcept {
    if (offset >= s.size()) return 0;
    uint32_t n = 0;
    for (std::size_t i = offset; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return 0;
        n = n * 10 + static_cast<uint32_t>(c - '0');
        if (n > 65536) return 0; // sanity cap
    }
    return n;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// SemanticWidth — factory methods
// ─────────────────────────────────────────────────────────────────────────────

SemanticWidth SemanticWidth::fromSignedValue(int64_t v) noexcept {
    return {minBitsSigned(v), /*isSigned=*/true};
}

SemanticWidth SemanticWidth::fromUnsignedValue(uint64_t v) noexcept {
    return {minBitsUnsigned(v), /*isSigned=*/false};
}

SemanticWidth SemanticWidth::fromSignedRange(int64_t lo, int64_t hi) noexcept {
    // Width must accommodate both extremes.
    const uint32_t bLo = minBitsSigned(lo);
    const uint32_t bHi = minBitsSigned(hi);
    return {std::max(bLo, bHi), /*isSigned=*/true};
}

SemanticWidth SemanticWidth::fromUnsignedRange(uint64_t lo, uint64_t hi) noexcept {
    const uint32_t bHi = minBitsUnsigned(hi); // lo ≤ hi; hi dominates
    return {bHi, /*isSigned=*/false};
}

SemanticWidth SemanticWidth::fromAnnotation(const std::string& ann) noexcept {
    if (ann.empty())                     return unknown();
    if (ann == "int"  || ann == "long")  return i(64);
    if (ann == "uint" || ann == "ulong") return u(64);
    if (ann == "short")                  return i(16);
    if (ann == "ushort")                 return u(16);
    if (ann == "byte")                   return u(8);
    if (ann == "sbyte")                  return i(8);
    if (ann == "bool")                   return u(1);
    if (ann == "char")                   return u(8);

    // iN / uN annotations (e.g. "i32", "u8", "i128")
    if (ann.size() >= 2) {
        if (ann[0] == 'i') {
            const uint32_t n = parseWidthSuffix(ann, 1);
            if (n >= 1) return i(n);
        }
        if (ann[0] == 'u') {
            const uint32_t n = parseWidthSuffix(ann, 1);
            if (n >= 1) return u(n);
        }
    }
    return unknown();
}

std::string SemanticWidth::toString() const {
    if (!isKnown()) return "i64"; // default
    return (isSigned ? "i" : "u") + std::to_string(bits);
}

// ─────────────────────────────────────────────────────────────────────────────
// legalizeWidth — core normalization function
// ─────────────────────────────────────────────────────────────────────────────

uint32_t legalizeWidth(uint32_t bits) noexcept {
    if (bits == 0)  return 64; // unknown → default register width
    if (bits <= 8)  return 8;
    if (bits <= 16) return 16;
    if (bits <= 32) return 32;
    if (bits <= 64) return 64;
    // Wide: round up to the next multiple of 64.
    return ((bits + 63) / 64) * 64;
}

SemanticWidth legalize(SemanticWidth sw) noexcept {
    return {legalizeWidth(sw.bits), sw.isSigned};
}

// ─────────────────────────────────────────────────────────────────────────────
// OpWidthRules — binary operation result widths
// ─────────────────────────────────────────────────────────────────────────────

namespace OpWidthRules {

/// Resolved bit count of an operand (unknown → 64, which is a safe default).
static uint32_t resolve(SemanticWidth sw) noexcept {
    return sw.isKnown() ? sw.bits : 64;
}

/// Whether the result should be signed (signed if either operand is signed,
/// following C integer promotion rules as a conservative default).
static bool resultSigned(SemanticWidth lhs, SemanticWidth rhs) noexcept {
    return lhs.isSigned || rhs.isSigned;
}

SemanticWidth addSub(SemanticWidth lhs, SemanticWidth rhs) noexcept {
    const uint32_t bits = std::max(resolve(lhs), resolve(rhs)) + 1;
    return {bits, resultSigned(lhs, rhs)};
}

SemanticWidth mul(SemanticWidth lhs, SemanticWidth rhs) noexcept {
    const uint32_t bits = resolve(lhs) + resolve(rhs);
    return {bits, resultSigned(lhs, rhs)};
}

SemanticWidth divRem(SemanticWidth lhs, SemanticWidth rhs) noexcept {
    // Quotient fits within the dividend; remainder fits within divisor.
    const uint32_t bits = std::max(resolve(lhs), resolve(rhs));
    return {bits, resultSigned(lhs, rhs)};
}

SemanticWidth bitwise(SemanticWidth lhs, SemanticWidth rhs) noexcept {
    const uint32_t bits = std::max(resolve(lhs), resolve(rhs));
    return {bits, resultSigned(lhs, rhs)};
}

SemanticWidth shl(SemanticWidth lhs, uint32_t shiftMax) noexcept {
    const uint32_t bits = resolve(lhs) + shiftMax;
    return {bits, lhs.isSigned};
}

SemanticWidth shr(SemanticWidth lhs) noexcept {
    // Right shift can only shrink; return the lhs width unchanged.
    return lhs;
}

SemanticWidth neg(SemanticWidth operand) noexcept {
    // Two's complement negation of an N-bit value may need N+1 bits
    // to avoid overflow (e.g., -INT_MIN requires an extra bit).
    const uint32_t bits = resolve(operand) + 1;
    return {bits, /*isSigned=*/true};
}

SemanticWidth compare() noexcept {
    return {1, /*isSigned=*/false}; // boolean: 1 bit, unsigned
}

SemanticWidth join(SemanticWidth a, SemanticWidth b) noexcept {
    if (!a.isKnown()) return b;
    if (!b.isKnown()) return a;
    const uint32_t bits  = std::max(a.bits, b.bits);
    const bool     isSigned = a.isSigned || b.isSigned;
    return {bits, isSigned};
}

} // namespace OpWidthRules

// ─────────────────────────────────────────────────────────────────────────────
// WidthAnalyzer — expression-level traversal
// ─────────────────────────────────────────────────────────────────────────────

SemanticWidth WidthAnalyzer::widthOf(const Expression* expr) const noexcept {
    if (!expr) return SemanticWidth::unknown();
    auto it = widths_.find(expr);
    return (it != widths_.end()) ? it->second : SemanticWidth::unknown();
}

WidthInfo WidthAnalyzer::infoOf(const Expression* expr) const noexcept {
    const SemanticWidth sem = widthOf(expr);
    return {sem, legalize(sem)};
}

void WidthAnalyzer::analyze(const Program* program) {
    forEachFunction(program, [&](const FunctionDecl* fn) {
        analyzeFunction(fn);
    });
}

void WidthAnalyzer::analyzeFunction(const FunctionDecl* fn) {
    if (!fn || !fn->body) return;
    // Seed parameter widths from type annotations.
    // (Parameters are not Expression nodes, so we skip caching them here;
    //  IdentifierExpr nodes that refer to parameters will be resolved by name
    //  through analyzeExpr.)
    analyzeStmt(fn->body.get());
}

void WidthAnalyzer::analyzeStmt(const Statement* stmt) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::BLOCK: {
        const auto* blk = static_cast<const BlockStmt*>(stmt);
        for (const auto& s : blk->statements)
            analyzeStmt(s.get());
        break;
    }
    case ASTNodeType::VAR_DECL: {
        const auto* vd = static_cast<const VarDecl*>(stmt);
        if (vd->initializer) analyzeExpr(vd->initializer.get());
        break;
    }
    case ASTNodeType::MOVE_DECL: {
        const auto* md = static_cast<const MoveDecl*>(stmt);
        if (md->initializer) analyzeExpr(md->initializer.get());
        break;
    }
    case ASTNodeType::RETURN_STMT: {
        const auto* rs = static_cast<const ReturnStmt*>(stmt);
        if (rs->value) analyzeExpr(rs->value.get());
        break;
    }
    case ASTNodeType::EXPR_STMT: {
        const auto* es = static_cast<const ExprStmt*>(stmt);
        if (es->expression) analyzeExpr(es->expression.get());
        break;
    }
    case ASTNodeType::IF_STMT: {
        const auto* is = static_cast<const IfStmt*>(stmt);
        if (is->condition) analyzeExpr(is->condition.get());
        analyzeStmt(is->thenBranch.get());
        if (is->elseBranch) analyzeStmt(is->elseBranch.get());
        break;
    }
    case ASTNodeType::WHILE_STMT: {
        const auto* ws = static_cast<const WhileStmt*>(stmt);
        if (ws->condition) analyzeExpr(ws->condition.get());
        analyzeStmt(ws->body.get());
        break;
    }
    case ASTNodeType::DO_WHILE_STMT: {
        const auto* dws = static_cast<const DoWhileStmt*>(stmt);
        analyzeStmt(dws->body.get());
        if (dws->condition) analyzeExpr(dws->condition.get());
        break;
    }
    case ASTNodeType::FOR_STMT: {
        const auto* fs = static_cast<const ForStmt*>(stmt);
        if (fs->start) analyzeExpr(fs->start.get());
        if (fs->end)   analyzeExpr(fs->end.get());
        if (fs->step)  analyzeExpr(fs->step.get());
        analyzeStmt(fs->body.get());
        break;
    }
    case ASTNodeType::FOR_EACH_STMT: {
        const auto* fes = static_cast<const ForEachStmt*>(stmt);
        if (fes->collection) analyzeExpr(fes->collection.get());
        analyzeStmt(fes->body.get());
        break;
    }
    case ASTNodeType::SWITCH_STMT: {
        const auto* sw = static_cast<const SwitchStmt*>(stmt);
        if (sw->condition) analyzeExpr(sw->condition.get());
        for (const auto& c : sw->cases) {
            if (c.value) analyzeExpr(c.value.get());
            for (const auto& v : c.values) analyzeExpr(v.get());
            for (const auto& s : c.body)   analyzeStmt(s.get());
        }
        break;
    }
    case ASTNodeType::CATCH_STMT: {
        const auto* ts = static_cast<const CatchStmt*>(stmt);
        if (ts->body) analyzeStmt(ts->body.get());
        break;
    }
    default:
        break; // break/continue/invalidate/prefetch — no sub-expressions to traverse
    }
}

SemanticWidth WidthAnalyzer::analyzeExpr(const Expression* expr) {
    if (!expr) return SemanticWidth::unknown();

    // Already cached?
    {
        auto it = widths_.find(expr);
        if (it != widths_.end()) return it->second;
    }

    switch (expr->type) {

    // ── Literals ─────────────────────────────────────────────────────────
    case ASTNodeType::LITERAL_EXPR: {
        const auto* lit = static_cast<const LiteralExpr*>(expr);
        switch (lit->literalType) {
        case LiteralExpr::LiteralType::INTEGER:
            // Signed by default; use exact minimum bits.
            return cache(expr, SemanticWidth::fromSignedValue(lit->intValue));
        case LiteralExpr::LiteralType::FLOAT:
            // Floats use 64-bit doubles.
            return cache(expr, SemanticWidth::i(64));
        case LiteralExpr::LiteralType::STRING:
        default:
            return cache(expr, SemanticWidth::unknown());
        }
    }

    // ── Identifiers ───────────────────────────────────────────────────────
    case ASTNodeType::IDENTIFIER_EXPR: {
        // We don't have a symbol table here, so we rely on range info from
        // the OptimizationContext if available.
        const auto* id = static_cast<const IdentifierExpr*>(expr);
        // Try to get a return-range for a zero-parameter function call (const fn).
        // For local variables we fall back to the default i64 width.
        if (const auto rng = ctx_.returnRange(id->name)) {
            const SemanticWidth sw = rng->isNonNeg()
                ? SemanticWidth::fromUnsignedRange(
                      static_cast<uint64_t>(rng->lo),
                      static_cast<uint64_t>(rng->hi))
                : SemanticWidth::fromSignedRange(rng->lo, rng->hi);
            return cache(expr, sw);
        }
        return cache(expr, SemanticWidth::unknown());
    }

    // ── Binary expressions ────────────────────────────────────────────────
    case ASTNodeType::BINARY_EXPR: {
        const auto* bin = static_cast<const BinaryExpr*>(expr);
        const SemanticWidth lhsW = analyzeExpr(bin->left.get());
        const SemanticWidth rhsW = analyzeExpr(bin->right.get());
        const std::string& op = bin->op;

        SemanticWidth result;
        if (op == "+"  || op == "-")               result = OpWidthRules::addSub(lhsW, rhsW);
        else if (op == "*")                         result = OpWidthRules::mul(lhsW, rhsW);
        else if (op == "/" || op == "%")            result = OpWidthRules::divRem(lhsW, rhsW);
        else if (op == "&" || op == "|" || op == "^") result = OpWidthRules::bitwise(lhsW, rhsW);
        else if (op == "<<") {
            // If the shift amount is a literal, we know its max value exactly.
            uint32_t shiftMax = 6; // conservative: up to 63 for 64-bit
            long long shiftVal = 0;
            if (isIntLiteral(bin->right.get(), &shiftVal))
                shiftMax = static_cast<uint32_t>(shiftVal > 0 ? shiftVal : 0);
            result = OpWidthRules::shl(lhsW, shiftMax);
        }
        else if (op == ">>" || op == ">>>")        result = OpWidthRules::shr(lhsW);
        else if (op == "==" || op == "!=" ||
                 op == "<"  || op == "<=" ||
                 op == ">"  || op == ">=")         result = OpWidthRules::compare();
        else if (op == "&&" || op == "||")         result = OpWidthRules::compare();
        else                                        result = OpWidthRules::join(lhsW, rhsW);

        return cache(expr, result);
    }

    // ── Unary expressions ─────────────────────────────────────────────────
    case ASTNodeType::UNARY_EXPR: {
        const auto* un = static_cast<const UnaryExpr*>(expr);
        const SemanticWidth opW = analyzeExpr(un->operand.get());
        if (un->op == "-")
            return cache(expr, OpWidthRules::neg(opW));
        if (un->op == "!")
            return cache(expr, OpWidthRules::compare());
        if (un->op == "~")
            return cache(expr, opW); // bitwise NOT preserves width
        return cache(expr, opW);
    }

    case ASTNodeType::PREFIX_EXPR: {
        const auto* pre = static_cast<const PrefixExpr*>(expr);
        const SemanticWidth opW = analyzeExpr(pre->operand.get());
        if (pre->op == "-")
            return cache(expr, OpWidthRules::neg(opW));
        if (pre->op == "!")
            return cache(expr, OpWidthRules::compare());
        return cache(expr, opW);
    }

    case ASTNodeType::POSTFIX_EXPR: {
        const auto* post = static_cast<const PostfixExpr*>(expr);
        return cache(expr, analyzeExpr(post->operand.get()));
    }

    // ── Ternary ───────────────────────────────────────────────────────────
    case ASTNodeType::TERNARY_EXPR: {
        const auto* tern = static_cast<const TernaryExpr*>(expr);
        analyzeExpr(tern->condition.get());
        const SemanticWidth thenW = analyzeExpr(tern->thenExpr.get());
        const SemanticWidth elseW = analyzeExpr(tern->elseExpr.get());
        return cache(expr, OpWidthRules::join(thenW, elseW));
    }

    // ── Call expressions ──────────────────────────────────────────────────
    case ASTNodeType::CALL_EXPR: {
        const auto* call = static_cast<const CallExpr*>(expr);
        for (const auto& arg : call->arguments)
            analyzeExpr(arg.get());

        // Width-cast intrinsics: iN(x) or uN(x)
        if (BuiltinEffectTable::isWidthCastName(call->callee)) {
            const SemanticWidth sw =
                SemanticWidth::fromAnnotation(call->callee);
            if (sw.isKnown())
                return cache(expr, sw);
        }

        // Constant return from function facts.
        if (auto rng = ctx_.returnRange(call->callee)) {
            const SemanticWidth sw =
                rng->isNonNeg()
                ? SemanticWidth::fromUnsignedRange(
                      static_cast<uint64_t>(rng->lo),
                      static_cast<uint64_t>(rng->hi))
                : SemanticWidth::fromSignedRange(rng->lo, rng->hi);
            return cache(expr, sw);
        }
        return cache(expr, SemanticWidth::unknown());
    }

    // ── Index expressions ─────────────────────────────────────────────────
    case ASTNodeType::INDEX_EXPR: {
        const auto* idx = static_cast<const IndexExpr*>(expr);
        analyzeExpr(idx->array.get());
        analyzeExpr(idx->index.get());
        return cache(expr, SemanticWidth::unknown()); // element type unknown statically
    }

    case ASTNodeType::INDEX_ASSIGN_EXPR: {
        const auto* ia = static_cast<const IndexAssignExpr*>(expr);
        analyzeExpr(ia->array.get());
        analyzeExpr(ia->index.get());
        analyzeExpr(ia->value.get());
        return cache(expr, SemanticWidth::unknown());
    }

    // ── Assign ────────────────────────────────────────────────────────────
    case ASTNodeType::ASSIGN_EXPR: {
        const auto* ae = static_cast<const AssignExpr*>(expr);
        if (ae->value) {
            const SemanticWidth sw = analyzeExpr(ae->value.get());
            return cache(expr, sw);
        }
        return cache(expr, SemanticWidth::unknown());
    }

    // ── Pipe expressions ──────────────────────────────────────────────────
    case ASTNodeType::PIPE_EXPR: {
        const auto* pe = static_cast<const PipeExpr*>(expr);
        analyzeExpr(pe->left.get());
        return cache(expr, SemanticWidth::unknown());
    }

    // ── Array literals ────────────────────────────────────────────────────
    case ASTNodeType::ARRAY_EXPR: {
        const auto* ae = static_cast<const ArrayExpr*>(expr);
        for (const auto& elem : ae->elements)
            analyzeExpr(elem.get());
        return cache(expr, SemanticWidth::unknown()); // pointer to array header
    }

    default:
        return cache(expr, SemanticWidth::unknown());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WidthLegalizationPass
// ─────────────────────────────────────────────────────────────────────────────

void WidthLegalizationPass::run(const Program* program) {
    analyzer_.analyze(program);

    // Collect statistics: walk AST and query each analyzed expression.
    wideCount_     = 0;
    narrowedCount_ = 0;

    std::function<void(const Expression*)> countExpr;
    std::function<void(const Statement*)>  countStmt;

    countExpr = [&](const Expression* e) {
        if (!e) return;
        const SemanticWidth sw = analyzer_.widthOf(e);
        if (sw.isKnown()) {
            if (sw.isWide())    ++wideCount_;
            if (sw.bits != 64) ++narrowedCount_;
        }
        switch (e->type) {
        case ASTNodeType::BINARY_EXPR: {
            const auto* b = static_cast<const BinaryExpr*>(e);
            countExpr(b->left.get()); countExpr(b->right.get()); break;
        }
        case ASTNodeType::UNARY_EXPR: {
            const auto* u = static_cast<const UnaryExpr*>(e);
            countExpr(u->operand.get()); break;
        }
        case ASTNodeType::PREFIX_EXPR: {
            const auto* p = static_cast<const PrefixExpr*>(e);
            countExpr(p->operand.get()); break;
        }
        case ASTNodeType::POSTFIX_EXPR: {
            const auto* p = static_cast<const PostfixExpr*>(e);
            countExpr(p->operand.get()); break;
        }
        case ASTNodeType::TERNARY_EXPR: {
            const auto* t = static_cast<const TernaryExpr*>(e);
            countExpr(t->condition.get());
            countExpr(t->thenExpr.get());
            countExpr(t->elseExpr.get()); break;
        }
        case ASTNodeType::CALL_EXPR: {
            const auto* c = static_cast<const CallExpr*>(e);
            for (const auto& a : c->arguments) countExpr(a.get()); break;
        }
        case ASTNodeType::INDEX_EXPR: {
            const auto* i = static_cast<const IndexExpr*>(e);
            countExpr(i->array.get()); countExpr(i->index.get()); break;
        }
        case ASTNodeType::ASSIGN_EXPR: {
            const auto* a = static_cast<const AssignExpr*>(e);
            countExpr(a->value.get()); break;
        }
        default: break;
        }
    };

    countStmt = [&](const Statement* s) {
        if (!s) return;
        switch (s->type) {
        case ASTNodeType::BLOCK: {
            const auto* b = static_cast<const BlockStmt*>(s);
            for (const auto& st : b->statements) countStmt(st.get()); break;
        }
        case ASTNodeType::VAR_DECL: {
            const auto* v = static_cast<const VarDecl*>(s);
            countExpr(v->initializer.get()); break;
        }
        case ASTNodeType::RETURN_STMT: {
            const auto* r = static_cast<const ReturnStmt*>(s);
            countExpr(r->value.get()); break;
        }
        case ASTNodeType::EXPR_STMT: {
            const auto* es = static_cast<const ExprStmt*>(s);
            countExpr(es->expression.get()); break;
        }
        case ASTNodeType::IF_STMT: {
            const auto* i = static_cast<const IfStmt*>(s);
            countExpr(i->condition.get());
            countStmt(i->thenBranch.get());
            countStmt(i->elseBranch.get()); break;
        }
        case ASTNodeType::WHILE_STMT: {
            const auto* w = static_cast<const WhileStmt*>(s);
            countExpr(w->condition.get()); countStmt(w->body.get()); break;
        }
        case ASTNodeType::FOR_STMT: {
            const auto* f = static_cast<const ForStmt*>(s);
            countExpr(f->start.get()); countExpr(f->end.get());
            countExpr(f->step.get());  countStmt(f->body.get()); break;
        }
        default: break;
        }
    };

    forEachFunction(program, [&](const FunctionDecl* fn) {
        countStmt(fn->body.get());
    });
}

WidthInfo WidthLegalizationPass::infoOf(const Expression* expr) const noexcept {
    return analyzer_.infoOf(expr);
}

} // namespace omscript
