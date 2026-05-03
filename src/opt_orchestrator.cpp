/// @file opt_orchestrator.cpp
/// @brief PassRegistry, pass metadata catalog, and OptimizationOrchestrator.
///
/// This file:
///  1. Implements the PassRegistry singleton and its topological-sort helper.
///  2. Registers metadata for every pre-pass in the pipeline.
///  3. Implements OptimizationOrchestrator::runPrepasses(), which replaces the
///     manual inline call sequence in CodeGenerator::generate() with a fact-
///     tracked, dependency-ordered pipeline.

#include "opt_orchestrator.h"
#include "codegen.h"   // CodeGenerator + OptimizationLevel
#include "optimization_manager.h" // PassScheduler
#include "alg_simp_pass.h"
#include "copy_prop_pass.h"
#include "dce_pass.h"
#include "cse_pass.h"
#include "ersl.h"
#include "pass_utils.h"
#include "width_legalization.h"
#include "width_opt_pass.h"
#include "diagnostic.h"

#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// PassRegistry implementation
// ─────────────────────────────────────────────────────────────────────────────

/* static */
PassRegistry& PassRegistry::instance() {
    static PassRegistry reg;
    return reg;
}

uint32_t PassRegistry::registerPass(PassMetadata meta) {
    meta.id = nextId_++;
    const size_t idx = passes_.size();
    passes_.push_back(std::move(meta));
    byId_[passes_[idx].id]     = idx;
    byName_[passes_[idx].name] = idx;
    return passes_[idx].id;
}

const PassMetadata* PassRegistry::find(uint32_t id) const noexcept {
    auto it = byId_.find(id);
    return (it != byId_.end()) ? &passes_[it->second] : nullptr;
}

const PassMetadata* PassRegistry::find(const std::string& name) const noexcept {
    auto it = byName_.find(name);
    return (it != byName_.end()) ? &passes_[it->second] : nullptr;
}

std::vector<uint32_t> PassRegistry::topologicalOrder(
    const std::vector<uint32_t>& subset) const
{
    // Build the working set.
    std::unordered_set<uint32_t> inSet;
    if (subset.empty()) {
        for (const auto& p : passes_) inSet.insert(p.id);
    } else {
        inSet.insert(subset.begin(), subset.end());
    }

    // Map fact-name → set of pass IDs that produce it.
    std::unordered_map<std::string, std::vector<uint32_t>> producers;
    for (const auto& p : passes_) {
        if (!inSet.count(p.id)) continue;
        for (const char* fact : p.provides_) {
            producers[fact].push_back(p.id);
        }
    }

    // Kahn's algorithm.
    // in-degree: number of unsatisfied requirements.
    std::unordered_map<uint32_t, int> inDegree;
    for (const auto& p : passes_) {
        if (!inSet.count(p.id)) continue;
        inDegree[p.id] = 0;
    }
    for (const auto& p : passes_) {
        if (!inSet.count(p.id)) continue;
        for (const char* req : p.requires_) {
            // Each producer of 'req' is a prerequisite.
            auto it = producers.find(req);
            if (it == producers.end()) continue;
            for (uint32_t pid : it->second) {
                if (pid != p.id) inDegree[p.id]++;
            }
        }
    }

    // Seed queue with zero-in-degree passes, sorted by ID for determinism.
    // Use deque so front removal is O(1) instead of O(n).
    std::deque<uint32_t> ready;
    {
        std::vector<uint32_t> tmp;
        for (const auto& [id, deg] : inDegree) {
            if (deg == 0) tmp.push_back(id);
        }
        std::sort(tmp.begin(), tmp.end());
        for (auto id : tmp) ready.push_back(id);
    }

    std::vector<uint32_t> order;
    order.reserve(inSet.size());

    while (!ready.empty()) {
        uint32_t cur = ready.front();
        ready.pop_front();
        order.push_back(cur);

        const PassMetadata* meta = find(cur);
        if (!meta) continue;

        // Decrement in-degree of every pass that requires a fact this pass provides.
        for (const char* fact : meta->provides_) {
            for (const auto& p : passes_) {
                if (!inSet.count(p.id)) continue;
                for (const char* req : p.requires_) {
                    if (std::string(req) == fact) {
                        inDegree[p.id]--;
                        if (inDegree[p.id] == 0) {
                            // Insert in sorted position for determinism.
                            auto pos = std::lower_bound(ready.begin(), ready.end(), p.id);
                            ready.insert(pos, p.id);
                        }
                    }
                }
            }
        }
    }

    if (order.size() != inSet.size()) {
        // Identify which passes were not reached (they are part of the cycle).
        std::unordered_set<uint32_t> reached(order.begin(), order.end());
        std::string cycleNames;
        for (const auto& p : passes_) {
            if (inSet.count(p.id) && !reached.count(p.id)) {
                if (!cycleNames.empty()) cycleNames += ", ";
                cycleNames += p.name;
            }
        }
        throw std::logic_error(
            "OptimizationOrchestrator: dependency cycle detected in pass registry"
            " (involved passes: " + cycleNames + ")");
    }
    return order;
}

// ─────────────────────────────────────────────────────────────────────────────
// Well-known PassId definitions
// ─────────────────────────────────────────────────────────────────────────────

namespace PassId {
    uint32_t kPreflightCheck    = 0;
    uint32_t kStringTypes       = 0;
    uint32_t kArrayTypes        = 0;
    uint32_t kConstantReturns   = 0;
    uint32_t kPurity            = 0;
    uint32_t kEffects           = 0;
    uint32_t kERSL              = 0;
    uint32_t kSynthesis         = 0;
    uint32_t kCFCTRE            = 0;
    uint32_t kEGraph            = 0;
    uint32_t kRangeAnalysis     = 0;
    uint32_t kRLC               = 0;
    uint32_t kDCE               = 0;
    uint32_t kCSE               = 0;
    uint32_t kAlgSimp           = 0;
    uint32_t kCopyProp          = 0;
    uint32_t kWidthLegalization = 0;
    uint32_t kWidthOpt          = 0;
} // namespace PassId

