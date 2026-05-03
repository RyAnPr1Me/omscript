#pragma once
#ifndef COPY_PROP_PASS_H
#define COPY_PROP_PASS_H

/// @file copy_prop_pass.h
/// @brief Copy Propagation (CopyProp) pass for the OmScript compiler.
///
/// The CopyProp pass eliminates trivial variable aliases of the form:
///
///     var y = x;
///
/// where `x` is a simple identifier (not a complex expression), by inlining
/// `x` directly at every subsequent use of `y` within the same block.
///
/// ## Algorithm (intra-block, forward dataflow)
///
///   For each statement sequence (block) in a function body:
///   1. Maintain a *copy map*: `name → source_name`.
///   2. Walk statements left-to-right:
///        a. If the statement is `var y = x` (IdentifierExpr RHS):
///             - Record `y → x` in the copy map.
///        b. If the statement is a `var y = <non-ident>` or any assignment
///           that writes to `y` (AssignExpr / prefix/postfix on `y`):
///             - Remove `y` from the copy map (lhs killed).
///             - Also remove any entries that depended transitively: any entry
///               `z → y` must also be invalidated.
///        c. If the source `x` of a copy `y → x` is itself overwritten, the
///           copy `y → x` is also killed.
///        d. Before recording or replacing, apply the copy map transitively:
///           `y → x` where `x → w` in the map becomes `y → w`, giving
///           *transitive* propagation in a single pass.
///   3. While walking expressions inside each statement, replace every
///      IdentifierExpr whose name is a key in the copy map with a fresh
///      IdentifierExpr for the mapped source name.
///   4. Recurse into nested blocks with a fresh, empty copy map (conservative:
///      avoids unsound propagation across control-flow joins).
///
/// ## What CopyProp enables downstream
///   - Further CFCTRE constant folding: if `y = SOME_CONST; f(y)` becomes
///     `f(SOME_CONST)`, the constant-folding engine can evaluate the call.
///   - AlgSimp rule matching: `x + y` where `y = x` becomes `x + x`, which
///     triggers the `x + x → 2*x` e-graph rule and the `x ^ x → 0` AlgSimp rule.
///   - Reduced register pressure: eliminates redundant copies in the generated IR.
///
/// ## Safety invariant
///   CopyProp never increases observable behaviour.  The only case that could
///   be unsound is if `x` is re-assigned between the copy definition and the
///   use — which is handled by step (c) above.

#include "ast.h"

namespace omscript {

/// Per-pass statistics returned from runCopyPropPass().
struct CopyPropStats {
    unsigned copiesEliminated = 0; ///< Number of identifier-identifier copies inlined
};

/// Run the Copy Propagation pass over every function in @p program.
///
/// @param program  The AST to analyse and transform (modified in place).
/// @param verbose  When true, print a per-function summary to stderr.
/// @returns        Aggregate statistics across all functions.
CopyPropStats runCopyPropPass(Program* program, bool verbose = false);

} // namespace omscript

#endif // COPY_PROP_PASS_H
