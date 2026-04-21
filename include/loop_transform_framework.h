#pragma once
#ifndef LOOP_TRANSFORM_FRAMEWORK_H
#define LOOP_TRANSFORM_FRAMEWORK_H

/// @file loop_transform_framework.h
/// @brief Unified loop transformation framework for OmScript.
///
/// This header is the single declaration point for all loop transformations
/// supported by the OmScript backend.  Previously, loop transforms were spread
/// across three separate systems:
///
///   - `polyopt.cpp` — polyhedral interchange, tiling, skewing, reversal, fusion,
///                     fission using SCoP extraction + Fourier-Motzkin legality.
///   - `codegen_opt.cpp` — LLVM-metadata-driven unrolling, vectorization hints.
///   - Various locations — ad-hoc inline transforms without shared legality.
///
/// **Unification design**
///
/// Every loop transform request now flows through `UnifiedLoopTransformer`,
/// which:
///
///   1. Queries the shared `LegalityService` for a high-level effect-safety
///      verdict (I/O check, mutation check).  An `Illegal` verdict aborts
///      immediately; `Unknown` proceeds to polyhedral analysis.
///
///   2. Calls `polyopt::checkLoopLegality()` for the fine-grained dependence-
///      direction-vector check.  Only transforms proven legal by the polyhedral
///      model are attempted.
///
///   3. Queries the shared `CostModel` via `isProfitable()` to decide whether
///      the expected savings justify the transform.  A transform that passes
///      legality but not profitability is not applied.
///
///   4. Dispatches to the appropriate backend implementation:
///        - Polyhedral transforms  → `polyopt::optimizeFunction()`
///        - LLVM metadata hints    → direct PassManager callback registration
///
/// ## Usage
/// ```cpp
/// LoopTransformRequest req;
/// req.transform  = LoopTransform::Tiling;
/// req.outerLoop  = loop;
/// req.function   = &F;
/// req.effects    = ctx.effects(F.getName().str());
///
/// UnifiedLoopTransformer xformer(legality, costModel);
/// LoopTransformResult res = xformer.execute(req, SE, DT, LI);
/// if (res.status == LoopTransformStatus::Applied) { /* update analyses */ }
/// ```
///
/// ## Adding a new transform
///   1. Add a value to `LoopTransform` in `optimization_manager.h`.
///   2. Add a case to `UnifiedLoopTransformer::execute()`.
///   3. Implement legality in `LegalityService::checkLegality()` or polyopt.
///   4. No changes to any other part of the pipeline are needed.

#include "optimization_manager.h" // LoopTransform, LegalityService, CostModel,
                                   // LegalityVerdict, LoopLegalityContext
#include "polyopt.h"               // checkLoopLegality, LoopLegalityResult

// Forward declarations.
namespace llvm {
class Loop;
class Function;
class ScalarEvolution;
class DominatorTree;
class LoopInfo;
} // namespace llvm

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// LoopTransformStatus — outcome of a UnifiedLoopTransformer::execute() call
// ─────────────────────────────────────────────────────────────────────────────
enum class LoopTransformStatus : uint8_t {
    Applied         = 0, ///< Transform was applied successfully
    SkippedIllegal  = 1, ///< Skipped: legality check returned Illegal
    SkippedUnknown  = 2, ///< Skipped: legality returned Unknown (conservative)
    SkippedUnprof   = 3, ///< Skipped: transform was legal but not profitable
    SkippedNoSCoP   = 4, ///< Skipped: SCoP detection failed for this loop
    SkippedDisabled = 5, ///< Skipped: this transform is disabled in config
    Failed          = 6, ///< Transform attempted but IR rewrite failed
};

// ─────────────────────────────────────────────────────────────────────────────
// LoopTransformRequest — input to UnifiedLoopTransformer::execute()
// ─────────────────────────────────────────────────────────────────────────────
///
/// Captures all information needed to evaluate and apply a single loop
/// transformation.  The caller fills this struct and passes it to
/// UnifiedLoopTransformer::execute().
struct LoopTransformRequest {
    /// The transformation to attempt.
    LoopTransform transform = LoopTransform::Tiling;

