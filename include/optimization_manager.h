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
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declarations — avoids pulling in heavy LLVM headers for translation
// units that only need the manager interface.
namespace llvm {
class Instruction;
class Loop;
class Function;
} // namespace llvm

// Forward declaration for demand-driven scheduling.
namespace omscript { class Program; }

namespace omscript {

// Forward declaration — FunctionEffects is defined in ast.h, included via
// opt_context.h → ast.h.

// ─────────────────────────────────────────────────────────────────────────────
// PipelineStage — the ordered pipeline stages of the unified optimizer
// ─────────────────────────────────────────────────────────────────────────────
///
/// Each stage corresponds to a distinct compilation phase with a defined
/// analysis and transformation mandate.  The OptimizationManager coordinates
/// cross-stage interactions (cost model, legality, analysis cache) while the
/// stage enum provides a stable vocabulary for labelling passes, scheduling
/// requests, and progress diagnostics.
///
/// Stage ordering is:
///   AST_ANALYSIS → AST_TRANSFORM → IR_CANONICALIZE → LOOP_TRANSFORM
///   → IR_MIDEND → LATE_SUPEROPT_HGOE
///
/// Passes that belong to a stage declare it in PassMetadata::phase (which maps
/// to these stages via the PassPhase enum in opt_pass.h).
enum class PipelineStage : uint8_t {
    /// Read-only analyses of the OmScript AST: string/array type pre-analysis,
    /// constant-return detection, purity inference, effect inference.
    AST_ANALYSIS = 0,

    /// Semantics-preserving AST transformations: synthesis expansion, CF-CTRE,
    /// e-graph equality saturation, range analysis.
    AST_TRANSFORM = 1,

    /// LLVM IR normalization that must hold as invariants for all subsequent
    /// loop transforms: LoopSimplify, LCSSA, IndVarSimplify, SimplifyCFG.
    IR_CANONICALIZE = 2,

    /// Polyhedral and structural loop transforms: interchange, tiling, skewing,
    /// reversal, fusion, fission.  All transforms go through the unified
    /// LoopTransformFramework (see loop_transform_framework.h).
    LOOP_TRANSFORM = 3,

    /// LLVM midend scalar + vectorization pipeline: inlining, IPSCCP, GVN,
    /// DSE, vectorization, SLP, post-vec cleanup.
    IR_MIDEND = 4,

    /// Late peephole + synthesis: superoptimizer idiom replacement, synthesis
    /// candidate selection, HGOE hardware-guided emission, post-pipeline
    /// simplification.
    LATE_SUPEROPT_HGOE = 5,
};

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
    Unknown = 2, ///< Cannot determine from available information;
                 ///  callers should defer to fine-grained analysis (e.g. polyopt)
                 ///  rather than treating this as Illegal.
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

    /// Pre-computed ERSL EffectSummary for the function.
    /// When non-null, LegalityService uses this directly instead of
    /// recomputing it from @p effects.  Populate from
    /// OptimizationContext::effectSummary() when available.
    const EffectSummary* ersl = nullptr;
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

    /// IR-level function legality check using LLVM function attributes.
    ///
    /// This overload is called by the polyhedral pass when OmScript's AST-level
    /// FunctionEffects are not available (e.g. when running as a standalone LLVM
    /// pass without the full OmScript context).  It synthesises a conservative
    /// legality verdict from the LLVM function's memory-effect attributes:
    ///
    ///   • nofree + nosync   → Legal  (no I/O or global mutation visible in IR)
    ///   • willreturn absent → Unknown (function may not terminate — conservatively
    ///                          skip transforms to avoid divergence)
    ///   • otherwise         → Unknown (cannot rule out I/O / mutation)
    ///
    /// Note: a Legal verdict from this check is weaker than one from
    /// canTransformLoops() with valid FunctionEffects — dependence analysis in
    /// polyopt must still confirm that the specific transform is safe.
    virtual LegalityVerdict canTransformFunction(
        const llvm::Function& F) const noexcept;
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
///
///   3. **Demand-driven execution** — runToProvide() accepts a target fact key
///      and a dispatch map, then runs the minimum set of passes required to
///      compute that fact (recursively satisfying prerequisites first).  This
///      enables on-demand analysis: a downstream consumer can request a fact
///      without knowing the full pipeline order.
class PassScheduler {
public:
    /// Callable type for pass wrappers stored in the dispatch map.
    using Runner = std::function<void(Program*, OptimizationContext&)>;

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
    /// Cascades through the dependency graph if one is attached to ctx.
    void applyInvalidation(const PassMetadata& meta) noexcept;

    /// Return true if all facts in meta.provides_ are already valid.
    /// Used by runInvalidated to skip passes that are still up-to-date.
    bool allProvidedValid(const PassMetadata& meta) const noexcept;

    /// Demand-driven execution: run the minimum set of passes to ensure
    /// @p targetFact is valid.  Prerequisites are resolved recursively before
    /// the producing pass runs.
    ///
    /// @param targetFact  The analysis fact that must be valid on return.
    /// @param program     The AST being compiled (passed through to each pass).
    /// @param dispatch    Map from PassId → runner callable.  Only passes whose
    ///                    IDs appear in this map will be executed.
    ///
    /// @returns true  if @p targetFact is valid after the call.
    /// @returns false if the fact cannot be computed (no registered producer,
    ///                or a producer's preconditions could not be satisfied).
    ///
    /// **Cycle detection**: runToProvide() tracks the call stack via an
    /// in-progress set and returns false if a circular dependency is detected,
    /// rather than recursing infinitely.
    bool runToProvide(const std::string& targetFact,
                      Program* program,
                      const std::unordered_map<uint32_t, Runner>& dispatch);

private:
    /// Internal recursive worker for runToProvide.
    bool runToProvideImpl(const std::string& targetFact,
                          Program* program,
                          const std::unordered_map<uint32_t, Runner>& dispatch,
                          std::unordered_set<std::string>& inProgress);

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
