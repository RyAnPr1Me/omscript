#pragma once
#ifndef RLC_PASS_H
#define RLC_PASS_H

/// @file rlc_pass.h
/// @brief Region Lifetime Coalescing (RLC) pass for the OmScript compiler.
///
/// The RLC pass exploits explicit memory regions in OmScript programs.
/// It looks for pairs of region variables whose lifetimes are disjoint
/// (the first is `invalidate`d before the second is created) and merges
/// them into a single region, eliminating redundant allocations.
///
/// Safety is guaranteed by the `invalidate` keyword: the programmer
/// explicitly marks the end of each region's lifetime.  The pass verifies
/// that no reference to the invalidated region survives past the invalidate
/// point, and emits E014 if a violation is found.
///
/// Diagnostic E013 is emitted when a region variable (created via
/// `newRegion()`) is neither `invalidate`d nor returned before the
/// function ends.

#include "ast.h"
#include <string>

namespace omscript {

/// Per-pass statistics returned from runRLCPass().
struct RLCStats {
    unsigned regionsCoalesced = 0;   ///< Number of (R1, R2) pairs successfully coalesced
    unsigned allocsRedirected = 0;   ///< Number of alloc() calls redirected from R2 to R1
    unsigned invalidatesRemoved = 0; ///< Number of early `invalidate` stmts removed
};

/// Run the Region Lifetime Coalescing pass over every function in @p program.
///
/// @param program   The AST to analyse and transform (modified in place).
/// @param verbose   When true, print a summary of coalescing decisions.
/// @returns         Aggregate statistics across all functions.
///
/// @throws DiagnosticError (E013) when a region variable has no invalidate.
/// @throws DiagnosticError (E014) when a region variable is used after invalidate.
RLCStats runRLCPass(Program* program, bool verbose = false);

} // namespace omscript

#endif // RLC_PASS_H
