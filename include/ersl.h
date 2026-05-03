#pragma once
#ifndef ERSL_H
#define ERSL_H

/// @file ersl.h
/// @brief Effect Refinement & Speculation Layer (ERSL) for the OmScript optimizer.
///
/// ERSL is a precision amplifier and safety oracle that sits on top of the
/// existing FunctionEffects system.  It does not introduce a new IR or a
/// separate optimizer pass; instead it enriches the existing effect summary
/// with four new orthogonal dimensions:
///
///   Region    — where does the effect touch memory?
///   Stability — do repeated calls return the same result?
///   EscapeClass — does the computed value leave the local scope?
///   EffectKind  — what category of effect is this?
///
/// Together these let every pass answer:
///   "How far can I push this transformation without risking a miscompile?"
///
/// ## Effect compatibility
///
/// `effectsConflict(A, B)` implements the four-rule conflict matrix:
///   1. Different memory regions → no conflict.
///   2. Both reads (no writes) → no conflict.
///   3. At least one write to the same region → conflict.
///   4. I/O in the same domain with ordering required → conflict.
///
/// ## Maximum safe optimization level
///
/// `getMaxSafeOptLevel(es)` maps an EffectSummary to an integer 1–5:
///   5 — PURE  (full speculation, duplication, hoisting, CSE)
///   4 — READONLY
///   3 — WRITE to local / non-escaping region only
///   2 — WRITE to global / arg-escaping region
///   1 — IO / External (strict ordering required)
///
/// ## Integration points
///
/// The orchestrator runs the `ersl` pass after the `effects` pass and stores
/// one EffectSummary per function inside FunctionFacts.  Downstream passes
/// consume it via OptimizationContext::effectSummary(funcName).
///
///   LICM     : if (es.isStable && !es.hasIO)  hoist invariant computation
///   CSE      : if (es.isIdempotent)            eliminate duplicate calls
///   Inliner  : if (es.hasIO)                  avoid aggressive inlining
///   Vectorizer: if (no conflicting writes)     mark loop.independent

#include "ast.h"  // FunctionEffects

#include <cstdint>
#include <string>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// EffectKind — the category of a memory / observable side-effect
// ─────────────────────────────────────────────────────────────────────────────
enum class EffectKind : uint8_t {
    None  = 0, ///< No observable effect (e.g. pure arithmetic)
    Read  = 1, ///< Reads from memory
    Write = 2, ///< Writes to memory
    IO    = 3, ///< Performs I/O (network, file, console, sleep …)
};

// ─────────────────────────────────────────────────────────────────────────────
// Region — the memory region touched by a side-effect
// ─────────────────────────────────────────────────────────────────────────────
enum class Region : uint8_t {
    None    = 0, ///< No memory is accessed (pure computation)
    Local   = 1, ///< Stack-allocated / function-local storage
    Arg     = 2, ///< Memory reachable through a function argument
    Global  = 3, ///< Module-level global variable
    Heap    = 4, ///< Heap-allocated memory with unknown aliasing
    Net     = 5, ///< Network (HTTP, sockets)
    Unknown = 6, ///< Unknown / conservative worst-case
};

// ─────────────────────────────────────────────────────────────────────────────
// Stability — whether repeating an operation yields the same result
// ─────────────────────────────────────────────────────────────────────────────
enum class Stability : uint8_t {
    Stable         = 0, ///< Same inputs always produce the same output (pure math,
                        ///  pure user functions).  Safe to CSE and hoist.
    InputDependent = 1, ///< Result depends on mutable memory visible to the caller
                        ///  (e.g. load from an array).  Safe within a single
                        ///  execution path if no interleaved write is present.
    External       = 2, ///< Depends on the outside world (I/O, clocks, RNG).
                        ///  Never safe to duplicate or hoist speculatively.
    Unknown        = 3, ///< Cannot determine — conservative: treat as External.
};

// ─────────────────────────────────────────────────────────────────────────────
// EscapeClass — whether a computed value leaves the local scope
// ─────────────────────────────────────────────────────────────────────────────
enum class EscapeClass : uint8_t {
    NoEscape    = 0, ///< Value stays within the function (local variable, temp).
    ArgEscape   = 1, ///< Value may be observable via a call argument.
    GlobalEscape = 2, ///< Value may be observable globally (stored in global / returned).
};

