/// @file optimization_manager.cpp
/// @brief Implementation of OptimizationManager, PassScheduler, LegalityService.

#include "optimization_manager.h"
#include "ersl.h"              // deriveEffectSummary, EffectSummary
#include "opt_orchestrator.h"  // PassRegistry, PassMetadata
#include "superoptimizer.h"    // superopt::instructionCost — used by DefaultCostModel
#include <cassert>
#include <iostream>
#include <unordered_set>

// LLVM headers needed for LegalityService::canTransformFunction.
#include <llvm/IR/Function.h>
#include <llvm/IR/Attributes.h>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// DefaultCostModel — wraps superopt::instructionCost
// ─────────────────────────────────────────────────────────────────────────────
//
// Defined in this .cpp file (not the header) so that optimization_manager.h
// does not need to include superoptimizer.h, avoiding the include cycle:
//   superoptimizer.h → optimization_manager.h → superoptimizer.h
//
// The factory function createDefaultCostModel() is declared in the header and
// returns a std::unique_ptr<CostModel> so callers never see this type.

namespace {

struct DefaultCostModel final : public CostModel {
    double instructionCost(const llvm::Instruction* inst) const override {
        return superopt::instructionCost(inst);
    }
};

} // anonymous namespace

std::unique_ptr<CostModel> createDefaultCostModel() {
    return std::make_unique<DefaultCostModel>();
}

// ─────────────────────────────────────────────────────────────────────────────
// LegalityService
// ─────────────────────────────────────────────────────────────────────────────

LegalityVerdict LegalityService::canTransformLoops(
    const LoopLegalityContext& ctx) const noexcept
{
    if (!ctx.effects) return LegalityVerdict::Unknown;

    // I/O operations (print, file I/O, sleep, …) must not be reordered — a
    // loop that prints every iteration cannot have its iterations interchanged.
    if (ctx.effects->hasIO) return LegalityVerdict::Illegal;

    // hasMutation indicates that the function mutates program state in ways
    // beyond simple array reads/writes (e.g. modifies global variables, calls
    // impure user functions).  Conservative: block all transforms.
    if (ctx.effects->hasMutation) return LegalityVerdict::Illegal;

    // ── ERSL refinement ──────────────────────────────────────────────────────
    // Derive an EffectSummary from the FunctionEffects and use it to tighten
    // or relax the verdict beyond the coarser flag checks above.
    //
    // Key insight: getMaxSafeOptLevel() encodes a total ordering of effect
    // severity.  Anything at level 3+ means no escaping writes and no I/O,
    // so loop transforms are safe (subject to fine-grained dependence analysis
    // in polyopt).
    //
    // Level 2 (global/arg write) → Unknown: we cannot rule out that reordering
    //   the writes would be observable to callers; defer to polyopt.
    // Level 1 (IO/External) → already blocked above.
    const EffectSummary es = deriveEffectSummary(*ctx.effects);
    if (es.maxSafeOptLevel <= 1)
        return LegalityVerdict::Illegal;

    if (es.maxSafeOptLevel == 2) {
        // Writes escape to global/arg: polyopt may still prove legality via
        // direction vectors, but we cannot assert Legal at this level.
        return LegalityVerdict::Unknown;
    }

    // writesMemory alone does NOT prevent loop transforms — dependence analysis
    // in polyopt handles read/write hazards precisely.  We only block here when
    // the writes are coupled to observable I/O or global mutation (handled above).
    return LegalityVerdict::Legal;
}

LegalityVerdict LegalityService::checkLegality(
    LoopTransform transform,
    const LoopLegalityContext& ctx) const noexcept
{
    switch (transform) {
    case LoopTransform::Interchange:
    case LoopTransform::Tiling:
    case LoopTransform::Reversal:
    case LoopTransform::Skewing:
        return canTransformLoops(ctx);

    case LoopTransform::Fusion:
    case LoopTransform::Fission:
        // Fusion and fission require knowledge of the adjacent loop's iteration
        // space (bounds, stride, data dependencies) which is not available at
        // this level.  Defer to polyopt's fine-grained analysis.
        return LegalityVerdict::Unknown;
    }
    return LegalityVerdict::Unknown;
}

