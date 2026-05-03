#pragma once
#ifndef PASS_UTILS_H
#define PASS_UTILS_H

/// @file pass_utils.h
/// @brief Shared utilities for AST-level optimization passes.
///
/// Provides small, frequently-needed helpers used by all pass implementations:
///
///   isIntLiteral    — test whether an expression is an integer literal and
///                     optionally retrieve its value.
///   isIntLiteralVal — test whether an expression is an integer literal equal
///                     to a specific value.
///   makeIntLiteral  — construct a new integer literal expression node.
///   forEachFunction — iterate over all concrete (non-null, body-having)
///                     functions in a Program.
///
/// These helpers replace a family of near-identical local definitions that
/// had proliferated across dce_pass.cpp, alg_simp_pass.cpp, cse_pass.cpp,
/// width_opt_pass.cpp, and opt_orchestrator.cpp.

#include "ast.h"

#include <memory>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Integer-literal predicates
// ─────────────────────────────────────────────────────────────────────────────

/// Return true if @p expr is an integer literal.
///
/// When @p value is non-null and the expression is an integer literal, the
/// literal's value is written to *value.
inline bool isIntLiteral(const Expression* expr,
                          long long* value = nullptr) noexcept {
    if (!expr || expr->type != ASTNodeType::LITERAL_EXPR) return false;
    const auto* lit = static_cast<const LiteralExpr*>(expr);
    if (lit->literalType != LiteralExpr::LiteralType::INTEGER) return false;
    if (value) *value = lit->intValue;
    return true;
}

/// Return true if @p expr is an integer literal whose value equals @p val.
inline bool isIntLiteralVal(const Expression* expr, long long val) noexcept {
    long long v = 0;
    return isIntLiteral(expr, &v) && v == val;
}

// ─────────────────────────────────────────────────────────────────────────────
// Integer-literal factory
// ─────────────────────────────────────────────────────────────────────────────

/// Create a new integer literal expression with value @p v.
inline std::unique_ptr<Expression> makeIntLiteral(long long v) {
    return std::make_unique<LiteralExpr>(v);
}

// ─────────────────────────────────────────────────────────────────────────────
// Function iteration
// ─────────────────────────────────────────────────────────────────────────────

/// Call @p callback(fn) for every non-null, body-having FunctionDecl in
/// @p program.  Safe to call with a null @p program (does nothing).
///
/// @p callback receives a raw FunctionDecl* and may mutate the function.
template<typename Fn>
void forEachFunction(Program* program, Fn&& callback) {
    if (!program) return;
    for (auto& fn : program->functions) {
        if (!fn || !fn->body) continue;
        callback(fn.get());
    }
}

/// Const overload — @p callback receives a `const FunctionDecl*`.
template<typename Fn>
void forEachFunction(const Program* program, Fn&& callback) {
    if (!program) return;
    for (const auto& fn : program->functions) {
        if (!fn || !fn->body) continue;
        callback(fn.get());
    }
}

} // namespace omscript

#endif // PASS_UTILS_H
