#pragma once
#ifndef OPT_ORCHESTRATOR_H
#define OPT_ORCHESTRATOR_H

/// @file opt_orchestrator.h
/// @brief The Optimization Orchestrator for the OmScript compiler.
///
/// The Orchestrator is the single entry point that sequences all pre-pass
/// analyses and AST-level transformations that run before LLVM IR is emitted.
/// It replaces the manual inline call sequence in CodeGenerator::generate()
/// with a dependency-aware, fact-tracked pipeline.
///
/// Stages driven by the Orchestrator
/// ==================================
///   1. Preprocessing   — preAnalyzeStringTypes / preAnalyzeArrayTypes
///   2. Constant returns — analyzeConstantReturnValues
///   3. Purity analysis  — autoDetectConstEvalFunctions
///   4. Effect inference — inferFunctionEffects
///   5. Synthesis pass   — runSynthesisPass
///   6. CF-CTRE          — runCFCTRE (cross-function compile-time evaluation)
///   7. E-graph          — egraph::optimizeProgram (equality saturation)
///
/// The Orchestrator checks AnalysisValidity before each stage so that, in
/// future incremental-compilation scenarios, a stage is skipped when its
/// prerequisites are already valid.
///
/// **Usage**
/// ```cpp
/// OptimizationContext ctx;
/// ctx.setCTEngine(&ctEngine_);
/// OptimizationOrchestrator orch(optLevel_, verbose_);
/// orch.runPrepasses(program, ctx);  // populates ctx; modifies program AST
/// // ... IR emission uses ctx for purity/range/const queries ...
/// ```

#include "opt_context.h"
#include "opt_pass.h"
#include "optimization_manager.h" // OptimizationManager
#include <unordered_map>
#include <vector>

namespace omscript {

// Forward declarations (implementations live in codegen.cpp / cfctre.cpp).
class CodeGenerator;
enum class OptimizationLevel;

// ─────────────────────────────────────────────────────────────────────────────
// OptimizationOrchestrator
// ─────────────────────────────────────────────────────────────────────────────
class OptimizationOrchestrator {
public:
    /// @param optLevel  Optimization level (drives which passes are active).
    /// @param verbose   When true, print per-pass timing and statistics.
    /// @param codegen   Non-owning pointer to the CodeGenerator that owns the
    ///                  pass implementations (preAnalyzeStringTypes, etc.).
    ///                  Must outlive the Orchestrator.
    /// @param manager   Optional non-owning pointer to a shared
    ///                  OptimizationManager.  When provided, the orchestrator
    ///                  uses manager->createScheduler(ctx) to obtain a
    ///                  PassScheduler that respects the manager's strict-mode
    ///                  setting and shares the manager's dependency graph.
    ///                  When nullptr (the default), a standalone PassScheduler
    ///                  is created from ctx directly.
    explicit OptimizationOrchestrator(OptimizationLevel optLevel,
                                      bool verbose,
                                      CodeGenerator* codegen,
                                      OptimizationManager* manager = nullptr) noexcept;

    ~OptimizationOrchestrator() = default;

    // Non-copyable.
    OptimizationOrchestrator(const OptimizationOrchestrator&)            = delete;
    OptimizationOrchestrator& operator=(const OptimizationOrchestrator&) = delete;

    // ── Primary entry point ───────────────────────────────────────────────

    /// Run all pre-pass analyses and AST transforms for @p program.
    ///
    /// On return:
    ///   • @p ctx is populated with per-function facts.
    ///   • All AnalysisValidity flags for completed passes are set to true.
    ///   • The AST may have been modified by the synthesis and e-graph passes.
    ///
    /// The existing behavior of CodeGenerator::generate() is preserved exactly:
    /// this method calls the same functions in the same order, but now does so
    /// through the unified context so facts are centrally recorded.
    void runPrepasses(Program* program, OptimizationContext& ctx);

    // ── Selective re-run ──────────────────────────────────────────────────