LegalityVerdict LegalityService::canTransformFunction(
    const llvm::Function& F) const noexcept
{
    // Use LLVM function attributes as an IR-level proxy for the effect summary.
    // These are set by codegen when inferMemoryEffects() runs after IR emission.
    //
    // nofree:  function does not free memory — no I/O that depends on freeing
    // nosync:  function does not perform synchronising operations (no I/O sync)
    //
    // Together, nofree+nosync is a strong indicator that the function has no
    // observable I/O side effects and loop transforms are likely safe (subject
    // to the fine-grained dependence check in polyopt).
    //
    // Absence of willreturn means the function may not terminate; transforms
    // that change loop trip counts or introduce new loops could change termination
    // behaviour — conservatively Unknown.
    const bool noFree  = F.hasFnAttribute(llvm::Attribute::NoFree);
    const bool noSync  = F.hasFnAttribute(llvm::Attribute::NoSync);
    const bool willRet = F.hasFnAttribute(llvm::Attribute::WillReturn);

    if (!willRet) {
        // Cannot verify termination; be conservative.
        return LegalityVerdict::Unknown;
    }
    if (noFree && noSync) {
        return LegalityVerdict::Legal;
    }
    // Either noFree or noSync is missing: potential I/O/sync effects.
    return LegalityVerdict::Unknown;
}

// ─────────────────────────────────────────────────────────────────────────────
// PassScheduler
// ─────────────────────────────────────────────────────────────────────────────

bool PassScheduler::checkPreconditions(const PassMetadata& meta) const noexcept {
    for (const char* req : meta.requires_) {
        if (!ctx_.validity().isValid(req)) {
            std::cerr << "[omscript][opt] "
                      << (strict_ ? "FATAL: " : "WARNING: ")
                      << "pass '" << meta.name << "' requires fact '" << req
                      << "' which is not valid -- "
                      << (strict_ ? "pass ordering is incorrect" : "skipping pass")
                      << "\n";
            assert(!strict_ && "Pass precondition violated -- see error above");
            return false;
        }
    }
    return true;
}

void PassScheduler::applyInvalidation(const PassMetadata& meta) noexcept {
    for (const char* fact : meta.invalidates_) {
        ctx_.validity().invalidate(fact);
        ctx_.cache().invalidate(fact);
    }
    // Re-mark provides_ valid: they were freshly computed by this pass run.
    for (const char* fact : meta.provides_) {
        ctx_.validity().markValid(fact);
    }
}

