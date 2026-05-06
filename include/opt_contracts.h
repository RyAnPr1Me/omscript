#pragma once
#ifndef OPT_CONTRACTS_H
#define OPT_CONTRACTS_H

/// @file opt_contracts.h
/// @brief Formalized optimization annotation contracts for OmScript.
///
/// This header is the single source of truth for:
///   - What each optimization annotation *asserts* about a function or variable
///     (properties the compiler may rely on without proof).
///   - What each annotation *permits* the optimizer to do.
///   - Which pairs of annotations are mutually exclusive (conflict/domination).
///
/// Every place in the compiler that emits LLVM attributes based on annotations
/// should query this table rather than hard-coding attribute names inline.
///
/// ## Annotation categories
///
///   @opt(...)       — optimizer hints (inline, hot, cold, vectorize, …)
///   @semantics(...) — behavioral contracts (pure, speculatable, noreturn, …)
///   @repr(...)      — layout / ABI constraints (C, packed, soa, align(N))
///                     (already handled by StructRepr enum; listed here for
///                      cross-reference only)
///
/// ## Inhibitor-wins precedence rule
///
///   When two annotations on the same declaration conflict, the *inhibitor*
///   (the one that restricts optimization) always wins:
///
///     @noinline   > @inline
///     @opt(cold)  > @opt(hot)
///     @opt(novectorize) > @opt(vectorize)
///     @optnone    > @inline, @hot, @vectorize, @fastmath, @flatten
///
/// ## OPTMAX attribute contract
///
///   WillReturn, NoSync, and NoFree are claimed by OPTMAX, but only when the
///   function body does not contradict them.  The `requiresBodyProof` flag
///   marks these claims as needing runtime verification by a body scan before
///   the attribute is added to the LLVM function.

#include <string>

namespace omscript {

/// Identifies every annotation that carries an opt contract.
enum class AnnotationId {
    // ── @opt(...) family ──────────────────────────────────────────────────
    OPT_INLINE,        ///< @opt(inline)     / @inline
    OPT_NOINLINE,      ///< @opt(noinline)   / @noinline
    OPT_HOT,           ///< @opt(hot)        / @hot
    OPT_COLD,          ///< @opt(cold)       / @cold
    OPT_VECTORIZE,     ///< @opt(vectorize)  / @vectorize
    OPT_NOVECTORIZE,   ///< @opt(novectorize)/ @novectorize
    OPT_UNROLL,        ///< @opt(unroll)     / @unroll
    OPT_NOUNROLL,      ///< @opt(nounroll)   / @nounroll
    OPT_PARALLEL,      ///< @opt(parallel)   / @parallel
    OPT_NOPARALLEL,    ///< @opt(noparallel) / @noparallel
    OPT_FLATTEN,       ///< @opt(flatten)    / @flatten
    OPT_MINSIZE,       ///< @opt(minsize)    / @minsize
    OPT_NONE,          ///< @optnone — disable all optimizations
    OPT_ALIGN,         ///< @opt(align=N)    / @align(N)
    // ── @semantics(...) family ────────────────────────────────────────────
    SEM_PURE,          ///< @semantics(pure)         / @pure
    SEM_SPECULATABLE,  ///< @semantics(speculatable)  / @speculatable
    SEM_NORETURN,      ///< @semantics(noreturn)      / @noreturn
    SEM_NOUNWIND,      ///< @semantics(nounwind)      / @nounwind
    SEM_RESTRICT,      ///< @semantics(restrict)      / @restrict / @noalias
    SEM_CONST_EVAL,    ///< @semantics(const_eval)    / @const_eval
    // ── OPTMAX pseudo-annotation ──────────────────────────────────────────
    OPTMAX,            ///< OPTMAX=: block / @optmax annotation
    // ── Unknown / sentinel ────────────────────────────────────────────────
    UNKNOWN,
};

/// Properties asserted and permitted by a single annotation.
struct OptContract {
    /// The annotation's canonical name (for diagnostics).
    const char* name;

    /// The annotation inhibits optimization (true for @noinline, @cold, etc.).
    bool isInhibitor;

    /// The annotation is a positive optimizer hint (true for @inline, @hot, etc.).
    bool isAccelerator;

    // ── Asserted LLVM attributes (added unconditionally when annotation present) ──

    /// Asserts the function will always return (no infinite loops, no abort).
    bool assertsWillReturn;
    /// Asserts no synchronization operations (no atomics, mutexes, fences).
    bool assertsNoSync;
    /// Asserts no heap deallocation (no free(), delete, or similar).
    bool assertsNoFree;
    /// Asserts no observable side effects on program-visible memory (pure).
    bool assertsReadOnly;

    // ── Conditional LLVM attributes (only safe after body proof) ──────────

