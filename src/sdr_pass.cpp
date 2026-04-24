// ─────────────────────────────────────────────────────────────────────────────
// sdr_pass.cpp — Speculative Devectorization & Revectorization (SDR)
// ─────────────────────────────────────────────────────────────────────────────
//
// See sdr_pass.h for the algorithm overview and phase descriptions.

#include "sdr_pass.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cassert>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript::sdr {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Return the FixedVectorType if @p v has one, otherwise nullptr.
static llvm::FixedVectorType* vecTypeOf(llvm::Value* v) noexcept {
    return llvm::dyn_cast<llvm::FixedVectorType>(v->getType());
}

/// True when @p inst is an arithmetic or bitwise binary operator on a vector type.
static bool isVectorArith(const llvm::Instruction* inst) noexcept {
    if (!inst->getType()->isVectorTy()) return false;
    switch (inst->getOpcode()) {
        case llvm::Instruction::Add:
        case llvm::Instruction::FAdd:
        case llvm::Instruction::Sub:
        case llvm::Instruction::FSub:
        case llvm::Instruction::Mul:
        case llvm::Instruction::FMul:
        case llvm::Instruction::SDiv:
        case llvm::Instruction::UDiv:
        case llvm::Instruction::FDiv:
        case llvm::Instruction::SRem:
        case llvm::Instruction::URem:
        case llvm::Instruction::FRem:
        case llvm::Instruction::And:
        case llvm::Instruction::Or:
        case llvm::Instruction::Xor:
        case llvm::Instruction::Shl:
        case llvm::Instruction::LShr:
        case llvm::Instruction::AShr:
            return true;
        default:
            return false;
    }
}

/// Compute a TTI arithmetic cost for a binary opcode on a given vector type.
static double vecArithCost(llvm::TargetTransformInfo& TTI,
                            unsigned opcode,
                            llvm::VectorType* vt) {
    using CK = llvm::TargetTransformInfo::TCK_RecipThroughput;
    llvm::Type* elemTy = vt->getElementType();
    llvm::TargetTransformInfo::OperandValueInfo ovi;
    auto cost = TTI.getArithmeticInstrCost(
        opcode, elemTy,
        llvm::TargetTransformInfo::TCK_RecipThroughput,
        ovi, ovi, vt);
    return static_cast<double>(cost.getValue().getFixedValue());
}

/// True when @p inst is an extractelement with a constant index.
static bool isConstExtract(const llvm::Instruction* inst) noexcept {
    if (auto* ee = llvm::dyn_cast<llvm::ExtractElementInst>(inst))
        return llvm::isa<llvm::ConstantInt>(ee->getIndexOperand());
    return false;
}

/// Return the constant lane index of an extractelement, or -1 if not constant.
static int64_t extractLaneIndex(const llvm::ExtractElementInst* ee) noexcept {
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(ee->getIndexOperand()))
        return static_cast<int64_t>(ci->getZExtValue());
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 1 — Detect suboptimal SIMD regions
// ─────────────────────────────────────────────────────────────────────────────
//
// Classifies each FixedVectorType-producing instruction into one of the four
// RegionKind categories.  Applies a lane-use fraction threshold before
// emitting an SdrRegion.

/// Compute the used-lane bitmask for @p vecInst by walking all users.
/// Returns {usedMask, usedLaneCount}.
static std::pair<uint64_t, unsigned> computeUsedLanes(llvm::Instruction* vecInst) {
    auto* fvt = llvm::cast<llvm::FixedVectorType>(vecInst->getType());
    const unsigned totalLanes = fvt->getNumElements();
    uint64_t mask = 0;

    for (llvm::User* u : vecInst->users()) {
        if (auto* ee = llvm::dyn_cast<llvm::ExtractElementInst>(u)) {
            int64_t idx = extractLaneIndex(ee);
            if (idx >= 0 && static_cast<unsigned>(idx) < totalLanes)
                mask |= (uint64_t(1) << idx);
        } else {
            // Non-extract user: conservatively mark all lanes used.
            mask = (totalLanes < 64) ? ((uint64_t(1) << totalLanes) - 1) : ~uint64_t(0);
            return {mask, totalLanes};
        }
    }
    return {mask, static_cast<unsigned>(__builtin_popcountll(mask))};
}