bool PassScheduler::allProvidedValid(const PassMetadata& meta) const noexcept {
    for (const char* fact : meta.provides_) {
        if (!ctx_.validity().isValid(fact)) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// PassScheduler::runToProvide — demand-driven fact computation
// ─────────────────────────────────────────────────────────────────────────────

bool PassScheduler::runToProvide(
    const std::string& targetFact,
    Program* program,
    const std::unordered_map<uint32_t, Runner>& dispatch)
{
    std::unordered_set<std::string> inProgress;
    return runToProvideImpl(targetFact, program, dispatch, inProgress);
}

bool PassScheduler::runToProvideImpl(
    const std::string& targetFact,
    Program* program,
    const std::unordered_map<uint32_t, Runner>& dispatch,
    std::unordered_set<std::string>& inProgress)
{
    // Already valid — nothing to do.
    if (ctx_.validity().isValid(targetFact)) return true;

    // Cycle detection: if we are already in the process of computing this
    // fact (from a recursive call), there is a dependency cycle in the pass
    // registry.  Return false rather than infinite recursion.
    if (!inProgress.insert(targetFact).second) {
        std::cerr << "[omscript][opt] CYCLE: circular dependency detected "
                     "while computing fact '" << targetFact << "'\n";
        return false;
    }

    const auto& reg = PassRegistry::instance();

    // Find the pass that produces targetFact.
    const PassMetadata* producer = nullptr;
    for (const auto& meta : reg.all()) {
        for (const char* fact : meta.provides_) {
            if (fact == targetFact) { producer = &meta; break; }
        }
        if (producer) break;
    }

    if (!producer) {
        inProgress.erase(targetFact);
        return false; // no registered producer
    }

    // Recursively satisfy prerequisites.  Use a fixed-point loop because
    // satisfying one prerequisite (e.g. synthesis) may invalidate another
    // (e.g. synthesis->applyInvalidation cascades to invalidate purity).
    // Repeat until all prerequisites are simultaneously valid, or until we
    // exceed a safety cap (which would indicate a real dependency cycle).
    static constexpr int kMaxPrereqRounds = 8;
    for (int round = 0; round < kMaxPrereqRounds; ++round) {
        bool allValid = true;
        for (const char* req : producer->requires_) {
            if (!ctx_.validity().isValid(req)) {
                allValid = false;
                if (!runToProvideImpl(req, program, dispatch, inProgress)) {
                    inProgress.erase(targetFact);
                    return false;
                }
            }
        }
        if (allValid) break;
    }

    // Check the dispatch map before running.
    auto it = dispatch.find(producer->id);
    if (it == dispatch.end()) {
        inProgress.erase(targetFact);
        return false; // pass has no runner in this context
    }

    // Verify preconditions (should be satisfied by the fixed-point loop above).
    if (!checkPreconditions(*producer)) {
        inProgress.erase(targetFact);
        return false;
    }

    // Run the pass.
    it->second(program, ctx_);
    applyInvalidation(*producer);

    inProgress.erase(targetFact);
    return ctx_.validity().isValid(targetFact);
}

// ─────────────────────────────────────────────────────────────────────────────
// AnalysisDependencyGraph::createDefault
// ─────────────────────────────────────────────────────────────────────────────

/* static */
AnalysisDependencyGraph AnalysisDependencyGraph::createDefault() {
    AnalysisDependencyGraph g;
    namespace F = AnalysisFact;

    // purity depends on constant_returns:
    //   if constant_returns changes, purity may need to be re-run
    g.addDependency(F::kPurity, F::kConstantReturns);

    // effects depends on purity:
    //   if purity is invalidated, effect summaries (which track pure calls)
    //   must also be considered stale
    g.addDependency(F::kEffects, F::kPurity);

    // ersl depends on effects:
    //   ERSL derives stability/idempotence/speculation flags from FunctionEffects;
    //   if effects change, all ERSL-derived flags must be recomputed
    g.addDependency(F::kERSL, F::kEffects);

    // synthesis depends on purity and effects:
    //   synthesis uses purity/effect facts to decide which functions to expand
    g.addDependency(F::kSynthesis, F::kPurity);
    g.addDependency(F::kSynthesis, F::kEffects);

    // cfctre depends on purity, effects, and synthesis:
    //   CF-CTRE uses all three to evaluate cross-function constants
    g.addDependency(F::kCFCTRE, F::kPurity);
    g.addDependency(F::kCFCTRE, F::kEffects);
    g.addDependency(F::kCFCTRE, F::kSynthesis);

    // egraph depends on cfctre:
    //   e-graph uses compile-time evaluation results from CF-CTRE
    g.addDependency(F::kEGraph, F::kCFCTRE);

    // range_analysis depends on purity, effects, and cfctre:
    //   range inference uses function effects and CF-CTRE uniform-return facts
    g.addDependency(F::kRangeAnalysis, F::kPurity);
    g.addDependency(F::kRangeAnalysis, F::kEffects);
    g.addDependency(F::kRangeAnalysis, F::kCFCTRE);

    return g;
}

// ─────────────────────────────────────────────────────────────────────────────
// OptimizationManager
// ─────────────────────────────────────────────────────────────────────────────

void OptimizationManager::setCostModel(std::unique_ptr<CostModel> model) noexcept {
    costModel_ = std::move(model);
}

PassScheduler OptimizationManager::createScheduler(OptimizationContext& ctx) const {
    PassScheduler sched(ctx);
#ifndef NDEBUG
    sched.setStrictMode(true);
#endif
    return sched;
}

} // namespace omscript
