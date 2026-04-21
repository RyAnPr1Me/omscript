/// @file optimization_manager.cpp
/// @brief Implementation of OptimizationManager, PassScheduler, LegalityService.

#include "optimization_manager.h"
#include "superoptimizer.h" // superopt::instructionCost — used by DefaultCostModel
#include <cassert>
#include <iostream>

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

// ─────────────────────────────────────────────────────────────────────────────
// PassScheduler
// ─────────────────────────────────────────────────────────────────────────────

bool PassScheduler::checkPreconditions(const PassMetadata& meta) const noexcept {
    for (const char* req : meta.requires_) {
        if (!ctx_.validity().isValid(req)) {
            const bool shouldAssert = strict_;
            std::cerr << "[omscript][opt] "
                      << (shouldAssert ? "FATAL: " : "WARNING: ")
                      << "pass '" << meta.name
                      << "' requires fact '" << req
                      << "' which is not yet valid — "
                      << (shouldAssert ? "pass ordering is incorrect"
                                       : "skipping pass")
                      << "\n";
            // In debug mode with strict checking, a precondition failure is a
            // programmer error that must be surfaced immediately.
            assert(!shouldAssert && "Pass precondition violated — see error above");
            return false;
        }
    }
    return true;
}

void PassScheduler::applyInvalidation(const PassMetadata& meta) noexcept {
    for (const char* fact : meta.invalidates_) {
        // Invalidate the flag-based validity record so downstream passes know
        // they need to re-run before consuming this fact.
        ctx_.validity().invalidate(fact);
        // Also evict any typed cached result to prevent stale data from being
        // returned by ctx.cache().get<T>(fact) after the transformation.
        ctx_.cache().invalidate(fact);
    }
}

bool PassScheduler::allProvidedValid(const PassMetadata& meta) const noexcept {
    for (const char* fact : meta.provides_) {
        if (!ctx_.validity().isValid(fact)) return false;
    }
    return true;
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
