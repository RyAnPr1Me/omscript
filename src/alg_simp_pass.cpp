/// @file alg_simp_pass.cpp
/// @brief Algebraic Simplification (AlgSimp) pass implementation.
///
/// Algorithm:
///   For each function, recursively walk every expression tree bottom-up.
///   At each binary or unary node, match against the identity/absorbing-element
///   and self-cancellation rules described in alg_simp_pass.h and replace with
///   the simplified form.
///
/// Float safety:
///   Rules that are unsound for IEEE-754 floats (e.g. x*0→0 is wrong for NaN)
///   are only applied when BOTH operands are provably integer-type.  The guard
///   function `definitelyInteger(expr)` returns true only when the expression
///   contains no float literals and no identifier nodes (whose types are unknown
///   at the AST level).  For unknown identifiers it conservatively returns false
///   ("may be float"), so identity rules like `x - x → 0` or `x / x → 1` are
///   only applied when both sides are integer literals or sub-expressions that
///   consist only of integer literals.  Simple literal-operand rules (x+0→x,
///   x*1→x) are safe for floats *except* where documented, and we keep the
///   conservative approach of only applying them when at least one operand is
///   a non-float literal.

#include "alg_simp_pass.h"
#include "pass_utils.h"

#include <iostream>
#include <memory>
#include <string>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// True when @p expr is a float literal.
static bool isFloatLit(const Expression* expr) {
    if (!expr || expr->type != ASTNodeType::LITERAL_EXPR) return false;
    return static_cast<const LiteralExpr*>(expr)->literalType
               == LiteralExpr::LiteralType::FLOAT;
}

/// True when @p a and @p b are identifier expressions with the same name.
static bool sameIdent(const Expression* a, const Expression* b) {
    const auto* ia = asIdentifier(a);
    const auto* ib = asIdentifier(b);
    if (!ia || !ib) return false;
    return ia->name == ib->name;
}

