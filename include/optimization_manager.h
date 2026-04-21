#pragma once
#ifndef OPTIMIZATION_MANAGER_H
#define OPTIMIZATION_MANAGER_H

/// @file optimization_manager.h
/// @brief Unified optimization pipeline coordinator for OmScript.
///
/// This header defines the central OptimizationManager and its component
/// services:
///
///   CostModel       — abstract interface for instruction cost estimation.
///                     Replaces the scattered cost tables in superoptimizer.cpp
///                     and polyopt.cpp with a single shared oracle, ensuring
///                     consistent profitability decisions across the pipeline.
///
///   LegalityService — high-level transformation safety oracle.
///                     Centralises effect/alias checks that guard whether
///                     loop transforms are even worth attempting (before the
///                     fine-grained polyhedral legality analysis in polyopt).
///
///   PassScheduler   — drives AST-level pass execution with correct
///                     precondition enforcement and automatic analysis
///                     invalidation after each transformation pass.
///
///   OptimizationManager — owns CostModel and LegalityService, and provides a
///                     single construction/injection point for the pipeline.
///                     The PassScheduler is constructed on-demand from the
///                     manager when the AST-level orchestrator needs it.
///
/// ## Architecture notes
///
/// The manager spans both the AST pre-pass layer (orchestrated by
/// OptimizationOrchestrator) and the LLVM IR layer (driven by
/// CodeGenerator::runOptimizationPasses).  Common services (cost model,
/// legality) are shared across both layers.
///
/// Fine-grained dependence analysis (which determines whether a specific loop
/// transform preserves execution order) remains in polyopt's SCoP extraction
/// and Fourier-Motzkin tests — that analysis is tightly coupled to the
/// polyhedral model.  The LegalityService handles the coarser, effect-level
/// checks that can rule out whole classes of transforms cheaply.

#include "opt_context.h"   // AnalysisCache, PassContract, OptimizationContext
#include "opt_pass.h"      // PassMetadata, AnalysisKey, IRInvariant
#include <memory>
#include <vector>

// Forward declarations — avoids pulling in heavy LLVM headers for translation
// units that only need the manager interface.
namespace llvm {
class Instruction;
class Loop;
class Function;
} // namespace llvm

namespace omscript {

// Forward declaration — FunctionEffects is defined in ast.h, included via
// opt_context.h → ast.h.

// ─────────────────────────────────────────────────────────────────────────────
// CostModel — abstract instruction cost interface
// ─────────────────────────────────────────────────────────────────────────────
///
/// Every component that needs cost information (superoptimizer, loop
/// profitability model, polyopt tile-size selection) queries this interface
/// instead of using hard-coded cost tables or separate ad-hoc heuristics.
///
/// The default implementation (created by createDefaultCostModel()) delegates
/// to superopt::instructionCost(), which provides a tuned x86-64 latency table
/// with optional hardware-profile override via SuperoptimizerConfig::costFn.
///
/// **Thread safety**: implementations MUST be safe to call concurrently from
/// multiple threads (each call pipeline runs on its own thread).
class CostModel {
public:
    virtual ~CostModel() = default;

    /// Return the estimated execution cost (latency in cycles) of @p inst.
    /// Returns 0.0 for null instructions.
    virtual double instructionCost(const llvm::Instruction* inst) const = 0;

