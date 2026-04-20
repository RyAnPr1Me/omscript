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

#include <deque>
#include <cassert>
#include <chrono>
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
        throw std::logic_error("OptimizationOrchestrator: dependency cycle detected in pass registry");
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
// Runs every pre-pass in dependency order, skipping passes whose analysis
// validity flag is already set.  The existing behavior of generate() is
// preserved: the same functions are called in the same logical order.

void OptimizationOrchestrator::runPrepasses(Program* program, OptimizationContext& ctx) {
    stats_ = {};

    // Passes run unconditionally on every compilation (validity starts false).
    runStringTypes    (program, ctx);
    runArrayTypes     (program, ctx);
    runConstantReturns(program, ctx);
    runPurity         (program, ctx);
    runEffects        (program, ctx);
    runSynthesis      (program, ctx);
    runCFCTRE         (program, ctx);
    runEGraph         (program, ctx);

    // After all passes have run, sync the per-function analysis data from
    // CodeGenerator's internal maps back into the unified context.
    syncFactsToContext(program, ctx);
}

void OptimizationOrchestrator::runInvalidated(Program* program, OptimizationContext& ctx) {
    const auto& v = ctx.validity();
    if (!v.stringTypes)     runStringTypes    (program, ctx);
    if (!v.arrayTypes)      runArrayTypes     (program, ctx);
    if (!v.constantReturns) runConstantReturns(program, ctx);
    if (!v.purity)          runPurity         (program, ctx);
    if (!v.effects)         runEffects        (program, ctx);
    if (!v.synthesis)       runSynthesis      (program, ctx);
    if (!v.cfctre)          runCFCTRE         (program, ctx);
    if (!v.egraph)          runEGraph         (program, ctx);
    syncFactsToContext(program, ctx);
}

// ── Per-pass wrappers ─────────────────────────────────────────────────────────
//
// Each wrapper:
//   1. Delegates to the corresponding CodeGenerator method (Phase A behavior).
//   2. Marks the produced fact valid in ctx.validity().
//   3. Increments the statistics counter.
//
// The `ctx` parameter is used only for validity tracking in these Phase A
// wrappers.  In future phases (B/D/E) the wrappers will read pre-conditions
// from ctx (e.g., verifying required facts are present) and write their
// analysis results directly into ctx instead of CodeGenerator's private maps.
// The parameter is retained now (rather than removed) so the signature is
// stable across phases and call sites do not need to change.

void OptimizationOrchestrator::runStringTypes(Program* program, OptimizationContext& ctx) {
    codegen_->preAnalyzeStringTypes(program);
    ctx.validity().stringTypes = true;
    ++stats_.passesRun;
}

void OptimizationOrchestrator::runArrayTypes(Program* program, OptimizationContext& ctx) {
    codegen_->preAnalyzeArrayTypes(program);
    ctx.validity().arrayTypes = true;
    ++stats_.passesRun;
}

void OptimizationOrchestrator::runConstantReturns(Program* program, OptimizationContext& ctx) {
    codegen_->analyzeConstantReturnValues(program);
    ctx.validity().constantReturns = true;
    ++stats_.passesRun;
}

void OptimizationOrchestrator::runPurity(Program* program, OptimizationContext& ctx) {
    codegen_->autoDetectConstEvalFunctions(program);
    ctx.validity().purity = true;
    ++stats_.passesRun;
}

void OptimizationOrchestrator::runEffects(Program* program, OptimizationContext& ctx) {
    codegen_->inferFunctionEffects(program);
    ctx.validity().effects = true;
    ++stats_.passesRun;
}

void OptimizationOrchestrator::runSynthesis(Program* program, OptimizationContext& ctx) {
    codegen_->runSynthesisPass(program, verbose_);
    ctx.validity().synthesis = true;
    ++stats_.passesRun;
}

void OptimizationOrchestrator::runCFCTRE(Program* program, OptimizationContext& ctx) {
    codegen_->runCFCTRE(program);
    ctx.validity().cfctre = true;
    ++stats_.passesRun;
}

void OptimizationOrchestrator::runEGraph(Program* program, OptimizationContext& ctx) {
    // Delegate the level/flag guard to CodeGenerator (it knows enableEGraph_).
    // If the guard passes, CodeGenerator configures the subsystem from its own
    // settings and then calls ctx.egraph().optimizeProgram().
    codegen_->runEGraphPass(program, ctx);
    ctx.validity().egraph = true;
    ++stats_.passesRun;
}

// ── syncFactsToContext ────────────────────────────────────────────────────────
//
// After all pre-passes have run, copy analysis results out of CodeGenerator's
// internal maps into the unified OptimizationContext so the rest of the
// pipeline queries a single surface.

void OptimizationOrchestrator::syncFactsToContext(Program* program,
                                                   OptimizationContext& ctx) const {
    if (!program) return;

    for (const auto& func : program->functions) {
        const std::string& name = func->name;
        FunctionFacts& ff = ctx.mutableFacts(name);

        // ── Const-eval membership ─────────────────────────────────────────
        ff.isConstFoldable = codegen_->isConstEvalFunction(name);

        // ── Constant return values ────────────────────────────────────────
        if (auto v = codegen_->getConstIntReturn(name)) {
            ff.constIntReturn = v;
        }
        if (auto v = codegen_->getConstStringReturn(name)) {
            ff.constStringReturn = v;
        }

        // ── Effect summary ────────────────────────────────────────────────
        ff.effects = codegen_->getFunctionEffects(name);

        // ── Purity: readnone AND constFoldable both qualify as "pure" ─────
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
