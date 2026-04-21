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

#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
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
    passes_.push_back(std::move(meta));
    return passes_.back().id;
}

const PassMetadata* PassRegistry::find(uint32_t id) const noexcept {
    for (const auto& p : passes_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const PassMetadata* PassRegistry::find(const std::string& name) const noexcept {
    for (const auto& p : passes_) {
        if (p.name == name) return &p;
    }
    return nullptr;
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
    uint32_t kStringTypes     = 0;
    uint32_t kArrayTypes      = 0;
    uint32_t kConstantReturns = 0;
    uint32_t kPurity          = 0;
    uint32_t kEffects         = 0;
    uint32_t kSynthesis       = 0;
    uint32_t kCFCTRE          = 0;
    uint32_t kEGraph          = 0;
    uint32_t kRangeAnalysis   = 0;
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
}

// ─────────────────────────────────────────────────────────────────────────────
// OptimizationOrchestrator
// ─────────────────────────────────────────────────────────────────────────────

OptimizationOrchestrator::OptimizationOrchestrator(OptimizationLevel optLevel,
                                                    bool verbose,
                                                    CodeGenerator* codegen) noexcept
    : optLevel_(optLevel), verbose_(verbose), codegen_(codegen) {
    registerAllPasses();
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
    // Maps a stable PassId to the per-pass wrapper that runs the work.
    // Built once per call (IDs are assigned at static-init time, so they are
    // stable for the lifetime of the process).
    using Runner = std::function<void(Program*, OptimizationContext&)>;
    const std::unordered_map<uint32_t, Runner> dispatch = {
        {PassId::kStringTypes,     [this](Program* p, OptimizationContext& c){ runStringTypes(p, c); }},
        {PassId::kArrayTypes,      [this](Program* p, OptimizationContext& c){ runArrayTypes(p, c); }},
        {PassId::kConstantReturns, [this](Program* p, OptimizationContext& c){ runConstantReturns(p, c); }},
        {PassId::kPurity,          [this](Program* p, OptimizationContext& c){ runPurity(p, c); }},
        {PassId::kEffects,         [this](Program* p, OptimizationContext& c){ runEffects(p, c); }},
        {PassId::kSynthesis,       [this](Program* p, OptimizationContext& c){ runSynthesis(p, c); }},
        {PassId::kCFCTRE,          [this](Program* p, OptimizationContext& c){ runCFCTRE(p, c); }},
        {PassId::kEGraph,          [this](Program* p, OptimizationContext& c){ runEGraph(p, c); }},
        {PassId::kRangeAnalysis,   [this](Program* p, OptimizationContext& c){ runRangeAnalysis(p, c); }},
    };

    const auto& reg   = PassRegistry::instance();
    const auto  order = reg.topologicalOrder(); // dependency-sorted IDs

    const auto tPipelineStart = std::chrono::steady_clock::now();

    for (uint32_t id : order) {
        const PassMetadata* meta = reg.find(id);
        if (!meta) continue;

        auto it = dispatch.find(id);
        if (it == dispatch.end()) continue; // pass has no wrapper (IR-level pass, etc.)

        // ── Skip already-valid passes (runInvalidated mode) ────────────
        if (skipValid) {
            bool allProvided = true;
            for (const char* fact : meta->provides_) {
                if (!ctx.validity().isValid(fact)) { allProvided = false; break; }
            }
            if (allProvided) {
                ++stats_.passesSkipped;
                continue;
            }
        }

        // ── Precondition guard ─────────────────────────────────────────
        // If a required fact is not yet valid, the pass ordering is wrong.
        // Emit a warning rather than aborting so release builds are robust.
        for (const char* req : meta->requires_) {
            if (!ctx.validity().isValid(req)) {
                std::cerr << "[omscript][opt] WARNING: pass '" << meta->name
                          << "' requires fact '" << req
                          << "' which is not yet valid — pass ordering may be incorrect\n";
            }
        }

        // ── Time and run the pass ──────────────────────────────────────
        const auto tStart = std::chrono::steady_clock::now();
        it->second(program, ctx);
        const auto tEnd   = std::chrono::steady_clock::now();
        const double ms   = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

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

// ── Per-pass wrappers ─────────────────────────────────────────────────────────
//
// Each wrapper:
//   1. Delegates to the corresponding CodeGenerator method.
//   2. Marks the produced fact valid in ctx.validity().
//   3. Increments the pass counter (done by runPassPipeline, not here).
//
// Phase B: runPassPipeline checks that all required facts are valid before
// calling each wrapper, and records wall-clock timing around the call.
// runPrepasses / runInvalidated are now driven by PassRegistry::topologicalOrder()
// rather than a hardcoded sequence, so adding or reordering passes only
// requires a metadata change in registerAllPasses().

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
    ctx.validity().purity = true;
}

void OptimizationOrchestrator::runEffects(Program* program, OptimizationContext& ctx) {
    codegen_->inferFunctionEffects(program);
    ctx.validity().effects = true;
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
    // Before running the e-graph, populate the pure-user-functions set so
    // the optimizer can include expressions containing calls to those functions.
    // A pure function always returns the same value for the same arguments, so
    // algebraic rules (distributivity, commutativity, etc.) can fire across
    // call boundaries — e.g. 2*f(x) + 3*f(x) → 5*f(x).
    std::unordered_set<std::string> pureUserFuncs;
    for (const auto& [name, ff] : ctx.allFacts()) {
        if (ff.isPure) pureUserFuncs.insert(name);
    }
    ctx.egraph().setPureUserFuncs(std::move(pureUserFuncs));

    // Delegate the level/flag guard to CodeGenerator (it knows enableEGraph_).
    // If the guard passes, CodeGenerator configures the subsystem from its own
    // settings and then calls ctx.egraph().optimizeProgram().
    codegen_->runEGraphPass(program, ctx);
    ctx.validity().egraph = true;
}

void OptimizationOrchestrator::runRangeAnalysis(Program* program, OptimizationContext& ctx) {
    // Synthesize ValueRange facts from CFCTRE results already stored in ctx.
    // This pass does not walk the AST; it derives bounds purely from the
    // uniform-return and constIntReturn facts set by syncFactsToContext.
    if (!program) {
        ctx.validity().rangeAnalysis = true;
        return;
    }
    for (const auto& func : program->functions) {
        FunctionFacts& ff = ctx.mutableFacts(func->name);
        if (ff.returnRange.has_value()) continue; // already populated

        if (ff.constIntReturn.has_value()) {
            const int64_t v = *ff.constIntReturn;
            ff.returnRange = ValueRange{v, v};
        } else if (ff.uniformCTReturn.has_value() && ff.uniformCTReturn->isInt()) {
            const int64_t v = ff.uniformCTReturn->asI64();
            ff.returnRange = ValueRange{v, v};
        }
    }
    ctx.validity().rangeAnalysis = true;
}

// ── syncFactsToContext ────────────────────────────────────────────────────────
//
// Completes the FunctionFacts stored in OptimizationContext by filling in
// derived facts and CF-CTRE results that are not written directly by the
// individual analysis passes.
//
// Note: raw analysis facts (isConstFoldable, constIntReturn, constStringReturn,
// effects) are now written directly into OptimizationContext by the analysis
// passes themselves (Phase F), so this function only needs to:
//   1. Re-assert raw facts from CodeGenerator as a safety net (ensures facts
//      set before optCtx_ was wired up — e.g. @const_eval in generateFunction —
//      are still visible).
//   2. Compute derived facts (isPure) that depend on the raw facts.
//   3. Populate CF-CTRE results (isDead, foldedByCFCTRE, uniformCTReturn) that
//      are only available after runCFCTRE() completes.

void OptimizationOrchestrator::syncFactsToContext(Program* program,
                                                   OptimizationContext& ctx) const {
    if (!program) return;

    for (const auto& func : program->functions) {
        const std::string& name = func->name;
        FunctionFacts& ff = ctx.mutableFacts(name);

        // ── Safety-net: assert raw facts from CodeGenerator accessors ─────
        // Analysis passes now write these directly, but functions declared
        // with @const_eval before optCtx_ was live (e.g. during early IR
        // emission) may not have been written yet.
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

        // ── Derived: purity ───────────────────────────────────────────────
        ff.isPure = ff.effects.isReadNone() || ff.isConstFoldable;

        // ── CF-CTRE: dead-function detection ─────────────────────────────
        if (ctx.ctEngine()) {
            ff.isDead          = ctx.ctEngine()->deadFunctions().count(name) > 0;
            ff.foldedByCFCTRE  = ctx.ctEngine()->foldableCallees().count(name) > 0;

            // Uniform return value (same constant on every call path).
            const auto& uniform = ctx.ctEngine()->uniformReturnValues();
            auto it = uniform.find(name);
            if (it != uniform.end()) {
                ff.uniformCTReturn = it->second;
            }
        }
    }
}

} // namespace omscript
