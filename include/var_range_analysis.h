#pragma once
#ifndef VAR_RANGE_ANALYSIS_H
#define VAR_RANGE_ANALYSIS_H

/// @file var_range_analysis.h
/// @brief Intra-function integer value-range analysis for OmScript.
///
/// This pass computes a conservative closed-interval range [lo, hi] for every
/// local variable in a function body using **forward dataflow**.
///
/// Algorithm
/// ─────────
/// The analysis performs a single forward pass over each statement list,
/// maintaining a `VarRangeMap` (variable name → ValueRange) that is updated
/// as definitions are encountered:
///
///   var x = 42         → x ∈ [42, 42]
///   var x = y + 1      → x ∈ [lo(y)+1, hi(y)+1]   (when y is known)
///   var x = len(arr)   → x ∈ [0, INT64_MAX]
///   var x = abs(v)     → x ∈ [0, INT64_MAX]
///   var x = a & mask   → x ∈ [0, mask]             (mask ≥ 0 literal)
///   var x = a % M      → x ∈ [0, M-1]              (M > 0 literal, a ≥ 0)
///   var x = a >> N     → x ∈ [0, hi(a)>>N]         (logical shift)
///   var x = clamp(…,l,h) → x ∈ [l, h]
///   for i in s..e      → i ∈ [min(s,e), max(s,e)-1] (when s,e are literals)
///   @range[lo,hi] expr → use the annotation directly
///
/// Branch handling
/// ───────────────
/// For `if (cond) { T } else { E }`, the analysis:
///   1. Evaluates a *narrowed* environment for the then-branch by tightening
///      bounds from the condition (e.g. `if (x < 10)` → x.hi = min(x.hi, 9)).
///   2. Evaluates a separate narrowed environment for the else-branch.
///   3. Joins the two post-branch environments: each variable gets the union
///      range (widest bound covering both outcomes).
///
/// Loop handling
/// ─────────────
/// Variables written anywhere inside a loop body are **invalidated** (removed
/// from the map) before the body is recursed — ensuring we never assert a
/// range that the loop might violate.  The loop induction variable itself
/// gets a range derived from the start/end expressions.
///
/// Consumers
/// ─────────
/// The map is used by:
///   • AlgSimpPass  — `definitelyNonNeg(expr, ranges)` enables strength
///                     reduction and comparison folding.
///   • DCEPass      — range-based branch elimination (always-true/false cond).
///   • WidthOptPass — narrower range → fewer significant bits → smaller type.

#include "ast.h"
#include "opt_context.h"  // ValueRange
#include "pass_utils.h"   // isIntLiteral, asIdentifier

#include <optional>
#include <string>
#include <unordered_map>

namespace omscript {

/// Map from variable name → known value range within a function.
using VarRangeMap = std::unordered_map<std::string, ValueRange>;

/// Evaluate the value range of @p expr given the current variable environment.
/// Returns `nullopt` when no bound can be proved.
///
/// The result is always conservative: if the true range cannot be narrowed
/// below [INT64_MIN, INT64_MAX], `nullopt` is returned rather than a trivially
/// wide range (callers can treat absence as "unknown / full range").
std::optional<ValueRange> evalExprRange(const Expression* expr,
                                         const VarRangeMap& env);

/// Compute intra-function variable ranges by forward dataflow over @p fn.
///
/// Returns a `VarRangeMap` containing only variables whose ranges have been
/// successfully narrowed (i.e., `isNarrowed()` is true).  Variables with
/// unknown range are absent from the result, so callers may treat absence as
/// [INT64_MIN, INT64_MAX].
VarRangeMap computeVarRanges(const FunctionDecl& fn);

} // namespace omscript

#endif // VAR_RANGE_ANALYSIS_H
