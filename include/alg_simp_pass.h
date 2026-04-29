#pragma once
#ifndef ALG_SIMP_PASS_H
#define ALG_SIMP_PASS_H

/// @file alg_simp_pass.h
/// @brief Algebraic Simplification (AlgSimp) pass for the OmScript compiler.
///
/// The AlgSimp pass applies structural algebraic identity rules directly on
/// the AST, without requiring the full e-graph equality-saturation machinery.
/// It is designed to run early in the pipeline (after CFCTRE has folded
/// constants, before the e-graph for further optimization at O2+).
///
/// **Rules applied (bottom-up per expression):**
///
///   Identity elements:
///     x + 0  → x          0 + x  → x
///     x - 0  → x
///     x * 1  → x          1 * x  → x
///     x / 1  → x
///     x ** 1 → x          x ** 0 → 1
///     x & ~0 → x          x | 0  → x
///     x ^ 0  → x          0 ^ x  → x
///     x << 0 → x          x >> 0 → x
///
///   Absorbing elements:
///     x * 0  → 0          0 * x  → 0
///     x & 0  → 0          0 & x  → 0
///     x | ~0 → ~0  (skipped — ~0 has no literal form in OmScript)
///
///   Boolean short-circuits (integer 0 = false, non-zero = true):
///     x && 0 → 0          0 && x → 0
///     x && 1 → x          1 && x → x
///     x || 0 → x          0 || x → x
///     x || 1 → 1          1 || x → 1
///
///   Self-cancellation (only when both operands are the same identifier):
///     x - x  → 0
///     x ^ x  → 0
///     x / x  → 1   (only for integer type; excluded for floats — NaN/Inf)
///
///   Double negation:
///     !(!x)  → x          -(-x) → x
///
/// **What AlgSimp does NOT do:**
///   - Constant folding (handled by CFCTRE and the e-graph).
///   - Cross-statement rewriting (it is expression-local).
///   - Float arithmetic (IEEE-754 semantics make identity rules unsound for
///     floats: e.g. NaN * 0 ≠ 0, (x + 0.0) may change sign of -0.0).
///     Float literals can be distinguished by LiteralExpr::LiteralType::FLOAT.
///
/// The pass recurses into all statement types that contain expressions.

#include "ast.h"

namespace omscript {

/// Per-pass statistics returned from runAlgSimpPass().
struct AlgSimpStats {
    unsigned rulesApplied = 0; ///< Total number of algebraic simplifications made
};

/// Run the Algebraic Simplification pass over every function in @p program.
///
/// @param program  The AST to analyse and transform (modified in place).
/// @param verbose  When true, print a per-function summary to stderr.
/// @returns        Aggregate statistics across all functions.
AlgSimpStats runAlgSimpPass(Program* program, bool verbose = false);

} // namespace omscript

#endif // ALG_SIMP_PASS_H
