/// @file loop_transform_framework.cpp
/// @brief Implementation of UnifiedLoopTransformer.

#include "loop_transform_framework.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

LoopLegalityContext
UnifiedLoopTransformer::buildLegalityContext(
    const LoopTransformRequest& req) const
{
    LoopLegalityContext ctx;
    ctx.function   = req.function;
    ctx.outerLoop  = req.outerLoop;
    ctx.effects    = req.effects;
    ctx.skewFactor = req.skewFactor;

    // Count loop depth and find innermost loop.
    if (req.outerLoop) {
        unsigned depth = 1;
        llvm::Loop* inner = req.outerLoop;
        while (!inner->getSubLoops().empty()) {
            inner = inner->getSubLoops().front();
            ++depth;
        }
        ctx.loopDepth = depth;
        ctx.innerLoop = inner;
    }

    return ctx;
}

bool UnifiedLoopTransformer::checkProfitability(
    LoopTransform transform,
    llvm::Loop*   loop) const noexcept
{
    if (!costModel_ || !loop) return true; // conservative: assume profitable

    // Estimate the cost of all instructions in the loop body.
    double totalCost = 0.0;
    for (llvm::BasicBlock* BB : loop->blocks()) {
        for (llvm::Instruction& I : *BB) {
            totalCost += costModel_->instructionCost(&I);
        }
    }

    // Profitability thresholds per transform type.
    // These are heuristic cycle budgets below which the overhead introduced by
    // the transform (extra loop variables, address computations, etc.) is not
    // justified.
    switch (transform) {
    case LoopTransform::Tiling:
        // Tiling adds two extra loops and complex subscript recomputation.
        // Require at least ~8 cycles of work per iteration.
        return totalCost >= 8.0;

    case LoopTransform::Interchange:
        // Interchange only changes the iteration order; overhead is small.
        // Require at least ~2 cycles.
        return totalCost >= 2.0;

    case LoopTransform::Skewing:
        // Skewing adds a multiply to the inner IV.  Require ~4 cycles.
        return totalCost >= 4.0;

    case LoopTransform::Reversal:
        // Reversal is nearly free (just flips the step sign).
        return totalCost >= 1.0;

    case LoopTransform::Fusion:
    case LoopTransform::Fission:
        // Fusion/fission trade off loop overhead vs. register reuse.
        // Require at least ~4 cycles.
        return totalCost >= 4.0;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// UnifiedLoopTransformer::execute
// ─────────────────────────────────────────────────────────────────────────────

LoopTransformResult
UnifiedLoopTransformer::execute(const LoopTransformRequest& req,
                                llvm::ScalarEvolution& SE,
                                llvm::DominatorTree&   DT,
                                llvm::LoopInfo&        LI) const
{
    LoopTransformResult result;

    if (!req.outerLoop || !req.function) {
        result.status = LoopTransformStatus::SkippedIllegal;
        return result;
    }

    // ── Step 1: High-level LegalityService check ──────────────────────────
    // Cheap effect-based safety check before we build any polyhedral model.
    if (legality_) {
        // Prefer AST-level FunctionEffects when available; fall back to IR
        // attributes otherwise.
        LegalityVerdict verdict;
        if (req.effects) {
            auto lctx         = buildLegalityContext(req);
            verdict           = legality_->checkLegality(req.transform, lctx);
        } else {
            verdict = legality_->canTransformFunction(*req.function);
        }

        result.legalityVerdict = verdict;

        if (verdict == LegalityVerdict::Illegal) {
            result.status = LoopTransformStatus::SkippedIllegal;
            return result;
        }
        // Unknown → proceed to polyhedral analysis (conservative but correct).
    }

    // ── Step 2: Polyhedral legality (dependence-direction-vector check) ───
    polyopt::PolyOptConfig pcfg =
        req.polyConfig.value_or(polyopt::PolyOptConfig{});
    pcfg.legality  = legality_;
    pcfg.costModel = costModel_;

    const auto polyLegality =
        polyopt::checkLoopLegality(req.outerLoop, SE, DT, LI, pcfg);
    result.polyLegality = polyLegality;

    bool transformAllowed = false;
    switch (req.transform) {
    case LoopTransform::Interchange: transformAllowed = polyLegality.interchange; break;
    case LoopTransform::Tiling:      transformAllowed = polyLegality.tiling;      break;
    case LoopTransform::Reversal:    transformAllowed = polyLegality.reversal;    break;
    case LoopTransform::Skewing:     transformAllowed = polyLegality.skewing;     break;
    case LoopTransform::Fusion:
    case LoopTransform::Fission:
        // Fusion and fission legality depends on adjacent loop compatibility;
        // polyopt checks both in its full SCoP pass.  Treat checkLoopLegality
        // returning skewing-legal as a proxy that the SCoP was detected.
        transformAllowed = polyLegality.tiling; // SCoP is valid
        break;
    }

    if (!transformAllowed) {
        // No SCoP detected (all-false polyLegality) or this transform is illegal.
        const bool noScop = !polyLegality.interchange && !polyLegality.tiling &&
                            !polyLegality.reversal && !polyLegality.skewing;
        result.status = noScop ? LoopTransformStatus::SkippedNoSCoP
                               : LoopTransformStatus::SkippedIllegal;
        return result;
    }

    // ── Step 3: CostModel profitability check ─────────────────────────────
    if (!checkProfitability(req.transform, req.outerLoop)) {
        result.status        = LoopTransformStatus::SkippedUnprof;
        result.wasProfitable = false;
        return result;
    }
    result.wasProfitable = true;

    // ── Step 4: Dispatch to backend implementation ─────────────────────────
    // Build a PolyOptConfig restricted to only the requested transform, then
    // run polyopt on the single function.  polyopt will detect the same SCoP
    // and apply only the transform we asked for (other transforms disabled).
    switch (req.transform) {
    case LoopTransform::Interchange:
        pcfg.enableTiling     = false;
        pcfg.enableInterchange= true;
        pcfg.enableSkewing    = false;
        pcfg.enableFusion     = false;
        pcfg.enableFission    = false;
        pcfg.enableReversal   = false;
        break;
    case LoopTransform::Tiling:
        pcfg.enableTiling     = true;
        pcfg.enableInterchange= false;
        pcfg.enableSkewing    = false;
        pcfg.enableFusion     = false;
        pcfg.enableFission    = false;
        pcfg.enableReversal   = false;
        break;
    case LoopTransform::Reversal:
        pcfg.enableTiling     = false;
        pcfg.enableInterchange= false;
        pcfg.enableSkewing    = false;
        pcfg.enableFusion     = false;
        pcfg.enableFission    = false;
        pcfg.enableReversal   = true;
        break;
    case LoopTransform::Skewing:
        pcfg.enableTiling     = false;
        pcfg.enableInterchange= false;
        pcfg.enableSkewing    = true;
        pcfg.enableFusion     = false;
        pcfg.enableFission    = false;
        pcfg.enableReversal   = false;
        break;
    case LoopTransform::Fusion:
        pcfg.enableTiling     = false;
        pcfg.enableInterchange= false;
        pcfg.enableSkewing    = false;
        pcfg.enableFusion     = true;
        pcfg.enableFission    = false;
        pcfg.enableReversal   = false;
        break;
    case LoopTransform::Fission:
        pcfg.enableTiling     = false;
        pcfg.enableInterchange= false;
        pcfg.enableSkewing    = false;
        pcfg.enableFusion     = false;
        pcfg.enableFission    = true;
        pcfg.enableReversal   = false;
        break;
    }

    result.polyStats = polyopt::optimizeFunction(*req.function, pcfg);

    const unsigned total = result.polyStats.loopsTiled    +
                           result.polyStats.loopsInterchanged +
                           result.polyStats.loopsSkewed   +
                           result.polyStats.loopsFused    +
                           result.polyStats.loopsFissioned+
                           result.polyStats.loopsReversed;

    result.status = (total > 0) ? LoopTransformStatus::Applied
                                : LoopTransformStatus::Failed;
    return result;
}

} // namespace omscript
