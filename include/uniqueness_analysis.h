#pragma once
#ifndef UNIQUENESS_ANALYSIS_H
#define UNIQUENESS_ANALYSIS_H

/// @file uniqueness_analysis.h
/// @brief Ownership-Aware Uniqueness Analysis for OmScript.
///
/// ## Core concept: Temporal Uniqueness
///
/// A string or array variable is *temporally unique* if no other live alias
/// exists to the same underlying heap buffer during a given program region.
/// This is stronger than "owned" — it means:
///   - refcount == 1 (logically, even if not materialised)
///   - no borrow references are active
///   - no future alias creation in the live region
///
/// ## Algorithm (flow-insensitive, sound)
///
/// The analysis makes a single forward scan over the function AST and marks
/// a variable as *Shared* (not unique) whenever it appears in an *aliasing
/// position*:
///
///   1. `var y = x`       — both x and y are Shared (y aliases x's buffer)
///   2. `y = x`           — same (when both are string/array vars)
///   3. `f(x)` arg pos    — x is Shared if f may capture the pointer
///   4. `arr[i] = x`      — x is Shared (stored into array element)
///
/// Variables NOT in an aliasing position stay Unique:
///   - string literals, binary-concat results, known allocating builtins
///   - assignment from a fresh expression (`var x = s + t`, `var x = ""`)
///   - return statements (ownership transfer — does not create a second ref)
///
/// ## Conservatism
///
/// The analysis is sound but not maximal — it may mark variables Shared when
/// they are truly Unique (false negatives lead to missed optimisations, never
/// to wrong code).  User-defined function calls conservatively treat their
/// string/array arguments as potentially captured (Shared).
///
/// ## Usage
///
/// ```cpp
/// UniquenessAnalysis ua = computeUniqueness(fn, knownStrVars, knownArrVars);
/// if (ua.isUnique("result")) {
///     // can use realloc+append instead of malloc+copy+copy
/// }
/// ```
///
/// ## Optimization targets enabled
///
/// 1. **In-place string append**: `x = x + y` (x unique)
///       → realloc(x) + memcpy(y only)  — saves O(len(x)) work per iteration
///       — turns string-building loops from O(n²) to O(n)
///
/// 2. **LLVM IR metadata**: fresh string/array allocations receive
///       `!omsc_unique` metadata so downstream passes can exploit uniqueness.
///
/// 3. **noalias parameters**: string/array parameters proven unique at all
///       call sites receive the `noalias` IR attribute, allowing LLVM AA to
///       prove inter-parameter non-aliasing.

#include "ast.h"
#include "opt_context.h"  // BuiltinEffectTable

#include <string>
#include <unordered_set>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// UniquenessAnalysis — per-function result
// ─────────────────────────────────────────────────────────────────────────────

/// Per-function uniqueness analysis result.
///
/// Contains the set of string/array variable names that are provably
/// *temporally unique* throughout the function — meaning no aliasing operation
/// was found that could create a second live reference to the same buffer.
struct UniquenessAnalysis {
    /// Names of string/array variables that are provably unique.
    /// Variables NOT in this set are either Shared or unknown.
    std::unordered_set<std::string> uniqueVars;

    /// Returns true if @p varName is provably unique (no live aliases).
    bool isUnique(const std::string& varName) const noexcept {
        return uniqueVars.count(varName) > 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// computeUniqueness
// ─────────────────────────────────────────────────────────────────────────────

/// Compute the uniqueness analysis for function @p fn.
///
/// @param fn          The function to analyse.
/// @param strVarHints Names already known to be string variables (from
///                    pre-analysis type propagation).  The analysis also
///                    discovers additional string variables from literal and
///                    builtin-call initialisers.
/// @param arrVarHints Names already known to be array variables.
///
/// @returns A `UniquenessAnalysis` whose `uniqueVars` contains every
///          string/array variable that is provably unique throughout @p fn.
///          Variables with unknown uniqueness are absent from `uniqueVars`.
UniquenessAnalysis computeUniqueness(
    const FunctionDecl&                    fn,
    const std::unordered_set<std::string>& strVarHints,
    const std::unordered_set<std::string>& arrVarHints);

} // namespace omscript

#endif // UNIQUENESS_ANALYSIS_H