    /// Return true when replacing a sequence of cost @p oldCost with one of
    /// cost @p newCost is considered profitable under @p threshold.
    ///
    /// Default: profitable when newCost < oldCost × threshold (0.95 = 5% gain).
    virtual bool isProfitable(double newCost, double oldCost,
                              double threshold = 0.95) const noexcept {
        return oldCost > 0.0 && newCost < oldCost * threshold;
    }
};

/// Create the default CostModel that wraps superopt::instructionCost().
/// Callers that need hardware-accurate cost data should install an HGOE-backed
/// model via OptimizationManager::setCostModel().
std::unique_ptr<CostModel> createDefaultCostModel();

// ─────────────────────────────────────────────────────────────────────────────
// LoopTransform — enumeration of supported loop transformations
// ─────────────────────────────────────────────────────────────────────────────
enum class LoopTransform : uint8_t {
    Interchange = 0, ///< Swap the iteration order of two loop levels
    Tiling,          ///< Cache blocking: tile the loop nest by tile sizes
    Reversal,        ///< Reverse the direction of a loop
    Skewing,         ///< Shear transformation (add multiple of outer IV to inner)
    Fusion,          ///< Merge two adjacent loops with compatible iteration spaces
    Fission,         ///< Split a loop body into multiple independent loops
};

// ─────────────────────────────────────────────────────────────────────────────
// LegalityVerdict — result of a high-level legality check
// ─────────────────────────────────────────────────────────────────────────────
enum class LegalityVerdict : uint8_t {
    Legal   = 0, ///< Transform is definitely safe to apply
    Illegal = 1, ///< Transform is definitely unsafe (would change semantics)
    Unknown = 2, ///< Cannot determine; treat conservatively as Illegal
};

// ─────────────────────────────────────────────────────────────────────────────
// LoopLegalityContext — context for LegalityService::checkLegality()
// ─────────────────────────────────────────────────────────────────────────────
///
/// Captures the information needed for a high-level safety check.  Fine-grained
/// dependence analysis (direction vectors, Fourier-Motzkin) is NOT done here —
/// that remains in polyopt.  This context covers the coarser concerns:
/// function effects (I/O, global mutation), and loop structural properties.
struct LoopLegalityContext {
    llvm::Function* function  = nullptr; ///< The LLVM function containing the loop
    llvm::Loop*     outerLoop = nullptr; ///< The outermost loop in the nest
    llvm::Loop*     innerLoop = nullptr; ///< The innermost loop (may equal outerLoop)
    unsigned        loopDepth = 0;       ///< Depth of the loop nest
    int64_t         skewFactor = 1;      ///< Skewing factor (for Skewing checks)

    /// Function effect summary from OmScript's pre-pass analysis.
    /// If null, the service falls back to a conservative Unknown verdict.
    const FunctionEffects* effects = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// LegalityService — shared high-level transformation legality oracle
// ─────────────────────────────────────────────────────────────────────────────
///
/// Centralises the high-level (effect/alias/structural) checks that guard
/// whether a loop transformation is worth attempting.  Components that currently
/// embed ad-hoc effect checks (e.g. "skip if function has I/O") call this
/// service instead so the logic is defined once.
///
/// Fine-grained polyhedral legality (dependence direction vectors, which
/// determine whether a specific transform of a specific loop nest preserves
/// execution order) remains in polyopt — it is too coupled to the polyhedral
/// model to centralise cheaply and is already well-encapsulated.
///
/// **Extensibility**: Subclass and inject into OptimizationManager to add
/// aliasing information from an external alias analysis or to tighten the
/// effect-based rules.
class LegalityService {
public:
    virtual ~LegalityService() = default;

    /// High-level check: can ANY loop transformation be applied to the function
    /// described by @p ctx?
    ///
    /// Default implementation:
    ///   • Returns Illegal if ctx.effects reports I/O or unconditional mutation.
    ///   • Returns Unknown if ctx.effects is null.
    ///   • Returns Legal otherwise (fine-grained dependence check is in polyopt).
    virtual LegalityVerdict canTransformLoops(
        const LoopLegalityContext& ctx) const noexcept;

    /// Per-transform legality check.
    ///
    /// Default implementation:
    ///   Interchange/Tiling/Reversal/Skewing → delegate to canTransformLoops.
    ///   Fusion/Fission → Unknown (require knowledge of adjacent loop spaces).
    virtual LegalityVerdict checkLegality(
        LoopTransform transform,
        const LoopLegalityContext& ctx) const noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// PassScheduler — drives AST-level passes with correctness enforcement
// ─────────────────────────────────────────────────────────────────────────────
///
/// PassScheduler adds two correctness-critical behaviours to the pass pipeline:
///
///   1. **Automatic invalidation** — after each transformation pass completes,
///      applyInvalidation() marks stale analysis facts (those in
///      PassMetadata::invalidates_) as invalid in both AnalysisValidity and
///      AnalysisCache.  Without this, downstream passes may silently read stale
///      facts computed before the AST was modified.
///
///   2. **Precondition enforcement** — before each pass runs,
///      checkPreconditions() verifies that all required facts
///      (PassMetadata::requires_) are valid.  In debug builds (strict mode)
///      this is a hard assertion; in release builds the pass is skipped with a
///      diagnostic warning.
class PassScheduler {
public:
    explicit PassScheduler(OptimizationContext& ctx) noexcept : ctx_(ctx) {}

