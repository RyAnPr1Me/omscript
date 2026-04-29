#pragma once
#ifndef CONST_FOLD_PASS_H
#define CONST_FOLD_PASS_H

/// @file const_fold_pass.h
/// @brief Constant Folding (ConstFold) pass for the OmScript compiler.
///
/// The ConstFold pass evaluates binary and unary operations whose operands are
/// compile-time integer or boolean literals, replacing the expression with a
/// single literal result.  It runs entirely on the AST, without the full
/// cross-function virtual-machine overhead of CFCTRE.
///
/// ## What ConstFold does
///
///   Binary integer arithmetic (both operands are INTEGER literals):
///     2 + 3  → 5         10 - 4  → 6
///     3 * 4  → 12        8 / 2   → 4    (integer division; skips div-by-zero)
///     7 % 3  → 1         2 ** 8  → 256
///     6 & 3  → 2         6 | 3   → 7
///     5 ^ 3  → 6         1 << 4  → 16
///     16 >> 2 → 4
///
///   Binary integer comparisons (both operands are INTEGER literals):
///     2 == 2 → 1 (true)  2 != 3 → 1    2 < 3 → 1
///     4 <= 4 → 1         5 > 3  → 1    3 >= 5 → 0 (false)
///
///   Binary boolean short-circuits (both operands are INTEGER 0/non-zero):
///     1 && 1 → 1         0 || 0 → 0    etc.
///
///   Unary operations on INTEGER literals:
///     -5     → -5 (literal)      ~3 → -4 (bitwise NOT)
///     !0     → 1 (logical NOT)   !7 → 0
///
/// ## What ConstFold does NOT do
///   - Floating-point folding: IEEE-754 rounding is hard to replicate exactly
///     in a portable compiler pass; CFCTRE handles float fold via the C++
///     runtime if needed.
///   - String / array literal folding: handled by CFCTRE.
///   - Cross-statement analysis: ConstFold is expression-local; it sees only
///     the literal values present in the syntactic expression tree.
///   - Overflow / wrapping semantics: arithmetic is performed in int64_t,
///     so results that overflow int64_t are silently correct for 64-bit
///     semantics.  Narrower-width wrapping (e.g. i8 overflow) is NOT applied.
///
/// ## Relationship to other passes
///   ConstFold complements AlgSimp and CopyProp:
///   - AlgSimp eliminates identity/absorbing-element patterns (x*0→0, x+0→x).
///   - CopyProp substitutes alias copies (var y=x; f(y) → f(x)).
///   - ConstFold then folds any remaining literal arithmetic that AlgSimp or
///     CopyProp produced.  Running the three passes in order converges quickly
///     on a simplified normal form without needing the e-graph.
///
///   ConstFold runs BEFORE CFCTRE so that CFCTRE sees fewer redundant nodes,
///   reducing its internal evaluation cost.

#include "ast.h"

namespace omscript {

/// Per-pass statistics returned from runConstFoldPass().
struct ConstFoldStats {
    unsigned expressionsFolded = 0; ///< Number of compile-time expressions replaced
};

/// Run the Constant Folding pass over every function in @p program.
///
/// @param program  The AST to analyse and transform (modified in place).
/// @param verbose  When true, print a per-function summary to stderr.
/// @returns        Aggregate statistics across all functions.
ConstFoldStats runConstFoldPass(Program* program, bool verbose = false);

} // namespace omscript

#endif // CONST_FOLD_PASS_H