    /// Re-run only the passes whose AnalysisValidity flags are false.
    /// Used in incremental compilation scenarios where a small transform has
    /// invalidated a subset of facts.
    void runInvalidated(Program* program, OptimizationContext& ctx);

    // ── Demand-driven execution ───────────────────────────────────────────

    /// Ensure that @p factKey is valid in @p ctx.
    ///
    /// Runs the minimum set of passes required to make @p factKey valid,
    /// recursively satisfying prerequisites first.  Unlike runPrepasses (which
    /// runs the full pipeline), this method only runs what is needed for the
    /// requested fact.
    ///
    /// @returns true  if @p factKey is valid on return.
    /// @returns false if the fact cannot be computed (no registered producer,
    ///                circular dependency, or a prerequisite could not be met).
    bool runToProvide(const std::string& factKey,
                      Program* program,
                      OptimizationContext& ctx);

    // ── Statistics ────────────────────────────────────────────────────────

    /// Wall-clock timing for a single pass.
    struct PassTiming {
        std::string name;      ///< Pass name (mirrors PassMetadata::name)
        double      elapsedMs; ///< Wall-clock time in milliseconds
    };

    struct RunStats {
        unsigned passesRun      = 0; ///< Number of passes that executed
        unsigned passesSkipped  = 0; ///< Number of passes skipped (already valid)
        double   totalElapsedMs = 0; ///< Wall-clock time for all passes (ms)

        /// Per-pass wall-clock times in execution order.
        /// Empty until after the first runPrepasses / runInvalidated call.
        std::vector<PassTiming> passTimings;
    };

    const RunStats& lastRunStats() const noexcept { return stats_; }

private:
    // ── Helpers ───────────────────────────────────────────────────────────

    // Run one named pass and record timing / validity.
    void runStringTypes    (Program* program, OptimizationContext& ctx);
    void runArrayTypes     (Program* program, OptimizationContext& ctx);
    void runConstantReturns(Program* program, OptimizationContext& ctx);
    void runPurity         (Program* program, OptimizationContext& ctx);
    void runEffects        (Program* program, OptimizationContext& ctx);
    void runSynthesis      (Program* program, OptimizationContext& ctx);
    void runCFCTRE         (Program* program, OptimizationContext& ctx);
    void runEGraph         (Program* program, OptimizationContext& ctx);
    void runRangeAnalysis  (Program* program, OptimizationContext& ctx);
    void runRLC            (Program* program, OptimizationContext& ctx);
    void runDCE            (Program* program, OptimizationContext& ctx);
    void runCSE            (Program* program, OptimizationContext& ctx);
    void runAlgSimp        (Program* program, OptimizationContext& ctx);

    /// Build the PassId → runner dispatch map used by runPassPipeline and
    /// runToProvide.  Defined once here so the 10-entry table never needs to
    /// be duplicated across call sites.
    std::unordered_map<uint32_t, PassScheduler::Runner> buildDispatch();

    /// Construct a PassScheduler, inheriting strict-mode from the manager
    /// (debug builds) or using a standalone scheduler when no manager is set.
    PassScheduler makeScheduler(OptimizationContext& ctx);

    /// Copy analysis results from the CodeGenerator's per-pass output into
    /// the unified OptimizationContext so callers can query a single surface.
    void syncFactsToContext(Program* program, OptimizationContext& ctx) const;

    /// Core pipeline driver used by both runPrepasses and runInvalidated.
    ///
    /// Computes the topological order from PassRegistry, then iterates over
    /// passes in dependency order.  When @p skipValid is true, each pass whose
    /// provided facts are already all-valid in ctx is skipped (increments
    /// passesSkipped).  Per-pass wall-clock timing is recorded in stats_.
    void runPassPipeline(Program* program, OptimizationContext& ctx, bool skipValid);

    // ── State ─────────────────────────────────────────────────────────────
    OptimizationLevel   optLevel_;
    bool                verbose_;
    CodeGenerator*      codegen_;   // non-owning
    OptimizationManager* manager_;  // non-owning; may be nullptr
    RunStats            stats_;
};

} // namespace omscript

#endif // OPT_ORCHESTRATOR_H
