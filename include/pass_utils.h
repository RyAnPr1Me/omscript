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
#include "ast_arena.h"  // StringHash, StringEqual, BumpAllocator

#include <algorithm>
#include <cctype>
#include <memory>
#include <string_view>

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
// AST node factories
// ─────────────────────────────────────────────────────────────────────────────

/// Create a new identifier expression with name @p name.
inline std::unique_ptr<Expression> makeIdentifier(const std::string& name) {
    return std::make_unique<IdentifierExpr>(name);
}

/// Create a new binary expression @p lhs @p op @p rhs.
inline std::unique_ptr<Expression> makeBinary(const std::string& op,
                                               std::unique_ptr<Expression> lhs,
                                               std::unique_ptr<Expression> rhs) {
    return std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs));
}

/// Create a new unary expression @p op @p operand.
inline std::unique_ptr<Expression> makeUnary(const std::string& op,
                                              std::unique_ptr<Expression> operand) {
    return std::make_unique<UnaryExpr>(op, std::move(operand));
}

// ─────────────────────────────────────────────────────────────────────────────
// Typed-cast helpers
// ─────────────────────────────────────────────────────────────────────────────

/// If @p expr is an IdentifierExpr, return the cast pointer; otherwise nullptr.
inline IdentifierExpr* asIdentifier(Expression* expr) noexcept {
    if (!expr || expr->type != ASTNodeType::IDENTIFIER_EXPR) return nullptr;
    return static_cast<IdentifierExpr*>(expr);
}

/// Const overload.
inline const IdentifierExpr* asIdentifier(const Expression* expr) noexcept {
    if (!expr || expr->type != ASTNodeType::IDENTIFIER_EXPR) return nullptr;
    return static_cast<const IdentifierExpr*>(expr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Compiler-generated VarDecl insertion
// ─────────────────────────────────────────────────────────────────────────────

/// Insert a compiler-generated `var @p name = <init>` declaration at position
/// @p insertPos in @p block's statement list.  The declaration is marked
/// `isCompilerGenerated = true` and is `const` by default.
///
/// Safe to call when @p block is null or @p insertPos is out of range
/// (clamped to block->statements.size()).
inline void insertCompilerVarDecl(BlockStmt* block,
                                   size_t insertPos,
                                   const std::string& name,
                                   std::unique_ptr<Expression> init,
                                   bool isConst = true) {
    if (!block) return;
    auto decl = std::make_unique<VarDecl>(name, std::move(init), isConst, /*type=*/"");
    decl->isCompilerGenerated = true;
    const size_t clampedPos = std::min(insertPos, block->statements.size());
    block->statements.insert(
        block->statements.begin() + static_cast<ptrdiff_t>(clampedPos),
        std::move(decl));
}

// ─────────────────────────────────────────────────────────────────────────────
// Type-name predicates (shared between Parser, Codegen, and analysis passes)
// ─────────────────────────────────────────────────────────────────────────────

/// Return true when @p name is an integer width-cast intrinsic of the form
/// `iN` or `uN` (N in [1..256]) — e.g. `i8`, `u32`, `i128`.
///
/// This is the canonical implementation used by both the Parser (to
/// disambiguate type annotations) and the BuiltinEffectTable (for purity
/// classification of width-cast calls).  Replaces the previously scattered
/// local copies in parser.cpp and opt_context.h.
inline bool isIntWidthTypeName(std::string_view name) noexcept {
    if (name.size() < 2) return false;
    if (name[0] != 'i' && name[0] != 'u') return false;
    int bw = 0;
    for (size_t j = 1; j < name.size(); ++j) {
        if (name[j] < '0' || name[j] > '9') return false;
        bw = bw * 10 + (name[j] - '0');
        if (bw > 256) return false;
    }
    return bw >= 1 && bw <= 256;
}

/// Return true when @p name is one of the built-in OmScript scalar / aggregate
/// type keywords, OR is an integer width-cast type name.
///
/// Used by the Parser to decide whether `identifier:name` is a type annotation
/// (colon is part of the annotation) vs. a ternary colon (expression context).
inline bool isKnownScalarTypeName(std::string_view name) noexcept {
    if (name == "int"    || name == "float"  || name == "double" ||
        name == "bool"   || name == "string" || name == "dict"   ||
        name == "bigint" || name == "ptr")
        return true;
    return isIntWidthTypeName(name);
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
