/// @file ersl.cpp
/// @brief Effect Refinement & Speculation Layer (ERSL) implementation.
///
/// See ersl.h for the design overview.

#include "ersl.h"

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// effectsConflict
// ─────────────────────────────────────────────────────────────────────────────

bool effectsConflict(const RefinedEffect& A, const RefinedEffect& B) noexcept {
    // Rule 1: either side is None → no conflict.
    if (A.kind == EffectKind::None || B.kind == EffectKind::None)
        return false;

    // Rule 2: different memory regions → no conflict
    // (two effects on disjoint regions can always be reordered).
    // Unknown region is treated conservatively: it may alias anything.
    if (A.region != Region::Unknown && B.region != Region::Unknown &&
        A.region != B.region)
        return false;

    // Rule 3: both reads (no writes, no IO) → no conflict.
    if (A.kind == EffectKind::Read && B.kind == EffectKind::Read)
        return false;

    // Rule 4: at least one write to the same (or unknown) region → conflict.
    if (A.kind == EffectKind::Write || B.kind == EffectKind::Write)
        return true;

    // Rule 5: both IO in the same domain → conflict (IO must be ordered).
    if (A.kind == EffectKind::IO && B.kind == EffectKind::IO)
        return true;

    // Mixed IO + Read/Write: treat as conflict (conservative).
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// deriveEffectSummary
// ─────────────────────────────────────────────────────────────────────────────

EffectSummary deriveEffectSummary(const FunctionEffects& fe) noexcept {
    EffectSummary es;

    // ── Mirror the existing flags ─────────────────────────────────────────
    es.hasIO        = fe.hasIO;
    es.writesGlobal = fe.writesGlobal;
    es.writesArg    = fe.hasMutation || fe.anyParamMutated();

    // ── Determine stability ────────────────────────────────────────────────
    // A function is Stable when:
    //   • It has no I/O (I/O results are External)
    //   • It has no writes to globals or args (such writes change the world state)
    //   • It has no indirect calls (unknown callees may have any effect)
    //   • It does not allocate/deallocate non-locally (allocation is observable via OOM)
    //
    // Note: readsMemory alone does NOT break stability — a function that only reads
    // memory is InputDependent (result may vary if caller mutates the memory between
    // calls), but is still safe to CSE within the same call path.
    const bool noWriteSideEffects = !fe.writesGlobal && !fe.writesMemory
                                     && !fe.hasMutation;
    const bool noExternalDependency = !fe.hasIO && !fe.hasIndirectCall;

    if (fe.isReadNone()) {
        // Reads nothing, writes nothing, no I/O → fully stable.
        es.isStable = true;
    } else if (noWriteSideEffects && noExternalDependency) {
        // May read (InputDependent) but no writes and no external deps → stable enough.
        es.isStable = true;
    } else {
        es.isStable = false;
    }

    // ── Derive idempotence ────────────────────────────────────────────────
    // Idempotent: stable AND no observable write effects.
    // A function with writesMemory=true that only writes LOCAL memory is still
    // idempotent if that memory doesn't escape (e.g. writes a local temp array)
    // but we take the conservative path: if writesMemory=true we consider it
    // non-idempotent unless the writes are provably local (which inferFunctionEffects
    // currently doesn't distinguish).  writesGlobal and writesArg are the
    // observable write tests.
    es.isIdempotent = es.isStable && !es.writesGlobal && !es.writesArg
                      && !fe.allocates && !fe.deallocates;

    // ── Derive speculation safety ─────────────────────────────────────────
    // canSpeculate: safe to hoist out of a branch / loop even if not taken.
    // Requires: stable + no I/O + no writes to escaping regions.
    es.canSpeculate = es.isStable && !es.hasIO
                      && !es.writesGlobal && !es.writesArg
                      && !fe.mayThrow;

    // ── Derive duplication safety ─────────────────────────────────────────
    es.canDuplicate = es.isIdempotent;

    // ── Determine write escape class ──────────────────────────────────────
    if (es.writesGlobal) {
        es.writeEscape = EscapeClass::GlobalEscape;
    } else if (es.writesArg) {
        es.writeEscape = EscapeClass::ArgEscape;
    } else {
        es.writeEscape = EscapeClass::NoEscape;
    }

    // ── Compute maximum safe optimization level ───────────────────────────
    es.maxSafeOptLevel = getMaxSafeOptLevel(es);

    return es;
}

// ─────────────────────────────────────────────────────────────────────────────
// getMaxSafeOptLevel
// ─────────────────────────────────────────────────────────────────────────────

unsigned getMaxSafeOptLevel(const EffectSummary& es) noexcept {
    // Level 1: I/O or external stability — strict ordering required.
    if (es.hasIO)
        return 1;

    // Level 2: writes to globals or arg-escaping memory — limited reordering.
    if (es.writesGlobal || es.writesArg)
        return 2;

    // Level 3: writes to local/heap (non-escaping) — loop-invariant hoisting safe.
    // At this point: no I/O, no escaping writes, but may have local writes.
    if (!es.isStable)
        return 3;

    // Level 4: read-only, stable, no writes — hoist above writes, CSE loads.
    if (!es.canDuplicate)
        return 4;

    // Level 5: pure, idempotent — full speculation, duplication, hoisting, CSE.
    return 5;
}

} // namespace omscript