    /// WillReturn should be verified by scanning the function body before adding.
    bool requiresWillReturnProof;
    /// NoSync should be verified by scanning the function body before adding.
    bool requiresNoSyncProof;
    /// NoFree should be verified by scanning the function body before adding.
    bool requiresNoFreeProof;
};

/// Return the optimization contract for the given annotation.
///
/// The returned reference is valid for the lifetime of the program (the table
/// is a static array).
inline const OptContract& getOptContract(AnnotationId id) {
    // clang-format off
    static constexpr OptContract kTable[] = {
        // name               inh    acc    wret   nosync nofree rdonly wretP  nsyncP nfreeP
        { "opt(inline)",      false, true,  false, false, false, false, false, false, false }, // OPT_INLINE
        { "opt(noinline)",    true,  false, false, false, false, false, false, false, false }, // OPT_NOINLINE
        { "opt(hot)",         false, true,  false, false, false, false, false, false, false }, // OPT_HOT
        { "opt(cold)",        true,  false, false, false, false, false, false, false, false }, // OPT_COLD
        { "opt(vectorize)",   false, true,  false, false, false, false, false, false, false }, // OPT_VECTORIZE
        { "opt(novectorize)", true,  false, false, false, false, false, false, false, false }, // OPT_NOVECTORIZE
        { "opt(unroll)",      false, true,  false, false, false, false, false, false, false }, // OPT_UNROLL
        { "opt(nounroll)",    true,  false, false, false, false, false, false, false, false }, // OPT_NOUNROLL
        { "opt(parallel)",    false, true,  false, false, false, false, false, false, false }, // OPT_PARALLEL
        { "opt(noparallel)",  true,  false, false, false, false, false, false, false, false }, // OPT_NOPARALLEL
        { "opt(flatten)",     false, true,  false, false, false, false, false, false, false }, // OPT_FLATTEN
        { "opt(minsize)",     false, false, false, false, false, false, false, false, false }, // OPT_MINSIZE
        { "optnone",          true,  false, false, false, false, false, false, false, false }, // OPT_NONE
        { "opt(align=N)",     false, false, false, false, false, false, false, false, false }, // OPT_ALIGN
        { "semantics(pure)",          false, true,  false, false, false, true,  false, false, false }, // SEM_PURE
        { "semantics(speculatable)",  false, true,  false, false, false, false, false, false, false }, // SEM_SPECULATABLE
        { "semantics(noreturn)",      false, false, false, false, false, false, false, false, false }, // SEM_NORETURN
        { "semantics(nounwind)",      false, false, false, false, false, false, false, false, false }, // SEM_NOUNWIND
        { "semantics(restrict)",      false, true,  false, false, false, false, false, false, false }, // SEM_RESTRICT
        { "semantics(const_eval)",    false, true,  false, false, false, true,  false, false, false }, // SEM_CONST_EVAL
        // OPTMAX: WillReturn / NoSync / NoFree are *claimed* but require body proof before emitting.
        { "optmax",           false, true,  false, false, false, false, true,  true,  true  }, // OPTMAX
        { "<unknown>",        false, false, false, false, false, false, false, false, false }, // UNKNOWN
    };
    // clang-format on
    const auto idx = static_cast<int>(id);
    const auto size = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
    if (idx < 0 || idx >= size) return kTable[size - 1]; // UNKNOWN
    return kTable[idx];
}

/// Return true if annotation `b` is dominated by annotation `a` (a wins).
///
/// When two annotations on the same declaration conflict, the dominating one
/// wins according to the inhibitor-wins rule: the inhibitor always beats the
/// accelerator.
///
/// Examples:
///   dominates(OPT_NOINLINE, OPT_INLINE)      → true
///   dominates(OPT_COLD, OPT_HOT)             → true
///   dominates(OPT_NOVECTORIZE, OPT_VECTORIZE) → true
///   dominates(OPT_NONE, OPT_INLINE)          → true
inline bool annotationDominates(AnnotationId a, AnnotationId b) {
    // Inhibitor annotation 'a' always beats accelerator 'b'.
    const OptContract& ca = getOptContract(a);
    const OptContract& cb = getOptContract(b);
    if (ca.isInhibitor && cb.isAccelerator) return true;
    // @optnone dominates everything.
    if (a == AnnotationId::OPT_NONE && b != AnnotationId::OPT_NONE) return true;
    return false;
}

/// Return true if annotations `a` and `b` directly conflict (one should win
/// and the other should be silently dropped or warned about).
///
/// Conflict pairs follow the inhibitor-wins rule: the inhibitor wins.
/// Either order is checked symmetrically.
inline bool annotationsConflict(AnnotationId a, AnnotationId b) {
    if (a == b) return false;
    // Check both directions.
    return annotationDominates(a, b) || annotationDominates(b, a);
}

/// Resolve a conflict between annotations `a` and `b` and return the winner.
/// Returns `a` when both are inhibitors or neither (tie-break: keep first).
inline AnnotationId resolveConflict(AnnotationId a, AnnotationId b) {
    if (annotationDominates(a, b)) return a;
    if (annotationDominates(b, a)) return b;
    return a; // tie — keep the first one seen
}

/// Convert an AnnotationId to its canonical display name (for diagnostics).
inline const char* annotationName(AnnotationId id) {
    return getOptContract(id).name;
}

} // namespace omscript

#endif // OPT_CONTRACTS_H