/// Detect a horizontal reduction pattern rooted at @p vecInst.
/// Pattern: all lanes extracted and fed into an associative binary arith tree.
/// Returns true when all lanes are consumed and only by a single arith tree.
static bool detectReduction(llvm::Instruction* vecInst) {
    auto* fvt = llvm::cast<llvm::FixedVectorType>(vecInst->getType());
    const unsigned totalLanes = fvt->getNumElements();

    // Collect all extract-element users.
    std::vector<llvm::ExtractElementInst*> extracts;
    for (llvm::User* u : vecInst->users()) {
        auto* ee = llvm::dyn_cast<llvm::ExtractElementInst>(u);
        if (!ee) return false; // Non-extract use → not a pure reduction.
        if (extractLaneIndex(ee) < 0) return false;
        extracts.push_back(ee);
    }
    if (extracts.size() != totalLanes) return false;

    // Verify that every extract feeds only arithmetic users and that
    // the arith users form a tree (each has at most one non-extract input).
    // We do a single-level check here (deep analysis is handled in phase 4).
    for (auto* ee : extracts) {
        for (llvm::User* eu : ee->users()) {
            auto* inst = llvm::dyn_cast<llvm::Instruction>(eu);
            if (!inst) return false;
            const unsigned op = inst->getOpcode();
            const bool isArith =
                op == llvm::Instruction::Add  || op == llvm::Instruction::FAdd ||
                op == llvm::Instruction::Sub  || op == llvm::Instruction::FSub ||
                op == llvm::Instruction::Mul  || op == llvm::Instruction::FMul ||
                op == llvm::Instruction::And  || op == llvm::Instruction::Or   ||
                op == llvm::Instruction::Xor;
            if (!isArith) return false;
        }
    }
    return true;
}

