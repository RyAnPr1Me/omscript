#pragma once
#ifndef SDR_PASS_H
#define SDR_PASS_H

// ─────────────────────────────────────────────────────────────────────────────
// sdr_pass.h — Speculative Devectorization & Revectorization (SDR)
// ─────────────────────────────────────────────────────────────────────────────
//
// SDR is a post-vectorization IR pass that questions existing vector code and
// rebuilds it better when the original vectorization was suboptimal.
//
// Pipeline position: LATE_SUPEROPT_HGOE — after superoptimizer idiom
// replacement but before final VectorCombine + codegen lowering.  At this
// point the IR has already been vectorized (by LoopVectorizer + SLP), so
// the SDR pass sees real vector ops rather than scalar candidates.
//
// Pass algorithm (four phases):
//   Phase 1 — Detect suboptimal SIMD regions
//     Classify each FixedVectorType instruction as one of:
//       PARTIAL_USE   — only a strict subset of lanes are consumed
//       EXTRACT_CHAIN — result is immediately extracted lane-by-lane
//       SCALAR_MIX    — vector op in a region dominated by scalar ops
//       WIDE_REDUCE   — all-lanes reduction using extract+arith pattern
//     Regions that pass the profitability threshold become SDR candidates.
//
//   Phase 2 — Devectorize (controlled scalar expansion)
//     For each candidate region: explode the vector SSA into per-lane
//     scalar values, preserving the dependency graph.  The original vector
//     def is replaced by insertElement chains so downstream code that *is*
//     profitable is unaffected.
//
//   Phase 3 — Scalar dataflow analysis
//     Walk the expanded scalars and compute:
//       • usedLaneMask — bitmask of lanes actually consumed downstream
//       • isReduction  — all lanes feed a horizontal arith tree
//       • parallelWidth — minimum vector width that covers used lanes
//
//   Phase 4 — Revectorize intelligently
//     Based on (usedLaneMask, isReduction, parallelWidth, TTI cost):
//       NARROW  — rebuild with a smaller FixedVectorType when lanes < original
//       WIDEN   — rebuild wider when TTI says throughput improves and lanes fit
//       REDUCE  — replace extract-arith chains with llvm.vector.reduce.* intrinsic
//       PASSTHROUGH — leave scalar if scalar cost ≤ vector cost (from TTI)
//
// Cost model:
//   Every rebuild decision is gated by TTI::getArithmeticInstrCost comparison
//   (new vs. original configuration).  No speculative rebuild is emitted if
//   it is not provably cheaper on the target.
//
// Thread safety: runSDR() is stateless and safe to call from multiple threads
//   provided each call receives its own Module reference.

#include <llvm/IR/Module.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <cstdint>
#include <functional>

namespace omscript::sdr {

// ─────────────────────────────────────────────────────────────────────────────
// SdrConfig — tuning knobs for the SDR pass
// ─────────────────────────────────────────────────────────────────────────────
struct SdrConfig {
    /// Minimum lane-use fraction below which a vector op is considered for
    /// devectorization.  E.g. 0.5 means "less than half the lanes are read".
    /// Range [0.0, 1.0].  Default: 0.5.
    double partialUseFraction = 0.5;

    /// Maximum number of instructions in a candidate region.  Regions larger
    /// than this are skipped to bound compile-time.  Default: 64.
    unsigned maxRegionSize = 64;

    /// Maximum original vector width (in lanes) eligible for phase-4 widening.
    /// Set to 0 to disable widening entirely.  Default: 8 (i.e. consider
    /// widening anything up to i32x8 / f32x8).
    unsigned maxWidenLanes = 8;

    /// Enable horizontal-reduction recognition and replacement with
    /// llvm.vector.reduce.* intrinsics.  Default: true.
    bool enableReductionRecognition = true;

    /// Enable lane-narrowing (e.g. i32x8 → i32x4 when only 4 lanes used).
    /// Default: true.
    bool enableNarrowing = true;

    /// Enable lane-widening when TTI indicates better throughput.
    /// Default: true.
    bool enableWidening = true;

    /// Profitability threshold: new cost must be ≤ (original cost × threshold)
    /// to justify a rebuild.  Default: 0.90 (require ≥10% improvement).
    double costThreshold = 0.90;

    /// Minimum TTI cost-saving (in abstract cycles) required to apply any
    /// transformation.  Guards against spurious micro-improvements that
    /// increase binary complexity without meaningful benefit.  Default: 1.0.
    double minAbsoluteSaving = 1.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// RegionKind — classification of a detected suboptimal SIMD region
// ─────────────────────────────────────────────────────────────────────────────
enum class RegionKind : uint8_t {
    PartialUse,   ///< Vector computed but only a strict subset of lanes read
    ExtractChain, ///< Vector lanes immediately extracted and used as scalars
    ScalarMix,    ///< Vector op embedded in a region dominated by scalar code
    WideReduce,   ///< All-lane horizontal reduction via extract+arith pattern
};

// ─────────────────────────────────────────────────────────────────────────────
// SdrRegion — one candidate region found during phase 1
// ─────────────────────────────────────────────────────────────────────────────
struct SdrRegion {
    llvm::Instruction* root     = nullptr; ///< Root vector instruction
    RegionKind         kind     = RegionKind::PartialUse;
    unsigned           origLanes = 0;      ///< Original vector width (lanes)
    unsigned           usedLanes = 0;      ///< Lanes actually consumed
    uint64_t           usedMask  = 0;      ///< Bitmask of consumed lane indices
    bool               isReduction = false;///< True when a horizontal reduction
    double             origCost    = 0.0;  ///< TTI cost of original sequence
};

// ─────────────────────────────────────────────────────────────────────────────
// SdrStats — per-module statistics from a single runSDR() call
// ─────────────────────────────────────────────────────────────────────────────
struct SdrStats {
    unsigned regionsDetected   = 0; ///< Phase-1: candidate regions found
    unsigned regionsAnalyzed   = 0; ///< Phase-2/3: regions fully analyzed
    unsigned narrowed          = 0; ///< Phase-4: vector narrowings applied
    unsigned widened           = 0; ///< Phase-4: vector widenings applied
    unsigned reductionsReplaced = 0;///< Phase-4: reduction intrinsics inserted
    unsigned passthrough        = 0;///< Phase-4: left as scalar (no gain)
    unsigned skippedCostly      = 0;///< Skipped because rebuild was not cheaper
    unsigned skippedLarge       = 0;///< Skipped because region too large

    /// Total transformations applied (sum of narrowed + widened + reductionsReplaced).
    unsigned totalTransformed() const noexcept {
        return narrowed + widened + reductionsReplaced;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// runSDR — the main entry point
// ─────────────────────────────────────────────────────────────────────────────
//
// Runs all four phases on every function in @p module.  Returns per-module
// statistics.
//
// @p getTTI   Callback that returns the TargetTransformInfo for a given
//             function.  Called at most once per function.  Must be callable
//             from the thread that calls runSDR().
//
// @p config   Tuning configuration.  A default-constructed SdrConfig gives
//             sensible defaults for O2-level compilation.
SdrStats runSDR(llvm::Module& module,
                const std::function<llvm::TargetTransformInfo(llvm::Function&)>& getTTI,
                const SdrConfig& config = {});

} // namespace omscript::sdr

#endif // SDR_PASS_H