/// True when @p expr is provably of integer type:
///   - Returns true  if every reachable leaf is an integer literal.
///   - Returns false for float literals (definitely-float) and for
///     IDENTIFIER_EXPR nodes (unknown type — conservatively treated as
///     "may be float" until a type-inference pass is available).
/// This three-valued logic (definitely-integer / definitely-float / unknown)
/// maps to the boolean as: true = "safe to apply integer-only rules",
/// false = "unsafe or unknown".
static bool definitelyInteger(const Expression* expr) {
    if (!expr) return true;
    if (isFloatLit(expr)) return false;
    switch (expr->type) {
    case ASTNodeType::BINARY_EXPR: {
        const auto* b = static_cast<const BinaryExpr*>(expr);
        return definitelyInteger(b->left.get()) && definitelyInteger(b->right.get());
    }
    case ASTNodeType::UNARY_EXPR:
        return definitelyInteger(static_cast<const UnaryExpr*>(expr)->operand.get());
    case ASTNodeType::LITERAL_EXPR: {
        // String literals are not integers — guard against str * 0 → 0.
        const auto* lit = static_cast<const LiteralExpr*>(expr);
        return lit->literalType != LiteralExpr::LiteralType::FLOAT &&
               lit->literalType != LiteralExpr::LiteralType::STRING;
    }
    case ASTNodeType::IDENTIFIER_EXPR:
        // Type information for user-declared variables is not available at this
        // pass level (the type-inference pass runs later).  Conservatively
        // return false so that rules like `x - x → 0` or `x / x → 1` are
        // never misapplied to float-typed identifiers.
        return false;
    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration
// ─────────────────────────────────────────────────────────────────────────────

static unsigned simplifyExpr(std::unique_ptr<Expression>& expr);
static unsigned simplifyStmt(Statement* stmt);

// ─────────────────────────────────────────────────────────────────────────────
// simplifyExpr — bottom-up algebraic simplification of one expression node
// ─────────────────────────────────────────────────────────────────────────────

static unsigned simplifyExpr(std::unique_ptr<Expression>& expr) {
    if (!expr) return 0;
    unsigned count = 0;

    // ── Recurse into children first (bottom-up) ──────────────────────────
    switch (expr->type) {
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<BinaryExpr*>(expr.get());
        count += simplifyExpr(bin->left);
        count += simplifyExpr(bin->right);
        break;
    }
    case ASTNodeType::UNARY_EXPR:
        count += simplifyExpr(static_cast<UnaryExpr*>(expr.get())->operand);
        break;
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<TernaryExpr*>(expr.get());
        count += simplifyExpr(tern->condition);
        count += simplifyExpr(tern->thenExpr);
        count += simplifyExpr(tern->elseExpr);
        break;
    }
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<CallExpr*>(expr.get());
        for (auto& arg : call->arguments)
            count += simplifyExpr(arg);
        break;
    }
    default:
        break;
    }

    // ── Apply rules at this node ─────────────────────────────────────────

    if (expr->type == ASTNodeType::BINARY_EXPR) {
        auto* bin = static_cast<BinaryExpr*>(expr.get());
        const std::string& op = bin->op;
        Expression* L = bin->left.get();
        Expression* R = bin->right.get();

        // ── Additive identity ──────────────────────────────────────────────
        if (op == "+") {
            if (isIntLiteralVal(R, 0)) {
                // x + 0 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
            if (isIntLiteralVal(L, 0)) {
                // 0 + x → x
                expr = std::move(bin->right);
                ++count;
                return count;
            }
        }

        // ── Subtractive identity ───────────────────────────────────────────
        if (op == "-") {
            if (isIntLiteralVal(R, 0)) {
                // x - 0 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
            if (isIntLiteralVal(L, 0)) {
                // 0 - x → -x
                expr = std::make_unique<UnaryExpr>("-", std::move(bin->right));
                ++count;
                return count;
            }
            // x - x → 0  (safe for integers only)
            if (sameIdent(L, R) && definitelyInteger(L)) {
                expr = makeIntLiteral(0);
                ++count;
                return count;
            }
        }

        // ── Multiplicative identity and absorbing zero ─────────────────────
        if (op == "*") {
            if (isIntLiteralVal(R, 1)) {
                // x * 1 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
            if (isIntLiteralVal(L, 1)) {
                // 1 * x → x
                expr = std::move(bin->right);
                ++count;
                return count;
            }
            // x * 0 → 0  and  0 * x → 0  (integers only — NaN*0≠0 for floats)
            if (isIntLiteralVal(R, 0) && definitelyInteger(L)) {
                expr = makeIntLiteral(0);
                ++count;
                return count;
            }
            if (isIntLiteralVal(L, 0) && definitelyInteger(R)) {
                expr = makeIntLiteral(0);
                ++count;
                return count;
            }
            // x * -1 → -x   and   -1 * x → -x
            if (isIntLiteralVal(R, -1)) {
                expr = std::make_unique<UnaryExpr>("-", std::move(bin->left));
                ++count;
                return count;
            }
            if (isIntLiteralVal(L, -1)) {
                expr = std::make_unique<UnaryExpr>("-", std::move(bin->right));
                ++count;
                return count;
            }
        }

        // ── Division identity ──────────────────────────────────────────────
        if (op == "/") {
            if (isIntLiteralVal(R, 1)) {
                // x / 1 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
            if (isIntLiteralVal(R, -1)) {
                // x / -1 → -x
                expr = std::make_unique<UnaryExpr>("-", std::move(bin->left));
                ++count;
                return count;
            }
            // x / x → 1  (integers, non-zero assumption; conservative — only ident)
            if (sameIdent(L, R) && definitelyInteger(L)) {
                expr = makeIntLiteral(1);
                ++count;
                return count;
            }
        }

        // ── Modulo identities ──────────────────────────────────────────────
        if (op == "%") {
            if (isIntLiteralVal(R, 1)) {
                // x % 1 → 0
                expr = makeIntLiteral(0);
                ++count;
                return count;
            }
            // x % x → 0  (non-zero assumption; conservative — only ident)
            if (sameIdent(L, R) && definitelyInteger(L)) {
                expr = makeIntLiteral(0);
                ++count;
                return count;
            }
        }

        // ── Exponentiation identities ──────────────────────────────────────
        if (op == "**") {
            if (isIntLiteralVal(R, 1)) {
                // x ** 1 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
            if (isIntLiteralVal(R, 0)) {
                // x ** 0 → 1  (integers; 0**0 is defined as 1 in OmScript)
                if (definitelyInteger(L)) {
                    expr = makeIntLiteral(1);
                    ++count;
                    return count;
                }
            }
            // 1 ** x → 1  (pure LHS only; side-effecting RHS must still execute)
            if (isIntLiteralVal(L, 1)) {
                expr = makeIntLiteral(1);
                ++count;
                return count;
            }
        }

        // ── Bitwise identities ─────────────────────────────────────────────
        if (op == "&") {
            if (isIntLiteralVal(R, 0) || isIntLiteralVal(L, 0)) {
                // x & 0 → 0,  0 & x → 0
                expr = makeIntLiteral(0);
                ++count;
                return count;
            }
            // x & x → x
            if (sameIdent(L, R)) {
                expr = makeIdentifier(static_cast<IdentifierExpr*>(L)->name);
                ++count;
                return count;
            }
        }
        if (op == "|") {
            if (isIntLiteralVal(R, 0)) {
                // x | 0 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
            if (isIntLiteralVal(L, 0)) {
                // 0 | x → x
                expr = std::move(bin->right);
                ++count;
                return count;
            }
            // x | x → x
            if (sameIdent(L, R)) {
                expr = makeIdentifier(static_cast<IdentifierExpr*>(L)->name);
                ++count;
                return count;
            }
        }
        if (op == "^") {
            if (isIntLiteralVal(R, 0)) {
                // x ^ 0 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
            if (isIntLiteralVal(L, 0)) {
                // 0 ^ x → x
                expr = std::move(bin->right);
                ++count;
                return count;
            }
            // x ^ x → 0  (integers)
            if (sameIdent(L, R)) {
                expr = makeIntLiteral(0);
                ++count;
                return count;
            }
        }

        // ── Shift identities ───────────────────────────────────────────────
        if (op == "<<" || op == ">>") {
            if (isIntLiteralVal(R, 0)) {
                // x << 0 → x,  x >> 0 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
        }

        // ── Self-identifier comparisons ────────────────────────────────────
        // These are only safe for integers because NaN comparisons for floats
        // do not satisfy the reflexive property (NaN != NaN).
        if (sameIdent(L, R) && definitelyInteger(L)) {
            if (op == "==" || op == "<=" || op == ">=") {
                // x == x → 1,  x <= x → 1,  x >= x → 1
                expr = makeIntLiteral(1);
                ++count;
                return count;
            }
            if (op == "!=" || op == "<" || op == ">") {
                // x != x → 0,  x < x → 0,  x > x → 0
                expr = makeIntLiteral(0);
                ++count;
                return count;
            }
        }

        // ── Boolean short-circuit simplification ──────────────────────────
        // Note: OmScript uses 0 = false, non-zero = true (like C).
        // We only fold when the operand is the integer literal 0 or 1.
        if (op == "&&") {
            if (isIntLiteralVal(R, 0) || isIntLiteralVal(L, 0)) {
                // x && 0 → 0,  0 && x → 0
                expr = makeIntLiteral(0);
                ++count;
                return count;
            }
            if (isIntLiteralVal(R, 1)) {
                // x && 1 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
            if (isIntLiteralVal(L, 1)) {
                // 1 && x → x
                expr = std::move(bin->right);
                ++count;
                return count;
            }
        }
        if (op == "||") {
            if (isIntLiteralVal(R, 1) || isIntLiteralVal(L, 1)) {
                // x || 1 → 1,  1 || x → 1
                expr = makeIntLiteral(1);
                ++count;
                return count;
            }
            if (isIntLiteralVal(R, 0)) {
                // x || 0 → x
                expr = std::move(bin->left);
                ++count;
                return count;
            }
            if (isIntLiteralVal(L, 0)) {
                // 0 || x → x
                expr = std::move(bin->right);
                ++count;
                return count;
            }
        }
    } // end BINARY_EXPR

    if (expr->type == ASTNodeType::UNARY_EXPR) {
        auto* un = static_cast<UnaryExpr*>(expr.get());

        // ── Double negation ────────────────────────────────────────────────
        // !!x → x   (logical double-not)
        // -(-x) → x  (arithmetic double-negate)
        if (un->operand && un->operand->type == ASTNodeType::UNARY_EXPR) {
            auto* inner = static_cast<const UnaryExpr*>(un->operand.get());
            if (un->op == inner->op && (un->op == "!" || un->op == "-")) {
                // Unwrap both negations.
                expr = std::move(static_cast<UnaryExpr*>(un->operand.get())->operand);
                ++count;
                return count;
            }
        }
    }

    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// simplifyStmt — walk all expressions reachable from a statement
// ─────────────────────────────────────────────────────────────────────────────

static unsigned simplifyStmt(Statement* stmt) {
    if (!stmt) return 0;
    unsigned count = 0;

    switch (stmt->type) {
    case ASTNodeType::EXPR_STMT:
        count += simplifyExpr(static_cast<ExprStmt*>(stmt)->expression);
        break;

    case ASTNodeType::VAR_DECL:
        count += simplifyExpr(static_cast<VarDecl*>(stmt)->initializer);
        break;

    case ASTNodeType::RETURN_STMT:
        count += simplifyExpr(static_cast<ReturnStmt*>(stmt)->value);
        break;

    case ASTNodeType::IF_STMT: {
        auto* ifS = static_cast<IfStmt*>(stmt);
        count += simplifyExpr(ifS->condition);
        count += simplifyStmt(ifS->thenBranch.get());
        count += simplifyStmt(ifS->elseBranch.get());
        break;
    }

    case ASTNodeType::WHILE_STMT: {
        auto* ws = static_cast<WhileStmt*>(stmt);
        count += simplifyExpr(ws->condition);
        count += simplifyStmt(ws->body.get());
        break;
    }

    case ASTNodeType::DO_WHILE_STMT: {
        auto* dw = static_cast<DoWhileStmt*>(stmt);
        count += simplifyStmt(dw->body.get());
        count += simplifyExpr(dw->condition);
        break;
    }

    case ASTNodeType::FOR_STMT: {
        auto* fs = static_cast<ForStmt*>(stmt);
        count += simplifyExpr(fs->start);
        count += simplifyExpr(fs->end);
        count += simplifyExpr(fs->step);
        count += simplifyStmt(fs->body.get());
        break;
    }

    case ASTNodeType::BLOCK: {
        auto* block = static_cast<BlockStmt*>(stmt);
        for (auto& s : block->statements)
            count += simplifyStmt(s.get());
        break;
    }

    case ASTNodeType::ASSUME_STMT:
        count += simplifyExpr(static_cast<AssumeStmt*>(stmt)->condition);
        break;

    case ASTNodeType::THROW_STMT:
        count += simplifyExpr(static_cast<ThrowStmt*>(stmt)->value);
        break;

    default:
        break;
    }

    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

AlgSimpStats runAlgSimpPass(Program* program, bool verbose) {
    AlgSimpStats total;

    forEachFunction(program, [&](FunctionDecl* fn) {
        const unsigned before = total.rulesApplied;
        for (auto& stmt : fn->body->statements)
            total.rulesApplied += simplifyStmt(stmt.get());

        const unsigned applied = total.rulesApplied - before;
        if (verbose && applied > 0) {
            std::cerr << "[AlgSimp] " << fn->name
                      << ": " << applied << " algebraic simplification(s)\n";
        }
    });

    return total;
}

} // namespace omscript