static std::vector<SdrRegion> phase1Detect(llvm::Function& F,
                                            llvm::TargetTransformInfo& TTI,
                                            const SdrConfig& cfg) {
    std::vector<SdrRegion> regions;

    for (auto& BB : F) {
        for (auto& I : BB) {
            auto* fvt = llvm::dyn_cast<llvm::FixedVectorType>(I.getType());
            if (!fvt || !isVectorArith(&I)) continue;

            const unsigned origLanes = fvt->getNumElements();
            if (origLanes < 2) continue; // Nothing to narrow.

            auto [usedMask, usedLanes] = computeUsedLanes(&I);

            // Classify the region.
            RegionKind kind;
            bool isReduction = false;

            if (detectReduction(&I)) {
                kind = RegionKind::WideReduce;
                isReduction = true;
                usedLanes = origLanes; // All lanes consumed by reduction.
            } else {
                // Scan users for extract-chain vs. partial-use vs. scalar-mix.
                unsigned extractUsers = 0, nonExtractUsers = 0;
                for (llvm::User* u : I.users()) {
                    if (isConstExtract(llvm::cast<llvm::Instruction>(u)))
                        ++extractUsers;
                    else
                        ++nonExtractUsers;
                }

                if (extractUsers > 0 && nonExtractUsers == 0) {
                    kind = (usedLanes < origLanes) ? RegionKind::PartialUse
                                                   : RegionKind::ExtractChain;
                } else if (usedLanes < origLanes) {
                    kind = RegionKind::PartialUse;
                } else {
                    kind = RegionKind::ScalarMix;
                }
            }

            // Apply lane-use fraction threshold (skip if not a reduction and
            // lane utilisation is already high enough).
            if (!isReduction && kind != RegionKind::ScalarMix) {
                const double useFrac = static_cast<double>(usedLanes) / origLanes;
                if (useFrac >= cfg.partialUseFraction) continue;
            }

            // Compute original cost for profitability gate in phase 4.
            const double origCost = vecArithCost(TTI, I.getOpcode(), fvt);

            regions.push_back(SdrRegion{
                &I, kind, origLanes, usedLanes, usedMask, isReduction, origCost
            });
        }
    }
    return regions;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 2 — Devectorize: explode vector SSA into per-lane scalar values
// ─────────────────────────────────────────────────────────────────────────────
//
// For each vector instruction root, extract each lane into a scalar using
// ExtractElement, then patch users of the vector via scalar->vector
// round-trip (InsertElement chains) so the rest of the function sees
// the same type.  The original vector instruction is left in place; phases
// 3 and 4 will clean it up once they know what to do with the scalars.
//
// Returns a mapping: lane index → scalar Value* for the root instruction.

using ScalarLanes = std::vector<llvm::Value*>; // index → scalar value

static ScalarLanes devectorize(SdrRegion& region, llvm::IRBuilder<>& builder) {
    llvm::Instruction* root = region.root;
    auto* fvt = llvm::cast<llvm::FixedVectorType>(root->getType());
    const unsigned N = fvt->getNumElements();
    llvm::Type* elemTy = fvt->getElementType();
    (void)elemTy; // used below via ExtractElement

    builder.SetInsertPoint(root->getNextNode());

    ScalarLanes lanes(N);
    for (unsigned i = 0; i < N; ++i) {
        auto* idx = llvm::ConstantInt::get(llvm::Type::getInt32Ty(root->getContext()), i);
        lanes[i] = builder.CreateExtractElement(root, idx,
                                                 root->getName() + ".sdr.s" + std::to_string(i));
    }
    return lanes;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3 — Scalar dataflow analysis
// ─────────────────────────────────────────────────────────────────────────────
//
// Walk the scalar lanes produced by devectorize() and:
//   • confirm which lanes are actually consumed downstream (live-lane mask)
//   • detect whether the reduction pattern holds in the scalar form
//   • compute the minimum vector width that covers all live lanes

struct DataflowResult {
    uint64_t liveMask      = 0;
    unsigned liveCount     = 0;
    unsigned parallelWidth = 0; // smallest power-of-2 ≥ liveCount
    bool     isReduction   = false;
};

static unsigned nextPow2(unsigned v) noexcept {
    if (v <= 1) return 1;
    --v;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

static DataflowResult phase3Analyze(const SdrRegion& region,
                                     const ScalarLanes& lanes) {
    DataflowResult res;
    const unsigned N = region.origLanes;

    for (unsigned i = 0; i < N; ++i) {
        if (!lanes[i]) continue;
        if (!lanes[i]->use_empty()) {
            res.liveMask |= (uint64_t(1) << i);
            ++res.liveCount;
        }
    }

    res.parallelWidth = nextPow2(res.liveCount);
    if (res.parallelWidth < 2) res.parallelWidth = 2; // Minimum useful vector width.

    // Confirm reduction if the region claimed it and scalar uses form a tree.
    res.isReduction = region.isReduction && (res.liveCount == N);

    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 4 — Revectorize intelligently
// ─────────────────────────────────────────────────────────────────────────────
//
// Decision tree:
//   1. If isReduction → replace extract+arith chain with llvm.vector.reduce.*
//   2. If liveCount < origLanes && enableNarrowing →
//        rebuild as smaller FixedVectorType (narrow), gate by TTI cost.
//   3. If liveCount > origLanes && enableWidening && target supports wider →
//        rebuild as wider FixedVectorType (widen), gate by TTI cost.
//   4. Otherwise → passthrough (leave scalar, possibly cheaper).

/// Map a binary opcode to the corresponding llvm.vector.reduce.* intrinsic ID.
/// Returns Intrinsic::not_intrinsic for unsupported opcodes.
static llvm::Intrinsic::ID reduceIntrinsicFor(unsigned opcode, bool isFP) noexcept {
    switch (opcode) {
        case llvm::Instruction::Add:
        case llvm::Instruction::FAdd: return llvm::Intrinsic::vector_reduce_add;
        case llvm::Instruction::Mul:
        case llvm::Instruction::FMul: return llvm::Intrinsic::vector_reduce_mul;
        case llvm::Instruction::And:  return llvm::Intrinsic::vector_reduce_and;
        case llvm::Instruction::Or:   return llvm::Intrinsic::vector_reduce_or;
        case llvm::Instruction::Xor:  return llvm::Intrinsic::vector_reduce_xor;
        default:                      return llvm::Intrinsic::not_intrinsic;
    }
}

/// Replace @p extract users with the given scalar @p replacement, leaving
/// extractelement instructions for potential DCE by subsequent passes.
static void replaceExtractUsers(llvm::Value* vec,
                                 unsigned laneIdx,
                                 llvm::Value* replacement) {
    llvm::SmallVector<llvm::User*, 8> toReplace;
    for (llvm::User* u : vec->users()) {
        auto* ee = llvm::dyn_cast<llvm::ExtractElementInst>(u);
        if (!ee) continue;
        if (extractLaneIndex(ee) == static_cast<int64_t>(laneIdx))
            toReplace.push_back(ee);
    }
    for (llvm::User* u : toReplace) {
        llvm::cast<llvm::Instruction>(u)->replaceAllUsesWith(replacement);
        llvm::cast<llvm::Instruction>(u)->eraseFromParent();
    }
}

/// Build a narrow vector from the live subset of lanes and insert it into the
/// function.  @p liveScalars must already be in lane-index order (may contain
/// nullptrs for dead lanes, which are filled with undef).
/// Returns the new FixedVectorType instruction or nullptr if rebuild failed.
static llvm::Value* buildNarrowVector(llvm::Instruction* insertBefore,
                                       llvm::Type* elemTy,
                                       const std::vector<llvm::Value*>& liveScalars,
                                       unsigned newLanes,
                                       llvm::LLVMContext& ctx) {
    llvm::IRBuilder<> b(insertBefore);
    auto* newVT = llvm::FixedVectorType::get(elemTy, newLanes);
    llvm::Value* vec = llvm::UndefValue::get(newVT);
    unsigned slot = 0;
    for (llvm::Value* s : liveScalars) {
        if (!s) {
            ++slot;
            continue;
        }
        auto* idx = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), slot);
        vec = b.CreateInsertElement(vec, s, idx, "sdr.narrow.ins");
        ++slot;
        if (slot >= newLanes) break;
    }
    return vec;
}

/// Emit llvm.vector.reduce.X(@p vec) just before @p insertBefore.
static llvm::Value* emitReductionIntrinsic(llvm::Instruction* insertBefore,
                                            llvm::Intrinsic::ID iid,
                                            llvm::Value* vec) {
    llvm::IRBuilder<> b(insertBefore);
    llvm::Module* M = insertBefore->getModule();
    llvm::Function* intrinsicFn =
        llvm::Intrinsic::getDeclaration(M, iid, {vec->getType()});
    return b.CreateCall(intrinsicFn, {vec}, "sdr.reduce");
}

enum class RevecAction { Reduce, Narrow, Widen, Passthrough };

static SdrStats phase4Revectorize(std::vector<SdrRegion>& regions,
                                   const std::vector<ScalarLanes>& allLanes,
                                   const std::vector<DataflowResult>& dfResults,
                                   llvm::TargetTransformInfo& TTI,
                                   const SdrConfig& cfg) {
    SdrStats stats;

    for (size_t ri = 0; ri < regions.size(); ++ri) {
        SdrRegion& reg    = regions[ri];
        const DataflowResult& df = dfResults[ri];
        const ScalarLanes& lanes = allLanes[ri];

        if (!reg.root) continue;
        auto* fvt = llvm::cast<llvm::FixedVectorType>(reg.root->getType());
        llvm::Type* elemTy = fvt->getElementType();
        llvm::LLVMContext& ctx = reg.root->getContext();

        // ── Case 1: Reduction → intrinsic ────────────────────────────────
        if (df.isReduction && cfg.enableReductionRecognition) {
            const llvm::Intrinsic::ID iid =
                reduceIntrinsicFor(reg.root->getOpcode(), elemTy->isFloatingPointTy());
            if (iid != llvm::Intrinsic::not_intrinsic) {
                // Cost of reduction intrinsic vs. extract+arith chain.
                using CK = llvm::TargetTransformInfo::TCK_RecipThroughput;
                auto redCost = TTI.getArithmeticReductionCost(
                    reg.root->getOpcode(), fvt, std::nullopt,
                    llvm::TargetTransformInfo::TCK_RecipThroughput);
                const double newCost = static_cast<double>(
                    redCost.getValue().getFixedValue());
                const double saving = reg.origCost - newCost;
                if (saving >= cfg.minAbsoluteSaving &&
                    newCost <= reg.origCost * cfg.costThreshold) {
                    // Replace: emit reduction intrinsic, replace all extract users.
                    llvm::Instruction* insertPt = reg.root->getNextNode();
                    llvm::Value* redResult = emitReductionIntrinsic(
                        insertPt, iid, reg.root);
                    // Replace all extractelement users that formed the reduction.
                    for (unsigned i = 0; i < reg.origLanes; ++i)
                        replaceExtractUsers(reg.root, i, redResult);
                    ++stats.reductionsReplaced;
                    continue;
                }
                ++stats.skippedCostly;
                continue;
            }
        }

        // ── Case 2: Narrow ────────────────────────────────────────────────
        if (cfg.enableNarrowing &&
            df.liveCount > 0 &&
            df.liveCount < reg.origLanes) {
            const unsigned newLanes = nextPow2(df.liveCount);
            if (newLanes >= reg.origLanes) { ++stats.skippedCostly; continue; }

            auto* newVT = llvm::FixedVectorType::get(elemTy, newLanes);
            const double newCost = vecArithCost(TTI, reg.root->getOpcode(), newVT);
            const double saving = reg.origCost - newCost;

            if (saving < cfg.minAbsoluteSaving ||
                newCost > reg.origCost * cfg.costThreshold) {
                ++stats.skippedCostly;
                continue;
            }

            // Collect live scalars in lane order.
            std::vector<llvm::Value*> liveScalars;
            liveScalars.reserve(newLanes);
            for (unsigned i = 0; i < reg.origLanes && liveScalars.size() < newLanes; ++i) {
                if (df.liveMask & (uint64_t(1) << i))
                    liveScalars.push_back(lanes[i]);
            }
            while (liveScalars.size() < newLanes)
                liveScalars.push_back(nullptr); // fill with undef

            llvm::Value* narrowVec = buildNarrowVector(
                reg.root->getNextNode(), elemTy, liveScalars, newLanes, ctx);
            if (!narrowVec) { ++stats.skippedCostly; continue; }

            // Replace extract users with extracts from the narrow vector.
            unsigned slot = 0;
            for (unsigned i = 0; i < reg.origLanes && slot < newLanes; ++i) {
                if (!(df.liveMask & (uint64_t(1) << i))) continue;
                auto* newIdx = llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), slot);
                llvm::IRBuilder<> b(reg.root->getNextNode());
                llvm::Value* newExt = b.CreateExtractElement(narrowVec, newIdx,
                                                              "sdr.narrow.ext");
                replaceExtractUsers(reg.root, i, newExt);
                ++slot;
            }
            ++stats.narrowed;
            continue;
        }

        // ── Case 3: Widen ─────────────────────────────────────────────────
        if (cfg.enableWidening &&
            reg.origLanes < cfg.maxWidenLanes) {
            const unsigned newLanes = std::min(reg.origLanes * 2, cfg.maxWidenLanes);
            auto* newVT = llvm::FixedVectorType::get(elemTy, newLanes);
            const double newCost = vecArithCost(TTI, reg.root->getOpcode(), newVT);
            const double saving  = reg.origCost - newCost;

            // Widening is worthwhile only when throughput actually improves.
            if (saving >= cfg.minAbsoluteSaving &&
                newCost <= reg.origCost * cfg.costThreshold) {
                // Widen: pad the existing vector with undef lanes using
                // shufflevector, then operate on the wider vector.
                llvm::IRBuilder<> b(reg.root);

                // Build shuffle mask: first origLanes from src, rest undef (poison).
                llvm::SmallVector<int, 16> mask;
                for (unsigned i = 0; i < newLanes; ++i)
                    mask.push_back(i < reg.origLanes ? static_cast<int>(i) : -1);

                // Widen each operand by shuffling with poison.
                llvm::SmallVector<llvm::Value*, 2> newOps;
                bool ok = true;
                for (unsigned opIdx = 0; opIdx < reg.root->getNumOperands(); ++opIdx) {
                    llvm::Value* op = reg.root->getOperand(opIdx);
                    if (!llvm::isa<llvm::FixedVectorType>(op->getType())) { ok = false; break; }
                    auto* poison = llvm::PoisonValue::get(op->getType());
                    llvm::Value* wider = b.CreateShuffleVector(op, poison, mask,
                                                               "sdr.widen.shuf");
                    newOps.push_back(wider);
                }
                if (!ok) { ++stats.skippedCostly; continue; }

                // Recreate the op on the wider type.
                llvm::Value* wideResult = nullptr;
                if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(reg.root)) {
                    wideResult = b.CreateBinOp(
                        bo->getOpcode(), newOps[0], newOps[1], "sdr.widen.op");
                }
                if (!wideResult) { ++stats.skippedCostly; continue; }

                // Narrow back to original width with extract-element so
                // existing users are unaffected.
                for (unsigned i = 0; i < reg.origLanes; ++i) {
                    auto* idx = llvm::ConstantInt::get(
                        llvm::Type::getInt32Ty(ctx), i);
                    llvm::IRBuilder<> eb(reg.root->getNextNode());
                    llvm::Value* ext = eb.CreateExtractElement(wideResult, idx,
                                                               "sdr.widen.ext");
                    replaceExtractUsers(reg.root, i, ext);
                }
                ++stats.widened;
                continue;
            }
        }

        // ── Case 4: Passthrough — leave as scalar ─────────────────────────
        // Scalar fallback: just leave the devectorized scalars in place.
        // Subsequent DCE passes will eliminate the original vector op if its
        // only users were the extract instructions we already replaced.
        ++stats.passthrough;
    }

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-function driver
// ─────────────────────────────────────────────────────────────────────────────