    /// The outermost loop of the nest to transform.
    llvm::Loop* outerLoop = nullptr;

    /// The LLVM function containing the loop nest.
    llvm::Function* function = nullptr;

    /// OmScript function effect summary for the containing function.
    /// If null, the LegalityService falls back to LLVM IR attributes.
    const FunctionEffects* effects = nullptr;

    /// Skewing factor (only used when transform == LoopTransform::Skewing).
    int64_t skewFactor = 1;

    /// Override the global PolyOptConfig for this specific request.
    /// Defaults are used when not provided.
    std::optional<polyopt::PolyOptConfig> polyConfig;
};

// ─────────────────────────────────────────────────────────────────────────────
// LoopTransformResult — output of UnifiedLoopTransformer::execute()
// ─────────────────────────────────────────────────────────────────────────────
struct LoopTransformResult {
    /// Whether and why the transform was (or wasn't) applied.
    LoopTransformStatus status = LoopTransformStatus::SkippedIllegal;

    /// High-level legality verdict from LegalityService (before polyhedral analysis).
    LegalityVerdict legalityVerdict = LegalityVerdict::Unknown;

    /// Polyhedral legality for the requested transform (valid when a SCoP was
    /// detected; all-false when SCoP detection failed).
    polyopt::LoopLegalityResult polyLegality;

    /// Whether the CostModel considered the transform profitable.
    bool wasProfitable = false;

    /// Statistics from polyopt (non-zero only when transform was dispatched).
    polyopt::PolyOptStats polyStats;

    bool applied() const noexcept {
        return status == LoopTransformStatus::Applied;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// UnifiedLoopTransformer — single dispatch point for all loop transforms
// ─────────────────────────────────────────────────────────────────────────────
///
/// UnifiedLoopTransformer is the authoritative loop-transform decision engine.
/// It holds non-owning pointers to the shared LegalityService and CostModel and
/// runs every transform request through the full legality + profitability chain
/// before dispatching to the appropriate backend implementation.
///
/// **Why this class exists**
///
/// Before the architecture unification, each transform had its own bespoke
/// legality check scattered across polyopt.cpp, codegen_opt.cpp, and other
/// files.  The consequence was:
///   - `polyopt` and `codegen_opt` could disagree on whether a transform is safe.
///   - Adding a new transform required editing multiple files.
///   - There was no single place to audit all loop-transform decisions.
///
/// UnifiedLoopTransformer fixes this by routing every request through the same
/// LegalityService → polyhedral check → CostModel → dispatch sequence.
class UnifiedLoopTransformer {
public:
    /// Construct with non-owning pointers to the shared services.
    /// @p legality and @p costModel may be null; if so, the corresponding
    /// checks are skipped (conservative: Unknown/always-profitable).
    UnifiedLoopTransformer(const LegalityService* legality,
                           const CostModel*       costModel) noexcept
        : legality_(legality), costModel_(costModel) {}

    /// Evaluate and (if safe and profitable) apply the transform described by
    /// @p req using the provided LLVM analyses.
    ///
    /// The result indicates whether the transform was applied and why it was
    /// skipped if not.
    LoopTransformResult execute(const LoopTransformRequest& req,
                                llvm::ScalarEvolution& SE,
                                llvm::DominatorTree& DT,
                                llvm::LoopInfo& LI) const;

private:
    const LegalityService* legality_;
    const CostModel*       costModel_;

    /// Build a LoopLegalityContext from the request fields.
    LoopLegalityContext buildLegalityContext(const LoopTransformRequest& req) const;

    /// Return true if the CostModel considers the transform profitable.
    /// Falls back to true (always profitable) when costModel_ is null.
    bool checkProfitability(LoopTransform transform,
                            llvm::Loop* loop) const noexcept;
};

} // namespace omscript

#endif // LOOP_TRANSFORM_FRAMEWORK_H