    /// Set strict mode.  Default: false (release), true in debug builds.
    /// Strict mode asserts on precondition failures rather than skipping.
    void setStrictMode(bool strict) noexcept { strict_ = strict; }
    bool strictMode() const noexcept { return strict_; }

    /// Return true if all facts required by @p meta are currently valid.
    /// Logs a warning for each missing requirement.  In strict mode, also
    /// asserts (in debug builds) that all requirements are met.
    /// Returns false if any requirement is not satisfied.
    bool checkPreconditions(const PassMetadata& meta) const noexcept;

    /// Apply the invalidation declared in @p meta after a pass completes.
    /// Marks each fact in meta.invalidates_ as invalid in ctx.validity()
    /// and removes the corresponding entry from ctx.cache().
    void applyInvalidation(const PassMetadata& meta) noexcept;

    /// Return true if all facts in meta.provides_ are already valid.
    /// Used by runInvalidated to skip passes that are still up-to-date.
    bool allProvidedValid(const PassMetadata& meta) const noexcept;

private:
    OptimizationContext& ctx_;
    bool strict_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// OptimizationManager — unified optimization pipeline coordinator
// ─────────────────────────────────────────────────────────────────────────────
///
/// The OptimizationManager is the single construction and wiring point for the
/// shared optimization services (cost model, legality).  It does NOT own the
/// OptimizationContext (that is owned by CodeGenerator::generate()).
///
/// **AST-level use** (OptimizationOrchestrator):
///   The orchestrator calls createScheduler(ctx) to obtain a PassScheduler
///   that uses the shared context.
///
/// **IR-level use** (CodeGenerator::runOptimizationPasses):
///   - setCostModel(createDefaultCostModel()) to install the cost oracle.
///   - costModel() to build the superoptimizer's costFn wrapper.
///   - legality() to perform high-level safety checks before polyopt.
///
/// **Example**
/// ```cpp
/// OptimizationManager mgr;
/// mgr.setCostModel(createDefaultCostModel());
/// auto sched = mgr.createScheduler(ctx);
/// sched.setStrictMode(true);
/// ```
class OptimizationManager {
public:
    OptimizationManager() = default;
    ~OptimizationManager() = default;

    // Non-copyable.
    OptimizationManager(const OptimizationManager&)            = delete;
    OptimizationManager& operator=(const OptimizationManager&) = delete;

    // ── Cost model ────────────────────────────────────────────────────────

    /// Set the active cost model.  Takes ownership.
    void setCostModel(std::unique_ptr<CostModel> model) noexcept;

    /// Return the active cost model, or nullptr if none has been set.
    const CostModel* costModel() const noexcept { return costModel_.get(); }
    CostModel*       costModel()       noexcept { return costModel_.get(); }

    // ── Legality service ──────────────────────────────────────────────────

    /// Return the legality service for high-level transform safety checks.
    LegalityService&       legality()       noexcept { return legality_; }
    const LegalityService& legality() const noexcept { return legality_; }

    // ── Pass scheduler factory ────────────────────────────────────────────

    /// Create a PassScheduler bound to @p ctx.
    /// In debug builds the scheduler is created in strict mode (assertions).
    PassScheduler createScheduler(OptimizationContext& ctx) const;

private:
    LegalityService            legality_;
    std::unique_ptr<CostModel> costModel_;
};

} // namespace omscript

#endif // OPTIMIZATION_MANAGER_H