// ─────────────────────────────────────────────────────────────────────────────
// Static registration of all pre-pass metadata
// ─────────────────────────────────────────────────────────────────────────────
//
// Runs once at program startup via the static-local flag below.

static void registerAllPasses() {
    static bool done = false;
    if (done) return;
    done = true;

    auto& reg = PassRegistry::instance();

    PassId::kPreflightCheck = reg.registerPass({
        0,
        "preflight_check",
        "Pre-flight error detection: scan for fatal errors (e.g. literal division by zero) before any optimisation",
        PassPhase::Preprocessing,
        PassKind::Analysis,
        {},                                  // requires nothing — runs first
        {AnalysisFact::kPreflightCheck},     // provides
        {},                                  // invalidates nothing
    });

    PassId::kStringTypes = reg.registerPass({
        0,
        "string_types",
        "Pre-analyse string return types and parameter types across all functions",
        PassPhase::Preprocessing,
        PassKind::Analysis,
        {},                              // requires nothing
        {AnalysisFact::kStringTypes},    // provides
        {},                              // invalidates nothing
    });

    PassId::kArrayTypes = reg.registerPass({
        0,
        "array_types",
        "Pre-analyse array return types and parameter types across all functions",
        PassPhase::Preprocessing,
        PassKind::Analysis,
        {},
        {AnalysisFact::kArrayTypes},
        {},
    });

    PassId::kConstantReturns = reg.registerPass({
        0,
        "constant_returns",
        "Detect zero-parameter functions that always return a compile-time constant",
        PassPhase::EvaluationAnalysis,
        PassKind::Analysis,
        {},
        {AnalysisFact::kConstantReturns},
        {},
    });

    PassId::kPurity = reg.registerPass({
        0,
        "purity",
        "Auto-detect pure user functions (no I/O, no global mutation)",
        PassPhase::EvaluationAnalysis,
        PassKind::Analysis,
        {AnalysisFact::kConstantReturns},
        {AnalysisFact::kPurity},
        {},
    });

    PassId::kEffects = reg.registerPass({
        0,
        "effects",
        "Infer function effect summaries (readsMemory/writesMemory/hasIO/hasMutation)",
        PassPhase::EvaluationAnalysis,
        PassKind::Analysis,
        {AnalysisFact::kPurity},
        {AnalysisFact::kEffects},
        {},
    });

    PassId::kERSL = reg.registerPass({
        0,
        "ersl",
        "Effect Refinement & Speculation Layer: derive stability, idempotence, "
        "escape class, and max-safe-optimization-level from FunctionEffects",
        PassPhase::EvaluationAnalysis,
        PassKind::Analysis,
        // ERSL requires effects to have been inferred first.
        {AnalysisFact::kEffects},
        {AnalysisFact::kERSL},
        // Pure analysis — does not modify the AST, invalidates nothing.
        {},
    });

    PassId::kSynthesis = reg.registerPass({
        0,
        "synthesis",
        "Replace std::synthesize bodies with synthesized expression trees",
        PassPhase::EvaluationAnalysis,
        PassKind::SemanticTransform,
        {AnalysisFact::kPurity, AnalysisFact::kEffects},
        {AnalysisFact::kSynthesis},
        {AnalysisFact::kConstantReturns, AnalysisFact::kPurity, AnalysisFact::kEffects},
    });

    PassId::kCFCTRE = reg.registerPass({
        0,
        "cfctre",
        "Cross-function compile-time reasoning: memoised concrete evaluation + abstract interpretation",
        PassPhase::EvaluationAnalysis,
        PassKind::Analysis,
        {AnalysisFact::kPurity, AnalysisFact::kEffects, AnalysisFact::kSynthesis},
        {AnalysisFact::kCFCTRE},
        {},
    });

    PassId::kEGraph = reg.registerPass({
        0,
        "egraph",
        "E-graph equality saturation: algebraic simplification + constant folding at AST level",
        PassPhase::ASTTransform,
        PassKind::SemanticTransform,
        {AnalysisFact::kCFCTRE},
        {AnalysisFact::kEGraph},
        // E-graph rewrites change expressions; any fact derived from expression
        // shapes (ranges, CSE candidates) is now stale.  CF-CTRE purity and
        // effect facts remain valid since the transformations are
        // semantics-preserving.
        {},
    });

    PassId::kRangeAnalysis = reg.registerPass({
        0,
        "range_analysis",
        "Synthesize integer return-value ranges from CFCTRE uniform-return and constIntReturn facts",
        PassPhase::ASTTransform,
        PassKind::Analysis,
        {AnalysisFact::kPurity, AnalysisFact::kEffects, AnalysisFact::kCFCTRE},
        {AnalysisFact::kRangeAnalysis},
        {},
    });

    PassId::kRLC = reg.registerPass({
        0,
        "rlc",
        "Region Lifetime Coalescing: merge temporally disjoint newRegion() pairs to reduce allocation overhead",
        PassPhase::ASTTransform,
        PassKind::SemanticTransform,
        {AnalysisFact::kEffects},           // requires effects analysis
        {AnalysisFact::kRLC},               // provides rlc fact
        {AnalysisFact::kPurity, AnalysisFact::kEffects, AnalysisFact::kCFCTRE}, // invalidates after AST mutation
    });

    PassId::kDCE = reg.registerPass({
        0,
        "dce",
        "Dead Code Elimination: remove unreachable branches from constant-condition ifs/whiles and prune post-return stmts",
        PassPhase::ASTTransform,
        PassKind::CostTransform,
        // DCE benefits from CFCTRE having folded constants into the AST, but
        // operates on literal constants alone and is safe to run earlier.
        {AnalysisFact::kCFCTRE},
        {AnalysisFact::kDCE},
        // Removing branches may invalidate range analysis results.
        {AnalysisFact::kRangeAnalysis},
    });

    PassId::kCSE = reg.registerPass({
        0,
        "cse",
        "Common Subexpression Elimination: hoist repeated pure binary subexpressions to compiler-managed temps",
        PassPhase::ASTTransform,
        PassKind::CostTransform,
        // CSE introduces new VarDecl nodes; run after DCE so dead code does
        // not generate spurious CSE candidates.
        {AnalysisFact::kDCE},
        {AnalysisFact::kCSE},
        // Introduces new variable declarations — invalidates any fact that
        // tracks exact variable counts or live ranges.
        {},
    });

    PassId::kAlgSimp = reg.registerPass({
        0,
        "alg_simp",
        "Algebraic Simplification: identity-element folding (x+0→x, x*1→x, x*0→0, x&&false→0, !!x→x, etc.)",
        PassPhase::ASTTransform,
        PassKind::CostTransform,
        // Run after CFCTRE so that constant folding has already simplified
        // sub-expressions, maximising the chance of a literal operand match.
        // DCE runs first to avoid simplifying dead branches.
        {AnalysisFact::kCFCTRE, AnalysisFact::kDCE},
        {AnalysisFact::kAlgSimp},
        // AlgSimp replaces expressions; any shape-derived facts are stale.
        {AnalysisFact::kRangeAnalysis},
    });

    PassId::kCopyProp = reg.registerPass({
        0,
        "copy_prop",
        "Copy Propagation: inline trivial var-alias copies (var y = x → substitute x at uses of y)",
        PassPhase::ASTTransform,
        PassKind::SemanticTransform,
        // Run after CFCTRE + DCE so that only reachable, non-folded copies
        // are processed.  AlgSimp runs first so that identity simplifications
        // have already been applied, potentially producing more copy candidates.
        {AnalysisFact::kCFCTRE, AnalysisFact::kDCE, AnalysisFact::kAlgSimp},
        {AnalysisFact::kCopyProp},
        // Substituting identifiers changes the shape of expressions; CSE,
        // range analysis, and any name-based facts are potentially stale.
        {AnalysisFact::kCSE, AnalysisFact::kRangeAnalysis},
    });

    PassId::kWidthLegalization = reg.registerPass({
        0,
        "width_legalization",
        "Width tracking and legalization: compute exact semantic bit-widths for all expressions, "
        "then snap them to hardware-friendly storage sizes (8/16/32/64/multiples-of-64) before codegen",
        PassPhase::ASTTransform,
        PassKind::Analysis,
        {AnalysisFact::kRangeAnalysis, AnalysisFact::kCopyProp, AnalysisFact::kAlgSimp},
        {AnalysisFact::kWidthLegalization},
        // Pure analysis — does not modify the AST, invalidates nothing.
        {},
    });

    PassId::kWidthOpt = reg.registerPass({
        0,
        "width_opt",
        "Width-aware optimizations: masking elimination (x & M → x when M covers all bits of x), "
        "shift narrowing/zeroing (x >> N → 0 when N >= width(x)), "
        "and impossible-branch pruning via value-range analysis",
        PassPhase::ASTTransform,
        PassKind::CostTransform,
        // Requires width legalization to have computed semantic widths.
        {AnalysisFact::kWidthLegalization},
        {AnalysisFact::kWidthOpt},
        // This pass rewrites AST expressions; invalidate dependent analyses.
        {AnalysisFact::kRangeAnalysis, AnalysisFact::kWidthLegalization,
         AnalysisFact::kCSE},
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// OptimizationOrchestrator
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// The standard analysis dependency graph, shared by every pipeline run.
/// Created once at first call and reused; the non-owning pointers in
/// AnalysisValidity and AnalysisCache point into this static.
const AnalysisDependencyGraph& defaultDepGraph() noexcept {
    static const AnalysisDependencyGraph g = AnalysisDependencyGraph::createDefault();
    return g;
}

} // anonymous namespace

OptimizationOrchestrator::OptimizationOrchestrator(OptimizationLevel optLevel,
                                                     bool verbose,
                                                     CodeGenerator* codegen,
                                                     OptimizationManager* manager) noexcept
    : optLevel_(optLevel), verbose_(verbose), codegen_(codegen), manager_(manager) {
    registerAllPasses();
}

// ── buildDispatch ─────────────────────────────────────────────────────────────
//
// Returns the per-pass runner map used by both runPassPipeline and runToProvide.
// Centralised here so the 10-entry table only appears once; the two callers
// just call buildDispatch() and pass the result along.

std::unordered_map<uint32_t, PassScheduler::Runner>
OptimizationOrchestrator::buildDispatch() {
    using R = PassScheduler::Runner;
    return {
        {PassId::kPreflightCheck,    R([this](Program* p, OptimizationContext& c){ runPreflightCheck(p, c); })},
        {PassId::kStringTypes,       R([this](Program* p, OptimizationContext& c){ runStringTypes(p, c); })},
        {PassId::kArrayTypes,        R([this](Program* p, OptimizationContext& c){ runArrayTypes(p, c); })},
        {PassId::kConstantReturns,   R([this](Program* p, OptimizationContext& c){ runConstantReturns(p, c); })},
        {PassId::kPurity,            R([this](Program* p, OptimizationContext& c){ runPurity(p, c); })},
        {PassId::kEffects,           R([this](Program* p, OptimizationContext& c){ runEffects(p, c); })},
        {PassId::kERSL,              R([this](Program* p, OptimizationContext& c){ runERSL(p, c); })},
        {PassId::kSynthesis,         R([this](Program* p, OptimizationContext& c){ runSynthesis(p, c); })},
        {PassId::kCFCTRE,            R([this](Program* p, OptimizationContext& c){ runCFCTRE(p, c); })},
        {PassId::kEGraph,            R([this](Program* p, OptimizationContext& c){ runEGraph(p, c); })},
        {PassId::kRangeAnalysis,     R([this](Program* p, OptimizationContext& c){ runRangeAnalysis(p, c); })},
        {PassId::kRLC,               R([this](Program* p, OptimizationContext& c){ runRLC(p, c); })},
        {PassId::kDCE,               R([this](Program* p, OptimizationContext& c){ runDCE(p, c); })},
        {PassId::kCSE,               R([this](Program* p, OptimizationContext& c){ runCSE(p, c); })},
        {PassId::kAlgSimp,           R([this](Program* p, OptimizationContext& c){ runAlgSimp(p, c); })},
        {PassId::kCopyProp,          R([this](Program* p, OptimizationContext& c){ runCopyProp(p, c); })},
        {PassId::kWidthLegalization, R([this](Program* p, OptimizationContext& c){ runWidthLegalization(p, c); })},
        {PassId::kWidthOpt,          R([this](Program* p, OptimizationContext& c){ runWidthOpt(p, c); })},
    };
}

// ── makeScheduler — shared scheduler construction helper ─────────────────────

PassScheduler OptimizationOrchestrator::makeScheduler(OptimizationContext& ctx) {
    PassScheduler s = manager_ ? manager_->createScheduler(ctx) : PassScheduler(ctx);
#ifndef NDEBUG
    if (!manager_) s.setStrictMode(true);
#endif
    return s;
}

// ── runPrepasses ─────────────────────────────────────────────────────────────
//
// Runs every pre-pass in dependency order, delegating to runPassPipeline.
// The pass execution order is determined by PassRegistry::topologicalOrder()
// from the declared requires/provides metadata — no hardcoded sequence.

void OptimizationOrchestrator::runPrepasses(Program* program, OptimizationContext& ctx) {
    stats_ = {};
    runPassPipeline(program, ctx, /*skipValid=*/false);
    syncFactsToContext(program, ctx);
}

void OptimizationOrchestrator::runInvalidated(Program* program, OptimizationContext& ctx) {
    stats_ = {};
    runPassPipeline(program, ctx, /*skipValid=*/true);
    syncFactsToContext(program, ctx);
}

// ── runToProvide ──────────────────────────────────────────────────────────────
//
// Demand-driven entry point: runs the minimum set of passes needed to ensure
// that factKey is valid in ctx.  Delegates to PassScheduler::runToProvide()
// using the shared dispatch map built by buildDispatch().

bool OptimizationOrchestrator::runToProvide(const std::string& factKey,
                                             Program* program,
                                             OptimizationContext& ctx) {
    const auto dispatch = buildDispatch();
    ctx.setDependencyGraph(&defaultDepGraph());
    PassScheduler scheduler = makeScheduler(ctx);
    const bool ok = scheduler.runToProvide(factKey, program, dispatch);
    if (ok) syncFactsToContext(program, ctx);
    return ok;
}

// ── runPassPipeline ───────────────────────────────────────────────────────────
//
// Core driver shared by runPrepasses and runInvalidated.
//
// Algorithm:
//   1. Ask PassRegistry for a topologically-sorted list of pass IDs.
//   2. Build a dispatch map: PassId → per-pass wrapper lambda.
//   3. For each pass in order:
//        a. If skipValid and all provided facts are already valid → skip.
//        b. Verify preconditions (required facts): emit a warning if any
//           required fact is not yet valid (programming-error guard).
//        c. Call the wrapper, timing it with a steady_clock wall-clock.
//        d. Record the pass name + elapsed time in stats_.passTimings.
//   4. Accumulate stats_.totalElapsedMs across the whole pipeline.
//   5. When verbose, print a per-pass timing summary table.

void OptimizationOrchestrator::runPassPipeline(Program* program,
                                                OptimizationContext& ctx,
                                                bool skipValid) {
    // ── Dispatch map ─────────────────────────────────────────────────────
    const auto dispatch = buildDispatch();

    const auto& reg   = PassRegistry::instance();
    const auto  order = reg.topologicalOrder(); // dependency-sorted IDs

    // Install the standard analysis dependency graph so that invalidating one
    // fact automatically cascades to all facts computed from it.
    ctx.setDependencyGraph(&defaultDepGraph());

    // Construct a PassScheduler for this pipeline run.
    // When an OptimizationManager is available, use it as the factory so the
    // scheduler inherits the manager's strict-mode policy.  Otherwise fall back
    // to a standalone scheduler constructed from ctx.
    PassScheduler scheduler = makeScheduler(ctx);

    const auto tPipelineStart = std::chrono::steady_clock::now();

    for (uint32_t id : order) {
        const PassMetadata* meta = reg.find(id);
        if (!meta) continue;

        auto it = dispatch.find(id);
        if (it == dispatch.end()) continue; // pass has no wrapper (IR-level pass, etc.)

        // ── Skip already-valid passes (runInvalidated mode) ────────────
        if (skipValid && scheduler.allProvidedValid(*meta)) {
            ++stats_.passesSkipped;
            continue;
        }

        // ── Precondition check & recovery ───────────────────────────────
        // checkPreconditions() warns (or asserts in strict mode) if any
        // required fact is invalid.  This commonly happens when an earlier
        // pass in the topological order invalidates a fact that this pass
        // requires (e.g. `synthesis` invalidates `purity`/`effects`, but
        // `cfctre` requires both).  Rather than skip the pass — which would
        // leave the produced fact invalid and silently disable downstream
        // analyses — we attempt to recompute the missing prerequisites
        // on-demand via the demand-driven scheduler.
        //
        // Use a fixed-point loop: satisfying one prerequisite (e.g. synthesis)
        // may cascade-invalidate another (e.g. purity), so we repeat until
        // all prerequisites are simultaneously valid.
        bool prereqsOk = true;
        static constexpr int kMaxPrereqRounds = 8;
        for (int round = 0; round < kMaxPrereqRounds; ++round) {
            bool allValid = true;
            for (const char* req : meta->requires_) {
                if (ctx.validity().isValid(req)) continue;
                allValid = false;
                // Try to satisfy the missing requirement by re-running its producer.
                if (!scheduler.runToProvide(req, program, dispatch)) {
                    prereqsOk = false;
                    break;
                }
            }
            if (!prereqsOk || allValid) break;
        }
        if (!prereqsOk || !scheduler.checkPreconditions(*meta)) {
            ++stats_.passesSkipped;
            continue;
        }

        // ── Time and run the pass ──────────────────────────────────────
        const auto tStart = std::chrono::steady_clock::now();
        it->second(program, ctx);
        const auto tEnd   = std::chrono::steady_clock::now();
        const double ms   = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

        // ── Apply invalidation ─────────────────────────────────────────
        // After a transformation pass (e.g. synthesis) modifies the AST,
        // the facts it declared in invalidates_ must be marked stale so
        // that any downstream pass or code-generation query that reads those
        // facts will see them as needing recomputation.  Without this step,
        // purity/effects facts computed before synthesis remain marked as
        // valid even though the synthesized AST may have different semantics.
        scheduler.applyInvalidation(*meta);

        stats_.passTimings.push_back({std::string(meta->name), ms});
        ++stats_.passesRun;
        // Note: stats_.passesRun is now incremented here instead of inside
        // each per-pass wrapper; the wrappers no longer touch stats_.
    }

    const auto tPipelineEnd = std::chrono::steady_clock::now();
    stats_.totalElapsedMs =
        std::chrono::duration<double, std::milli>(tPipelineEnd - tPipelineStart).count();

    // ── Verbose timing summary ─────────────────────────────────────────
    if (verbose_) {
        std::cout << "  [opt] Pre-pass pipeline: "
                  << stats_.passesRun    << " ran, "
                  << stats_.passesSkipped << " skipped, "
                  << stats_.totalElapsedMs << " ms total\n";
        for (const auto& pt : stats_.passTimings) {
            std::cout << "  [opt]   " << pt.name << ": " << pt.elapsedMs << " ms\n";
        }
    }
}

// ── Per-pass wrappers — delegate to CodeGenerator, mark fact valid ────────────

void OptimizationOrchestrator::runStringTypes(Program* program, OptimizationContext& ctx) {
    codegen_->preAnalyzeStringTypes(program);
    ctx.validity().stringTypes = true;
}

void OptimizationOrchestrator::runArrayTypes(Program* program, OptimizationContext& ctx) {
    codegen_->preAnalyzeArrayTypes(program);
    ctx.validity().arrayTypes = true;
}

void OptimizationOrchestrator::runConstantReturns(Program* program, OptimizationContext& ctx) {
    codegen_->analyzeConstantReturnValues(program);
    ctx.validity().constantReturns = true;
}

void OptimizationOrchestrator::runPurity(Program* program, OptimizationContext& ctx) {
    codegen_->autoDetectConstEvalFunctions(program);
    // Immediately derive isPure from isConstFoldable so that downstream passes
    // (CFCTRE, EGraph) can query ctx.allFacts()[name].isPure without waiting
    // for syncFactsToContext to run at the end of the pipeline.
    // Effects haven't been inferred yet at this point, so we only use the
    // isConstFoldable flag here; syncFactsToContext will refine with effects later.
    forEachFunction(program, [&](FunctionDecl* fn) {
        FunctionFacts& ff = ctx.mutableFacts(fn->name);
        if (ff.isConstFoldable)
            ff.isPure = true;
    });
    ctx.validity().purity = true;
}

void OptimizationOrchestrator::runPreflightCheck(Program* program,
                                                   OptimizationContext& ctx) {
    if (!program) {
        ctx.validity().preflightCheck = true;
        return;
    }

    // Walk every expression in every function body looking for fatal errors.
    // Currently detects:
    //   • Literal integer division by zero  (x / 0)
    //   • Literal integer modulo by zero    (x % 0)
    //
    // The check is intentionally simple and conservative: only literal-zero
    // right-hand operands are flagged.  Dynamic division-by-zero is a runtime
    // concern handled by the existing bounds-check infrastructure.

    std::function<void(const Expression*, const std::string&)> checkExpr;
    std::function<void(const Statement*,  const std::string&)> checkStmt;

    checkExpr = [&](const Expression* e, const std::string& fnName) {
        if (!e) return;
        if (e->type == ASTNodeType::BINARY_EXPR) {
            const auto* bin = static_cast<const BinaryExpr*>(e);
            // Check for literal zero divisor.
            if ((bin->op == "/" || bin->op == "%") && bin->right &&
                isIntLiteralVal(bin->right.get(), 0)) {
                Diagnostic d;
                d.severity = DiagnosticSeverity::Error;
                d.code     = ErrorCode::E011_DIVISION_BY_ZERO;
                d.location = {fnName, 0, 0};
                d.message  = "division by zero (literal 0 as "
                             + std::string(bin->op == "/" ? "divisor" : "modulus")
                             + ") in function '" + fnName + "'";
                throw DiagnosticError(d);
            }
            // Recurse into both sides.
            checkExpr(bin->left.get(),  fnName);
            checkExpr(bin->right.get(), fnName);
            return;
        }
        // Recurse into common expression shapes.
        switch (e->type) {
        case ASTNodeType::UNARY_EXPR:
            checkExpr(static_cast<const UnaryExpr*>(e)->operand.get(), fnName); break;
        case ASTNodeType::PREFIX_EXPR:
            checkExpr(static_cast<const PrefixExpr*>(e)->operand.get(), fnName); break;
        case ASTNodeType::POSTFIX_EXPR:
            checkExpr(static_cast<const PostfixExpr*>(e)->operand.get(), fnName); break;
        case ASTNodeType::TERNARY_EXPR: {
            const auto* t = static_cast<const TernaryExpr*>(e);
            checkExpr(t->condition.get(), fnName);
            checkExpr(t->thenExpr.get(),  fnName);
            checkExpr(t->elseExpr.get(),  fnName); break;
        }
        case ASTNodeType::CALL_EXPR:
            for (const auto& a : static_cast<const CallExpr*>(e)->arguments)
                checkExpr(a.get(), fnName);
            break;
        case ASTNodeType::INDEX_EXPR: {
            const auto* i = static_cast<const IndexExpr*>(e);
            checkExpr(i->array.get(), fnName); checkExpr(i->index.get(), fnName); break;
        }
        case ASTNodeType::ASSIGN_EXPR:
            checkExpr(static_cast<const AssignExpr*>(e)->value.get(), fnName); break;
        default: break;
        }
    };

    checkStmt = [&](const Statement* s, const std::string& fnName) {
        if (!s) return;
        switch (s->type) {
        case ASTNodeType::BLOCK:
            for (const auto& st : static_cast<const BlockStmt*>(s)->statements)
                checkStmt(st.get(), fnName);
            break;
        case ASTNodeType::VAR_DECL:
            checkExpr(static_cast<const VarDecl*>(s)->initializer.get(), fnName); break;
        case ASTNodeType::MOVE_DECL:
            checkExpr(static_cast<const MoveDecl*>(s)->initializer.get(), fnName); break;
        case ASTNodeType::RETURN_STMT:
            checkExpr(static_cast<const ReturnStmt*>(s)->value.get(), fnName); break;
        case ASTNodeType::EXPR_STMT:
            checkExpr(static_cast<const ExprStmt*>(s)->expression.get(), fnName); break;
        case ASTNodeType::IF_STMT: {
            const auto* i = static_cast<const IfStmt*>(s);
            checkExpr(i->condition.get(), fnName);
            checkStmt(i->thenBranch.get(), fnName);
            checkStmt(i->elseBranch.get(), fnName); break;
        }
        case ASTNodeType::WHILE_STMT: {
            const auto* w = static_cast<const WhileStmt*>(s);
            checkExpr(w->condition.get(), fnName); checkStmt(w->body.get(), fnName); break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            const auto* d = static_cast<const DoWhileStmt*>(s);
            checkStmt(d->body.get(), fnName); checkExpr(d->condition.get(), fnName); break;
        }
        case ASTNodeType::FOR_STMT: {
            const auto* f = static_cast<const ForStmt*>(s);
            checkExpr(f->start.get(), fnName); checkExpr(f->end.get(), fnName);
            checkExpr(f->step.get(),  fnName); checkStmt(f->body.get(), fnName); break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            const auto* fe = static_cast<const ForEachStmt*>(s);
            checkExpr(fe->collection.get(), fnName); checkStmt(fe->body.get(), fnName); break;
        }
        case ASTNodeType::SWITCH_STMT: {
            const auto* sw = static_cast<const SwitchStmt*>(s);
            checkExpr(sw->condition.get(), fnName);
            for (const auto& c : sw->cases) {
                if (c.value) checkExpr(c.value.get(), fnName);
                for (const auto& v : c.values) checkExpr(v.get(), fnName);
                for (const auto& st : c.body) checkStmt(st.get(), fnName);
            }
            break;
        }
        default: break;
        }
    };

    forEachFunction(program, [&](const FunctionDecl* fn) {
        checkStmt(fn->body.get(), fn->name);
    });
    // Also check global variable initializers.
    for (const auto& g : program->globals) {
        if (g && g->initializer)
            checkExpr(g->initializer.get(), "<global>");
    }

    ctx.validity().preflightCheck = true;
}

void OptimizationOrchestrator::runEffects(Program* program, OptimizationContext& ctx) {
    codegen_->inferFunctionEffects(program);
    ctx.validity().effects = true;
}

void OptimizationOrchestrator::runERSL(Program* program, OptimizationContext& ctx) {
    // Derive EffectSummary for every function from its already-inferred FunctionEffects.
    // This is a pure computation: no AST modification, no LLVM analysis required.
    if (program) {
        for (const auto& func : program->functions) {
            if (!func) continue;
            FunctionFacts& ff = ctx.mutableFacts(func->name);
            ff.ersl = deriveEffectSummary(ff.effects);
        }
    }
    ctx.validity().ersl = true;
}

void OptimizationOrchestrator::runSynthesis(Program* program, OptimizationContext& ctx) {
    codegen_->runSynthesisPass(program, verbose_);
    ctx.validity().synthesis = true;
}

void OptimizationOrchestrator::runCFCTRE(Program* program, OptimizationContext& ctx) {
    codegen_->runCFCTRE(program);
    ctx.validity().cfctre = true;
}

void OptimizationOrchestrator::runEGraph(Program* program, OptimizationContext& ctx) {
    // Populate pure-user-functions so algebraic rules can fire across call boundaries.
    std::unordered_set<std::string> pureUserFuncs;
    for (const auto& [name, ff] : ctx.allFacts()) {
        if (ff.isPure) pureUserFuncs.insert(name);
    }
    ctx.egraph().setPureUserFuncs(std::move(pureUserFuncs));
    codegen_->runEGraphPass(program, ctx);
    ctx.validity().egraph = true;
}

void OptimizationOrchestrator::runRangeAnalysis(Program* program, OptimizationContext& ctx) {
    // Synthesize ValueRange facts from CFCTRE results and AST return-expression analysis.
    if (!program) {
        ctx.validity().rangeAnalysis = true;
        return;
    }

    // Helper: try to read a literal int from an expression.
    auto litInt = [](const Expression* e) -> std::optional<int64_t> {
        long long v = 0;
        if (isIntLiteral(e, &v)) return static_cast<int64_t>(v);
        return {};
    };

    // Try to compute a ValueRange for a single return expression.
    // Returns nullopt when no bound can be proven.
    std::function<std::optional<ValueRange>(const Expression*)> exprRange;
    exprRange = [&](const Expression* e) -> std::optional<ValueRange> {
        if (!e) return {};

        // Literal integer → exact point range.
        if (auto v = litInt(e))
            return ValueRange{*v, *v};

        // Unary negation of a literal: -N → [-N, -N]
        if (e->type == ASTNodeType::UNARY_EXPR) {
            auto* ue = static_cast<const UnaryExpr*>(e);
            if (ue->op == "-") {
                if (auto v = litInt(ue->operand.get()))
                    return ValueRange{-*v, -*v};
            }
        }

        if (e->type == ASTNodeType::BINARY_EXPR) {
            auto* bin = static_cast<const BinaryExpr*>(e);
            // return expr % C → [0, C-1]  (when C is a positive literal)
            if (bin->op == "%") {
                if (auto c = litInt(bin->right.get())) {
                    if (*c > 0) return ValueRange{0, *c - 1};
                }
            }
            // return a + b → join(range(a), range(b)) only when both are in [0, N]
            // (conservative: only emit if we know both sides are non-negative constants
            //  or one side is a non-negative constant and the other is non-negative)
            if (bin->op == "+") {
                auto lr = exprRange(bin->left.get());
                auto rr = exprRange(bin->right.get());
                if (lr && rr && lr->lo >= 0 && rr->lo >= 0) {
                    // Sum of two non-negative ranges.
                    // Overflow guard: cap at INT64_MAX.
                    int64_t lo = lr->lo + rr->lo;
                    int64_t hi = (rr->hi <= std::numeric_limits<int64_t>::max() - lr->hi)
                                     ? lr->hi + rr->hi
                                     : std::numeric_limits<int64_t>::max();
                    return ValueRange{lo, hi};
                }
            }
        }

        if (e->type == ASTNodeType::CALL_EXPR) {
            auto* call = static_cast<const CallExpr*>(e);
            // abs(x) → [0, INT64_MAX]
            if (call->callee == "abs" && call->arguments.size() == 1)
                return ValueRange{0, std::numeric_limits<int64_t>::max()};
            // clamp(x, lo, hi) → [lo, hi]
            if (call->callee == "clamp" && call->arguments.size() == 3) {
                auto lo = litInt(call->arguments[1].get());
                auto hi = litInt(call->arguments[2].get());
                if (lo && hi && *lo <= *hi)
                    return ValueRange{*lo, *hi};
            }
            // max(0, x) or max(x, 0) → [0, INT64_MAX]
            if (call->callee == "max" && call->arguments.size() == 2) {
                auto lo0 = litInt(call->arguments[0].get());
                auto lo1 = litInt(call->arguments[1].get());
                if ((lo0 && *lo0 == 0) || (lo1 && *lo1 == 0))
                    return ValueRange{0, std::numeric_limits<int64_t>::max()};
            }
            // min(C, x) or min(x, C) for a positive C → [-INT64_MAX, C]
            if (call->callee == "min" && call->arguments.size() == 2) {
                auto c0 = litInt(call->arguments[0].get());
                auto c1 = litInt(call->arguments[1].get());
                if (c0) return ValueRange{std::numeric_limits<int64_t>::min(), *c0};
                if (c1) return ValueRange{std::numeric_limits<int64_t>::min(), *c1};
            }
        }

        return {}; // unknown
    };

    // Collect all return expressions from a function body (recursively).
    // Also sets `hasUnboundReturn` if any return expression can't be bounded.
    std::function<void(const Statement*, std::vector<const Expression*>&, bool&)>
        collectReturns = [&](const Statement* s,
                             std::vector<const Expression*>& rets,
                             bool& hasUnboundReturn) {
        if (!s) return;
        switch (s->type) {
        case ASTNodeType::RETURN_STMT: {
            auto* rs = static_cast<const ReturnStmt*>(s);
            if (rs->value) rets.push_back(rs->value.get());
            else hasUnboundReturn = true; // void return
            break;
        }
        case ASTNodeType::BLOCK: {
            auto* blk = static_cast<const BlockStmt*>(s);
            for (const auto& child : blk->statements)
                collectReturns(child.get(), rets, hasUnboundReturn);
            break;
        }
        case ASTNodeType::IF_STMT: {
            auto* ifs = static_cast<const IfStmt*>(s);
            collectReturns(ifs->thenBranch.get(), rets, hasUnboundReturn);
            collectReturns(ifs->elseBranch.get(), rets, hasUnboundReturn);
            break;
        }
        case ASTNodeType::WHILE_STMT:
            collectReturns(static_cast<const WhileStmt*>(s)->body.get(), rets, hasUnboundReturn);
            break;
        case ASTNodeType::DO_WHILE_STMT:
            collectReturns(static_cast<const DoWhileStmt*>(s)->body.get(), rets, hasUnboundReturn);
            break;
        case ASTNodeType::FOR_STMT:
            collectReturns(static_cast<const ForStmt*>(s)->body.get(), rets, hasUnboundReturn);
            break;
        case ASTNodeType::FOR_EACH_STMT:
            collectReturns(static_cast<const ForEachStmt*>(s)->body.get(), rets, hasUnboundReturn);
            break;
        default:
            break;
        }
    };

    for (const auto& func : program->functions) {
        FunctionFacts& ff = ctx.mutableFacts(func->name);
        if (ff.returnRange.has_value()) continue; // already populated

        // Phase 1: point range from known-constant return (CFCTRE result).
        if (ff.constIntReturn.has_value()) {
            ff.returnRange = ValueRange{*ff.constIntReturn, *ff.constIntReturn};
            continue;
        }
        if (ff.uniformCTReturn.has_value() && ff.uniformCTReturn->isInt()) {
            const int64_t v = ff.uniformCTReturn->asI64();
            ff.returnRange = ValueRange{v, v};
            continue;
        }

        // Phase 2: structural analysis across ALL return statements.
        if (!func->body || func->body->statements.empty()) continue;

        std::vector<const Expression*> retExprs;
        bool hasUnboundReturn = false;
        for (const auto& s : func->body->statements)
            collectReturns(s.get(), retExprs, hasUnboundReturn);

        if (retExprs.empty() || hasUnboundReturn) continue;

        // Compute range for each return expression and join them.
        // If any return can't be bounded, the whole function range is unknown.
        std::optional<ValueRange> joined;
        bool allBounded = true;
        for (const Expression* re : retExprs) {
            auto rng = exprRange(re);
            if (!rng) { allBounded = false; break; }
            joined = joined ? std::optional(ValueRange::join(*joined, *rng)) : rng;
        }

        if (allBounded && joined && joined->isNarrowed())
            ff.returnRange = *joined;
    }
    ctx.validity().rangeAnalysis = true;
}

void OptimizationOrchestrator::runRLC(Program* program, OptimizationContext& ctx) {
    codegen_->runRLCPass(program, verbose_);
    ctx.validity().rlc = true;
}

void OptimizationOrchestrator::runDCE(Program* program, OptimizationContext& ctx) {
    runDCEPass(program, verbose_);
    ctx.validity().dce = true;
    // DCE removes code branches; range analysis results may be stale.
    ctx.validity().rangeAnalysis = false;
}

void OptimizationOrchestrator::runCSE(Program* program, OptimizationContext& ctx) {
    // Build the idempotent-function table from ERSL facts so the CSE pass can
    // also eliminate duplicate calls to stable, canDuplicate user functions.
    // If ERSL hasn't run yet (ersl validity flag is false), fall back to the
    // binary-only mode by passing nullptr.
    if (ctx.validity().ersl && program) {
        std::unordered_map<std::string, EffectSummary> idempotent;
        for (const auto& func : program->functions) {
            if (!func) continue;
            const EffectSummary& es = ctx.effectSummary(func->name);
            if (es.canDuplicate)
                idempotent.emplace(func->name, es);
        }
        runCSEPass(program, verbose_, &idempotent);
    } else {
        runCSEPass(program, verbose_, nullptr);
    }
    ctx.validity().cse = true;
}

void OptimizationOrchestrator::runAlgSimp(Program* program, OptimizationContext& ctx) {
    runAlgSimpPass(program, verbose_);
    ctx.validity().algSimp = true;
    // AlgSimp rewrites expressions; range facts derived from expression shapes
    // may be stale.
    ctx.validity().rangeAnalysis = false;
}

void OptimizationOrchestrator::runCopyProp(Program* program, OptimizationContext& ctx) {
    runCopyPropPass(program, verbose_);
    ctx.validity().copyProp = true;
    // CopyProp substitutes identifiers; CSE keys and range values are stale.
    ctx.validity().cse          = false;
    ctx.validity().rangeAnalysis = false;
    // Width legalization depends on expression shapes — re-run after CopyProp.
    ctx.validity().widthLegalization = false;
}

void OptimizationOrchestrator::runWidthLegalization(Program* program,
                                                      OptimizationContext& ctx) {
    WidthLegalizationPass pass(ctx);
    pass.run(program);
    ctx.validity().widthLegalization = true;
    if (verbose_) {
        std::cout << "  [width] Width legalization complete: "
                  << pass.narrowedCount() << " narrowed, "
                  << pass.wideCount()     << " wide (>64-bit) expressions\n";
    }
}

void OptimizationOrchestrator::runWidthOpt(Program* program,
                                            OptimizationContext& ctx) {
    const uint32_t n = runWidthOptPass(program, ctx, verbose_);
    ctx.validity().widthOpt = true;
    if (n > 0) {
        // AST was modified — width legalization and range analysis are stale.
        ctx.validity().widthLegalization = false;
        ctx.validity().rangeAnalysis     = false;
        ctx.validity().cse               = false;
    }
}

// ── syncFactsToContext ────────────────────────────────────────────────────────
// Fill derived facts and CF-CTRE results into OptimizationContext after passes run.

void OptimizationOrchestrator::syncFactsToContext(Program* program,
                                                   OptimizationContext& ctx) const {
    if (!program) return;

    for (const auto& func : program->functions) {
        const std::string& name = func->name;
        FunctionFacts& ff = ctx.mutableFacts(name);

        // Safety-net: assert raw facts — @const_eval functions declared before
        // optCtx_ was live may not have been written by analysis passes.
        if (!ff.isConstFoldable)
            ff.isConstFoldable = codegen_->isConstEvalFunction(name);
        if (!ff.constIntReturn)
            ff.constIntReturn = codegen_->getConstIntReturn(name);
        if (!ff.constStringReturn)
            ff.constStringReturn = codegen_->getConstStringReturn(name);
        if (!ff.effects.readsMemory && !ff.effects.writesMemory &&
            !ff.effects.hasIO      && !ff.effects.hasMutation) {
            const FunctionEffects inferred = codegen_->getFunctionEffects(name);
            if (inferred.readsMemory || inferred.writesMemory ||
                inferred.hasIO       || inferred.hasMutation) {
                ff.effects = inferred;
            }
        }

        // Derived purity.
        ff.isPure = ff.effects.isReadNone() || ff.isConstFoldable;

        // CF-CTRE results.
        if (ctx.ctEngine()) {
            ff.isDead         = ctx.ctEngine()->deadFunctions().count(name) > 0;
            ff.foldedByCFCTRE = ctx.ctEngine()->foldableCallees().count(name) > 0;
            const auto& uniform = ctx.ctEngine()->uniformReturnValues();
            auto it = uniform.find(name);
            if (it != uniform.end()) ff.uniformCTReturn = it->second;
        }
    }
}

} // namespace omscript