// ─────────────────────────────────────────────────────────────────────────────
// RefinedEffect — the extended per-effect descriptor
// ─────────────────────────────────────────────────────────────────────────────
///
/// A single RefinedEffect describes one observable effect of a function.
/// Functions often have multiple effects (e.g. reads arg memory AND writes
/// a local); callers typically work with the coarser EffectSummary which
/// aggregates all effects into actionable Boolean flags.
struct RefinedEffect {
    EffectKind  kind      = EffectKind::None;          ///< What kind of effect
    Region      region    = Region::None;              ///< Which memory region
    Stability   stability = Stability::Stable;         ///< Repeatability
    EscapeClass escape    = EscapeClass::NoEscape;     ///< Visibility outside fn
};

// ─────────────────────────────────────────────────────────────────────────────
// EffectSummary — aggregated per-function ERSL facts derived from FunctionEffects
// ─────────────────────────────────────────────────────────────────────────────
///
/// Populated by deriveEffectSummary() from the existing FunctionEffects.
/// All flags are conservative: true means "definitely has this property",
/// false means "does not have this property or unknown → assume worst case".
struct EffectSummary {
    // ── Aggregate effect flags (mirrored from FunctionEffects) ───────────
    bool hasIO        = false; ///< Performs I/O
    bool writesGlobal = false; ///< Writes to a global variable
    bool writesArg    = false; ///< Writes to argument-reachable memory

    // ── ERSL-extended dimensions ─────────────────────────────────────────
    /// True when the function always returns the same value for the same
    /// inputs AND has no externally-observable write effects.  Implies
    /// canDuplicate and canSpeculate.
    bool isStable = false;

    /// True when calling the function twice with the same arguments has the
    /// same net effect as calling it once.  Implies canDuplicate.
    /// Derived: isStable && !writesGlobal && !writesArg
    bool isIdempotent = false;

    /// True when the function can be called from a speculative execution
    /// path (e.g. on a branch that may not be taken) without changing
    /// observable behavior.  Derived: isStable && !hasIO && !writesGlobal
    bool canSpeculate = false;

    /// True when the function call can be duplicated (CSE can eliminate
    /// redundant calls to it).  Derived: isIdempotent
    bool canDuplicate = false;

    // ── Escape classification for write effects ──────────────────────────
    EscapeClass writeEscape = EscapeClass::NoEscape;

    // ── Maximum safe optimization level ─────────────────────────────────
    /// Integer 1–5 encoding how aggressively this function's computations
    /// can be transformed:
    ///   5 — PURE (full speculation, duplication, hoisting, vectorization)
    ///   4 — READONLY (hoist above writes, CSE loads)
    ///   3 — LOCAL WRITE only (hoist above non-conflicting ops, loop-invariant hoisting)
    ///   2 — GLOBAL/ARG WRITE (limited reordering, no speculative hoisting)
    ///   1 — IO / External (strict ordering, no reordering)
    unsigned maxSafeOptLevel = 1;
};

// ─────────────────────────────────────────────────────────────────────────────
// ERSL free functions
// ─────────────────────────────────────────────────────────────────────────────

/// True when effects @p A and @p B conflict (i.e. reordering them could
/// change observable behaviour).
///
/// Rules (in order):
///   1. Either effect is None → no conflict.
///   2. Different regions → no conflict.
///   3. Both reads → no conflict.
///   4. At least one write to the same region → conflict.
///   5. Both are I/O in the same region (Net, etc.) → conflict.
bool effectsConflict(const RefinedEffect& A, const RefinedEffect& B) noexcept;

/// Derive an EffectSummary from the existing FunctionEffects record.
///
/// This is a pure function: it reads @p fe and fills all EffectSummary
/// fields deterministically.  No LLVM analysis is required.
EffectSummary deriveEffectSummary(const FunctionEffects& fe) noexcept;

/// Return the maximum safe optimization level for @p es (integer 1–5).
///
/// The mapping follows the ERSL specification:
///   PURE (isStable && !hasIO && !writesGlobal && !writesArg) → 5
///   READONLY (no writes, no I/O, may read)                   → 4
///   LOCAL WRITE (writes local/heap, no escaping writes)       → 3
///   GLOBAL/ARG WRITE (escaping writes, no I/O)                → 2
///   IO / External                                             → 1
unsigned getMaxSafeOptLevel(const EffectSummary& es) noexcept;

} // namespace omscript

#endif // ERSL_H
