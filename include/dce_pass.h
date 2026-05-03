#pragma once
#ifndef DCE_PASS_H
#define DCE_PASS_H

/// @file dce_pass.h
/// @brief Dead Code Elimination (DCE) pass for the OmScript compiler.
///
/// The DCE pass removes unreachable code that arises from constant conditions.
/// It operates on the AST and applies three transformations:
///
///   1. **Constant-condition if-elimination**: When an `if` statement's
///      condition is a compile-time integer literal, the taken branch is
///      substituted for the entire `if` statement; the dead branch is dropped.
///
///   2. **Dead-loop elimination**: A `while (0)` loop (or any while loop whose
///      condition is the literal 0) is removed entirely.  Similarly, a
///      `do { ... } while (0)` is replaced by its body (it executes once).
///
///   3. **Unreachable-after-return pruning**: Statements following an
///      unconditional `return` within the same block are removed, since they
///      can never execute.
///
/// The pass is sound: it only removes code that is provably dead.
/// It does **not** perform:
///   - Constant propagation (handled by CFCTRE and the e-graph).
///   - Removal of unused variable declarations (variables may be observed by
///     a debugger or have non-trivial destructors in future language versions).

#include "ast.h"

namespace omscript {

/// Per-pass statistics returned from runDCEPass().
struct DCEStats {
    unsigned deadIfBranches   = 0; ///< `if` statements replaced by a single branch
    unsigned deadLoops        = 0; ///< While loops removed (condition is literal 0)
    unsigned unreachableStmts = 0; ///< Statements pruned after unconditional return
};

/// Run the Dead Code Elimination pass over every function in @p program.
///
/// @param program   The AST to analyse and transform (modified in place).
/// @param verbose   When true, print a per-function summary to stderr.
/// @returns         Aggregate statistics across all functions.
DCEStats runDCEPass(Program* program, bool verbose = false);

} // namespace omscript

#endif // DCE_PASS_H