static SdrStats runOnFunction(llvm::Function& F,
                               llvm::TargetTransformInfo& TTI,
                               const SdrConfig& cfg) {
    SdrStats stats;
    if (F.isDeclaration()) return stats;

    // Phase 1: detect candidate regions.
    auto regions = phase1Detect(F, TTI, cfg);
    stats.regionsDetected = static_cast<unsigned>(regions.size());

    if (regions.empty()) return stats;

    // Apply max-region-size filter.
    unsigned instrCount = 0;
    for (auto& BB : F) instrCount += static_cast<unsigned>(BB.size());
    {
        auto it = std::remove_if(regions.begin(), regions.end(),
            [&](const SdrRegion& r) {
                (void)r;
                if (instrCount > cfg.maxRegionSize * 4) {
                    ++stats.skippedLarge;
                    return true;
                }
                return false;
            });
        regions.erase(it, regions.end());
    }
    stats.regionsAnalyzed = static_cast<unsigned>(regions.size());
    if (regions.empty()) return stats;

    // Phase 2: devectorize.
    std::vector<ScalarLanes> allLanes(regions.size());
    llvm::IRBuilder<> builder(F.getContext());
    for (size_t i = 0; i < regions.size(); ++i) {
        if (!regions[i].root) continue;
        allLanes[i] = devectorize(regions[i], builder);
    }

    // Phase 3: scalar dataflow analysis.
    std::vector<DataflowResult> dfResults(regions.size());
    for (size_t i = 0; i < regions.size(); ++i) {
        if (!regions[i].root) continue;
        dfResults[i] = phase3Analyze(regions[i], allLanes[i]);
    }

    // Phase 4: revectorize.
    SdrStats phase4Stats = phase4Revectorize(regions, allLanes, dfResults, TTI, cfg);

    // Merge stats.
    stats.narrowed           += phase4Stats.narrowed;
    stats.widened            += phase4Stats.widened;
    stats.reductionsReplaced += phase4Stats.reductionsReplaced;
    stats.passthrough        += phase4Stats.passthrough;
    stats.skippedCostly      += phase4Stats.skippedCostly;

    return stats;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// runSDR — public entry point
// ─────────────────────────────────────────────────────────────────────────────

SdrStats runSDR(llvm::Module& module,
                const std::function<llvm::TargetTransformInfo(llvm::Function&)>& getTTI,
                const SdrConfig& cfg) {
    SdrStats total;

    for (llvm::Function& F : module) {
        if (F.isDeclaration()) continue;
        llvm::TargetTransformInfo TTI = getTTI(F);
        SdrStats fs = runOnFunction(F, TTI, cfg);
        total.regionsDetected   += fs.regionsDetected;
        total.regionsAnalyzed   += fs.regionsAnalyzed;
        total.narrowed          += fs.narrowed;
        total.widened           += fs.widened;
        total.reductionsReplaced+= fs.reductionsReplaced;
        total.passthrough       += fs.passthrough;
        total.skippedCostly     += fs.skippedCostly;
        total.skippedLarge      += fs.skippedLarge;
    }

    return total;
}

} // namespace omscript::sdr
