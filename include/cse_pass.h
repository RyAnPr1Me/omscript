#pragma once
#ifndef CSE_PASS_H
#define CSE_PASS_H

/// @file cse_pass.h
/// @brief Common Subexpression Elimination (CSE) pass for the OmScript compiler.
///
/// The CSE pass detects pure subexpressions that are computed more than once
/// within the same block and replaces the repeated computations with references
/// to a single compiler-generated temporary variable.
///
/// **What is considered a CSE candidate?**
///   - Binary expressions whose both operands are identifiers or integer
///     literals (e.g. `a + b`, `x * 2`).  These are the innermost "atoms"
///     of larger expressions and are cheapest to hoist.
///   - The expression must not involve any side-effecting operation:
///     function calls and assignments are excluded by default.
///   - With ERSL enabled: call expressions targeting functions whose
///     EffectSummary has canDuplicate=true are also eligible.  This covers
///     user-defined functions that are stable and produce no observable write
///     effects (e.g. pure math helpers).
///   - The expression must appear at the *statement level* (inside ExprStmt,
///     VarDecl initializer, ReturnStmt value, or IfStmt condition) in the
///     **same** block — CSE does not cross block boundaries.
///
/// **What is *not* done:**
///   - Hoisting across basic-block boundaries or loop headers (conservative).
///   - CSE of expressions containing sub-expressions with side effects.
///   - Alias analysis for array/struct accesses.
///
/// The pass runs per-function, per-block.  Nested blocks are each processed
/// independently.

#include "ast.h"
#include "ersl.h"

#include <string>
#include <unordered_map>

namespace omscript {

/// Per-pass statistics returned from runCSEPass().
struct CSEStats {
    unsigned expressionsHoisted = 0; ///< Pure subexpressions replaced by a temp var
    unsigned tempVarsIntroduced = 0; ///< Number of new `var _cse_N = ...` declarations
};

/// Run the Common Subexpression Elimination pass over every function in
/// @p program.
///
/// @param program         The AST to analyse and transform (modified in place).
/// @param verbose         When true, print a per-function summary to stderr.
/// @param idempotentFuncs Optional map: function name → EffectSummary.
///                        When provided, call expressions targeting functions
///                        whose EffectSummary::canDuplicate is true are
///                        treated as CSE candidates in addition to pure binary
///                        expressions.  Pass nullptr to use the default
///                        binary-only behaviour.
/// @returns               Aggregate statistics across all functions.
CSEStats runCSEPass(Program* program,
                    bool verbose = false,
                    const std::unordered_map<std::string, EffectSummary>* idempotentFuncs = nullptr);

} // namespace omscript

#endif // CSE_PASS_H
