#include "codegen.h"
#include "diagnostic.h"
#include "hardware_graph.h"
#include "ipof_pass.h"
#include "optimization_manager.h" // OptimizationManager, CostModel
#include "sdr_pass.h"
#include "superoptimizer.h"
#include "polyopt.h"
#include <iostream>
#include <unordered_set>
#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#ifdef POLLY_LIB_PATH
#if LLVM_VERSION_MAJOR >= 22
#include <llvm/Plugins/PassPlugin.h>
#else
#include <llvm/Passes/PassPlugin.h>
#endif
#endif
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/PGOOptions.h>
#include <llvm/Support/TargetSelect.h>
#if LLVM_VERSION_MAJOR < 22
#include <llvm/Support/VirtualFileSystem.h>
#endif
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/GlobalOpt.h>
#include <llvm/Transforms/IPO/HotColdSplitting.h>
#include <llvm/Transforms/IPO/ArgumentPromotion.h>
#include <llvm/Transforms/IPO/ConstantMerge.h>
#include <llvm/Transforms/IPO/DeadArgumentElimination.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <llvm/Transforms/IPO/InferFunctionAttrs.h>
#include <llvm/Transforms/IPO/PartialInlining.h>
#include <llvm/Transforms/IPO/GlobalSplit.h>
#include <llvm/Transforms/IPO/StripDeadPrototypes.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/CorrelatedValuePropagation.h>
#include <llvm/Transforms/Scalar/ConstraintElimination.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/FlattenCFG.h>
#include <llvm/Transforms/Scalar/Float2Int.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/IndVarSimplify.h>
#include <llvm/Transforms/Scalar/LoopDeletion.h>
#include <llvm/Transforms/Scalar/DFAJumpThreading.h>
#include <llvm/Transforms/Scalar/InductiveRangeCheckElimination.h>
#include <llvm/Transforms/Scalar/InferAlignment.h>
#include <llvm/Transforms/Scalar/AlignmentFromAssumptions.h>
#include <llvm/Transforms/Scalar/LoopBoundSplit.h>
#include <llvm/Transforms/Scalar/LoopDistribute.h>
#include <llvm/Transforms/Scalar/LoopFlatten.h>
#include <llvm/Transforms/Scalar/LoopFuse.h>
#include <llvm/Transforms/Scalar/LoopIdiomRecognize.h>
#include <llvm/Transforms/Scalar/LoopInstSimplify.h>
#include <llvm/Transforms/Scalar/LoopInterchange.h>
#include <llvm/Transforms/Scalar/LoopLoadElimination.h>
#include <llvm/Transforms/Scalar/LoopDataPrefetch.h>
#include <llvm/Transforms/Scalar/LoopRotation.h>
#include <llvm/Transforms/Scalar/LoopSink.h>
#include <llvm/Transforms/Scalar/LoopUnrollAndJamPass.h>
#include <llvm/Transforms/Scalar/LoopVersioningLICM.h>
#include <llvm/Transforms/Scalar/LoopPredication.h>
#include <llvm/Transforms/Scalar/LoopSimplifyCFG.h>
#include <llvm/Transforms/Scalar/CallSiteSplitting.h>
#include <llvm/Transforms/Scalar/Sink.h>
#include <llvm/Transforms/Scalar/ConstantHoisting.h>
#include <llvm/Transforms/Scalar/SeparateConstOffsetFromGEP.h>
#include <llvm/Transforms/Scalar/PartiallyInlineLibCalls.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Scalar/MergeICmps.h>
#include <llvm/Transforms/Scalar/MergedLoadStoreMotion.h>
#include <llvm/Transforms/Scalar/MemCpyOptimizer.h>
#include <llvm/Transforms/Scalar/NaryReassociate.h>
#include <llvm/Transforms/Scalar/BDCE.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/StraightLineStrengthReduce.h>
#include <llvm/Transforms/Scalar/TailRecursionElimination.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Scalar/SimpleLoopUnswitch.h>
#include <llvm/Transforms/Scalar/SpeculativeExecution.h>
#include <llvm/Transforms/Scalar/DivRemPairs.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/Utils/CanonicalizeFreezeInLoops.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/LibCallsShrinkWrap.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>
#include <llvm/Transforms/Scalar/LICM.h>
#include <llvm/Transforms/Vectorize/VectorCombine.h>
#include <llvm/Transforms/Vectorize/LoadStoreVectorizer.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>
#include <llvm/Transforms/Vectorize/SLPVectorizer.h>
#include <llvm/Transforms/Scalar/NewGVN.h>
#include <llvm/Transforms/Scalar/Scalarizer.h>
#include <llvm/Transforms/IPO/CalledValuePropagation.h>
#include <llvm/Transforms/IPO/Attributor.h>
#include <llvm/Transforms/IPO/SCCP.h>
#if LLVM_VERSION_MAJOR < 20
#include <llvm/Transforms/IPO/SyntheticCountsPropagation.h>
#endif
#include <llvm/Transforms/IPO/ElimAvailExtern.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#if LLVM_VERSION_MAJOR < 20
#include <llvm/Transforms/Scalar/LoopReroll.h>
#endif
#include <llvm/Transforms/Scalar/GuardWidening.h>
#include <llvm/Transforms/IPO/MergeFunctions.h>
#include <llvm/Transforms/Scalar/LowerExpectIntrinsic.h>
#include <llvm/Transforms/Scalar/WarnMissedTransforms.h>
#include <llvm/Transforms/Scalar/LowerConstantIntrinsics.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/BranchProbabilityInfo.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Transforms/Utils/ScalarEvolutionExpander.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <algorithm>
#include <optional>
#include <stdexcept>

// LLVM 19 changed the signatures of registerOptimizerEarlyEPCallback and
// registerOptimizerLastEPCallback to include a ThinOrFullLTOPhase argument.
#if LLVM_VERSION_MAJOR >= 19
#define OM_MODULE_EP_EXTRA_PARAM , llvm::ThinOrFullLTOPhase
#else
#define OM_MODULE_EP_EXTRA_PARAM
#endif

namespace omscript {

namespace {

/// Hardware-profile–driven cost model wrapping hgoe::instrCostFromProfile.
struct HGOECostModel final : public omscript::CostModel {
    hgoe::MicroarchProfile profile;
    explicit HGOECostModel(hgoe::MicroarchProfile p) : profile(std::move(p)) {}
    double instructionCost(const llvm::Instruction* i) const override {
        return hgoe::instrCostFromProfile(i, profile);
    }
};

struct UnifiedExecutionModel {
    unsigned opcacheLoopUopBudget = 256;
    unsigned streamingMinElements = 4096;
    unsigned writeCombiningStoreBudget = 8;
    unsigned maprMaxHoistDistance = 24;
    unsigned treeMaxLeaves = 8;
    double branchMispredictPenaltyUops = 16.0;
    unsigned branchSpeculationUopBudget = 6;
};

static UnifiedExecutionModel buildExecutionModel(const llvm::Function& F,
                                                 const llvm::TargetTransformInfo& TTI) {
    UnifiedExecutionModel model;
    const bool hasProfile = F.getParent() && F.getParent()->getProfileSummary(/*IsCS=*/false);

    // Cache-line size: 0 means the target doesn't know; default to 64 bytes.
    const unsigned cacheLineBytes =
        TTI.getCacheLineSize() > 0 ? static_cast<unsigned>(TTI.getCacheLineSize()) : 64u;

    // ── Streaming-store threshold ─────────────────────────────────────────
    {
        unsigned l1Bytes = 32768u; // 32 KB default
        if (auto l1 = TTI.getCacheSize(llvm::TargetTransformInfo::CacheLevel::L1D);
                l1 && *l1 > 0)
            l1Bytes = static_cast<unsigned>(*l1);
        // Minimum: two cache lines worth of elements to avoid spurious promotion
        // of loops with only a handful of iterations (e.g. loop count = 3).
        const unsigned minElems = (2u * cacheLineBytes) / 8u;
        model.streamingMinElements = std::max(l1Bytes / 8u, minElems);
    }

    // ── Write-combining buffer budget ─────────────────────────────────────
    model.writeCombiningStoreBudget = (cacheLineBytes >= 64u) ? 10u : 8u;

    // ── Register-file–derived budgets ─────────────────────────────────────
    const unsigned scalarRegs = TTI.getNumberOfRegisters(/*Vector=*/false);
    if (scalarRegs > 0) {
        // Tree rebalancing max width: reserve 6 registers for values live outside
        const unsigned pressureBudget = scalarRegs > 6u ? scalarRegs - 6u : scalarRegs;
        model.treeMaxLeaves = std::clamp<unsigned>(pressureBudget / 2u, 4u, 12u);

        // MAPR hoist distance: scaled from register count (more regs → wider OoO window).
        // x86-64 (16 GPRs): 16 + 8 = 24.  AArch64 / RISC-V (32 GPRs): 32 + 16 = 48.
        model.maprMaxHoistDistance =
            std::clamp<unsigned>(scalarRegs + (scalarRegs >> 1u), 16u, 48u);
    }

    // Without profile data, branch probabilities are synthetic 50/50 guesses; be conservative.
    if (!hasProfile) {
        model.branchSpeculationUopBudget = 3;
    }

    return model;
}

static unsigned instructionUopCost(const llvm::Instruction& I,
                                   const llvm::TargetTransformInfo& TTI) {
    if (I.isTerminator() || llvm::isa<llvm::DbgInfoIntrinsic>(I)) return 0;
    const llvm::InstructionCost c =
        TTI.getInstructionCost(&I, llvm::TargetTransformInfo::TCK_RecipThroughput);
    if (!c.isValid()) return 1u;
    const int64_t v = c.getValue().value_or(1);
    return v <= 0 ? 1u : static_cast<unsigned>(v);
}

static const llvm::Value* getBasePointer(const llvm::Value* V) {
    const llvm::Value* cur = V;
    while (true) {
        cur = cur->stripPointerCasts();
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(cur)) {
            cur = gep->getPointerOperand();
            continue;
        }
        return cur;
    }
}

static bool hasNoAliasProvenance(const llvm::LoadInst* LI, const llvm::StoreInst* SI) {
    const llvm::Value* loadBase = getBasePointer(LI->getPointerOperand());
    const llvm::Value* storeBase = getBasePointer(SI->getPointerOperand());
    if (loadBase == storeBase) return false;

    if (llvm::isa<llvm::AllocaInst>(loadBase) && llvm::isa<llvm::AllocaInst>(storeBase))
        return true;
    if (llvm::isa<llvm::GlobalValue>(loadBase) && llvm::isa<llvm::GlobalValue>(storeBase))
        return true;

    const auto* loadArg = llvm::dyn_cast<llvm::Argument>(loadBase);
    const auto* storeArg = llvm::dyn_cast<llvm::Argument>(storeBase);
    if (loadArg && storeArg)
        return loadArg != storeArg && loadArg->hasNoAliasAttr() && storeArg->hasNoAliasAttr();
    if (loadArg && loadArg->hasNoAliasAttr() &&
        (llvm::isa<llvm::AllocaInst>(storeBase) || llvm::isa<llvm::GlobalValue>(storeBase)))
        return true;
    if (storeArg && storeArg->hasNoAliasAttr() &&
        (llvm::isa<llvm::AllocaInst>(loadBase) || llvm::isa<llvm::GlobalValue>(loadBase)))
        return true;
    return false;
}

// Converging signed→unsigned division/remainder conversion.
static unsigned runSignedToUnsignedConverge(llvm::Module& M, int maxIter) {
    unsigned total = 0;
    for (int i = 0; i < maxIter; ++i) {
        unsigned round = 0;
        for (auto& F : M) {
            round += superopt::inferNonNegativeFlags(F);
            round += superopt::convertSRemToURem(F);
            round += superopt::convertSDivToUDiv(F);
        }
        total += round;
        if (round == 0) break; // fixed point
    }
    return total;
}

} // namespace

// Aggressive SimplifyCFG options used throughout the optimization pipeline.
static constexpr int kCFGSpeculationBonus = 10;
static llvm::SimplifyCFGOptions aggressiveCFGOpts() {
    return llvm::SimplifyCFGOptions()
        .convertSwitchToLookupTable(true)
        .convertSwitchRangeToICmp(true)
        .hoistCommonInsts(true)
        .sinkCommonInsts(true)
        .speculateBlocks(true)
        .forwardSwitchCondToPhi(true)
        .bonusInstThreshold(kCFGSpeculationBonus);
}

// Hyperblock-aggressive SimplifyCFG options for superblock/hyperblock formation.
static constexpr int kHyperblockSpeculationBonus = 24;
static llvm::SimplifyCFGOptions hyperblockCFGOpts() {
    return llvm::SimplifyCFGOptions()
        .convertSwitchToLookupTable(true)
        .convertSwitchRangeToICmp(true)
        .hoistCommonInsts(true)
        .sinkCommonInsts(true)
        .speculateBlocks(true)
        .forwardSwitchCondToPhi(true)
        .needCanonicalLoops(false)
        .bonusInstThreshold(kHyperblockSpeculationBonus);
}

// ── Rule 4: OpCache-Friendly Loop Partitioning Pass ──────────────────────────
struct OpcacheLoopPartitionPass
    : public llvm::PassInfoMixin<OpcacheLoopPartitionPass> {

    // Attach loop metadata to the back-edge branch of `L`.
    static void annotateLoop(llvm::Loop* L, llvm::LLVMContext& Ctx) {
        llvm::BasicBlock* latch = L->getLoopLatch();
        if (!latch) return;
        llvm::BranchInst* back = llvm::dyn_cast<llvm::BranchInst>(latch->getTerminator());
        if (!back) return;

        // Collect existing loop metadata operands (skip the self-ref at [0]).
        llvm::SmallVector<llvm::Metadata*, 8> mds;
        mds.push_back(nullptr); // placeholder for self-reference
        if (llvm::MDNode* existing = back->getMetadata(llvm::LLVMContext::MD_loop)) {
            for (unsigned i = 1; i < existing->getNumOperands(); ++i) {
                // Drop any pre-existing unroll or distribute nodes — we'll
                // re-add them with our desired values.
                if (auto* op = llvm::dyn_cast<llvm::MDNode>(existing->getOperand(i))) {
                    if (op->getNumOperands() > 0) {
                        if (auto* s = llvm::dyn_cast<llvm::MDString>(op->getOperand(0))) {
                            llvm::StringRef name = s->getString();
                            if (name.starts_with("llvm.loop.unroll") ||
                                name.starts_with("llvm.loop.distribute"))
                                continue;
                        }
                    }
                }
                mds.push_back(existing->getOperand(i));
            }
        }

        // llvm.loop.unroll.disable = true  → prevent over-unrolling that
        // would expand the body beyond the µop cache capacity.
        mds.push_back(llvm::MDNode::get(Ctx, {
            llvm::MDString::get(Ctx, "llvm.loop.unroll.disable"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt1Ty(Ctx), 1))
        }));
        // llvm.loop.distribute.enable = true  → if LoopDistributePass sees
        mds.push_back(llvm::MDNode::get(Ctx, {
            llvm::MDString::get(Ctx, "llvm.loop.distribute.enable"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt1Ty(Ctx), 1))
        }));

        llvm::MDNode* md = llvm::MDNode::get(Ctx, mds);
        md->replaceOperandWith(0, md); // make self-referential
        back->setMetadata(llvm::LLVMContext::MD_loop, md);
    }

    // Estimate loop body frontend pressure as target-aware "uop-like" cost
    // (TTI user cost), excluding subloop blocks.
    static unsigned estimateLoopUops(llvm::Loop* L, llvm::LoopInfo& LI,
                                     const llvm::TargetTransformInfo& TTI) {
        unsigned count = 0;
        for (llvm::BasicBlock* BB : L->blocks()) {
            // Only count blocks whose innermost containing loop is exactly L
            // (i.e. direct members, not blocks in a subloop).
            if (LI.getLoopFor(BB) == L) {
                for (const llvm::Instruction& I : *BB)
                    count += instructionUopCost(I, TTI);
            }
        }
        return count;
    }

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& LI = FAM.getResult<llvm::LoopAnalysis>(F);
        auto& TTI = FAM.getResult<llvm::TargetIRAnalysis>(F);
        llvm::LLVMContext& Ctx = F.getContext();
        const UnifiedExecutionModel model = buildExecutionModel(F, TTI);
        bool changed = false;

        // Use a worklist to process all loops (including nested loops).
        llvm::SmallVector<llvm::Loop*, 16> worklist;
        for (llvm::Loop* top : LI) {
            worklist.push_back(top);
            for (llvm::Loop* sub : top->getLoopsInPreorder())
                worklist.push_back(sub);
        }
        for (llvm::Loop* L : worklist) {
            if (estimateLoopUops(L, LI, TTI) > model.opcacheLoopUopBudget) {
                annotateLoop(L, Ctx);
                changed = true;
            }
        }
        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }
};

// ── Rule 5: Large-Array Streaming Store Pass ─────────────────────────────────
struct LargeArrayStreamingStorePass
    : public llvm::PassInfoMixin<LargeArrayStreamingStorePass> {
    static bool isArrayElementStore(const llvm::StoreInst* SI) {
        if (llvm::MDNode* tbaa = SI->getMetadata(llvm::LLVMContext::MD_tbaa)) {
            if (tbaa->getNumOperands() < 2) return false;
            auto* typeNode = llvm::dyn_cast<llvm::MDNode>(tbaa->getOperand(0));
            if (!typeNode || typeNode->getNumOperands() < 1) return false;
            auto* s = llvm::dyn_cast<llvm::MDString>(typeNode->getOperand(0));
            return s && s->getString() == "array element";
        }
        return false;
    }

    static bool loopLikelyReadsStoreTarget(llvm::Loop* L, const llvm::StoreInst* SI) {
        const llvm::Value* storeBase = getBasePointer(SI->getPointerOperand());
        for (llvm::BasicBlock* BB : L->blocks()) {
            for (const llvm::Instruction& I : *BB) {
                auto* LI = llvm::dyn_cast<llvm::LoadInst>(&I);
                if (!LI || LI->isVolatile() || LI->isAtomic()) continue;
                if (getBasePointer(LI->getPointerOperand()) == storeBase)
                    return true;
            }
        }
        return false;
    }

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& LI  = FAM.getResult<llvm::LoopAnalysis>(F);
        auto& SE  = FAM.getResult<llvm::ScalarEvolutionAnalysis>(F);
        auto& TTI = FAM.getResult<llvm::TargetIRAnalysis>(F);
        const UnifiedExecutionModel model = buildExecutionModel(F, TTI);

        // Build a map from BasicBlock → containing loop (innermost).
        llvm::DenseMap<llvm::BasicBlock*, llvm::Loop*> bbLoop;
        for (llvm::Loop* top : LI) {
            for (llvm::Loop* L : top->getLoopsInPreorder()) {
                for (llvm::BasicBlock* BB : L->blocks())
                    bbLoop[BB] = L;
            }
            for (llvm::BasicBlock* BB : top->blocks())
                if (!bbLoop.count(BB)) bbLoop[BB] = top;
        }

        // TBAA "array element" type name used by OmScript codegen.
        llvm::LLVMContext& Ctx = F.getContext();
        llvm::MDNode* ntMD = llvm::MDNode::get(
            Ctx, llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 1)));

        llvm::DenseMap<llvm::Loop*, unsigned> promotedStoresPerLoop;
        bool changed = false;
        for (llvm::BasicBlock& BB : F) {
            auto it = bbLoop.find(&BB);
            if (it == bbLoop.end()) continue;
            llvm::Loop* L = it->second;

            // Estimate trip count using SCEV.
            llvm::BasicBlock* exitBB = L->getExitingBlock();
            if (!exitBB) continue;
            const llvm::SCEV* tripCount = SE.getBackedgeTakenCount(L);
            if (!tripCount || llvm::isa<llvm::SCEVCouldNotCompute>(tripCount))
                continue;
            if (auto* constTrip = llvm::dyn_cast<llvm::SCEVConstant>(tripCount)) {
                if (constTrip->getValue()->getSExtValue()
                    < static_cast<int64_t>(model.streamingMinElements))
                    continue;
            } else {
                // Non-constant trip count: conservatively skip unless
                continue;
            }

            for (llvm::Instruction& I : BB) {
                auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I);
                if (!SI) continue;
                if (SI->isVolatile() || SI->isAtomic()) continue;
                // Skip stores that are already non-temporal.
                if (SI->getMetadata(llvm::LLVMContext::MD_nontemporal)) continue;
                // Avoid overwhelming write-combining buffers.
                if (promotedStoresPerLoop[L] >= model.writeCombiningStoreBudget)
                    continue;
                // Only promote stores with array-element TBAA.
                if (!isArrayElementStore(SI)) continue;
                // Basic reuse filter: if loop also reads from the same base object,
                // the data is likely reused and should stay cache-resident.
                if (loopLikelyReadsStoreTarget(L, SI)) continue;
                // Promote to non-temporal store.
                SI->setMetadata(llvm::LLVMContext::MD_nontemporal, ntMD);
                ++promotedStoresPerLoop[L];
                changed = true;
            }
        }
        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }
};

// ── Memory Access Phase Reordering (MAPR) ─────────────────────────────────────
struct MemoryAccessPhaseReorderPass
    : public llvm::PassInfoMixin<MemoryAccessPhaseReorderPass> {
    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& AA = FAM.getResult<llvm::AAManager>(F);
        auto& TTI = FAM.getResult<llvm::TargetIRAnalysis>(F);
        const UnifiedExecutionModel model = buildExecutionModel(F, TTI);
        bool changed = false;
        for (auto& BB : F)
            changed |= processBlock(BB, AA, model.maprMaxHoistDistance);
        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }

private:
    // Compute the set of instructions in BB that transitively contribute to
    static llvm::SmallPtrSet<llvm::Instruction*, 8>
    addressDeps(const llvm::LoadInst* LI, const llvm::BasicBlock* BB) {
        llvm::SmallPtrSet<llvm::Instruction*, 8> deps;
        llvm::SmallVector<const llvm::Value*, 8> wl;
        wl.push_back(LI->getPointerOperand());
        while (!wl.empty()) {
            const auto* V = wl.pop_back_val();
            const auto* I = llvm::dyn_cast<llvm::Instruction>(V);
            if (!I || I->getParent() != BB) continue;
            if (!deps.insert(const_cast<llvm::Instruction*>(I)).second) continue;
            for (const auto& op : I->operands())
                wl.push_back(op.get());
        }
        return deps;
    }

    // Attempt to hoist LI as far toward the top of its basic block as alias
    // analysis and data dependencies allow.  Returns true if the load moved.
    static bool tryHoistLoad(llvm::LoadInst* LI, llvm::AAResults& AA,
                             unsigned maxHoistDist) {
        llvm::BasicBlock* BB = LI->getParent();
        const auto deps    = addressDeps(LI, BB);
        const auto loadLoc = llvm::MemoryLocation::get(LI);

        // `insertBefore`: the earliest safe instruction we found so far
        // (the load will be placed immediately before it).
        llvm::Instruction* insertBefore = nullptr;

        llvm::BasicBlock::iterator it(LI);
        unsigned dist = 0;
        while (it != BB->begin() && dist < maxHoistDist) {
            --it;
            ++dist;
            llvm::Instruction* pred = &*it;

            // Hard stop: PHI nodes must stay at the top of their basic block.
            // Hoisting the load before a PHI node would violate LLVM IR invariants.
            if (llvm::isa<llvm::PHINode>(pred)) break;

            // Hard stop: `pred` is in the address-computation chain of LI.
            // Moving LI before pred would use pred's result before it exists.
            if (deps.count(pred)) break;

            // Non-volatile, non-atomic loads are pure from a memory-ordering
            // perspective — loads don't conflict with each other.
            if (auto* prevLI = llvm::dyn_cast<llvm::LoadInst>(pred)) {
                if (prevLI->isVolatile() || prevLI->isAtomic()) break;
                insertBefore = pred;
                continue;
            }

            // Store: only safe to hoist past if provably NoAlias.
            if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(pred)) {
                if (SI->isVolatile() || SI->isAtomic()) break;
                // Require stronger provenance in addition to AA=NoAlias to
                // reduce risk from weak/incomplete alias metadata.
                if (!hasNoAliasProvenance(LI, SI)) break;
                if (AA.alias(loadLoc, llvm::MemoryLocation::get(SI))
                        != llvm::AliasResult::NoAlias)
                    break;
                insertBefore = pred;
                continue;
            }

            // Fence, call, or any instruction with memory side effects: stop.
            if (llvm::isa<llvm::FenceInst>(pred))    break;
            if (pred->mayReadOrWriteMemory())         break;
            if (pred->mayHaveSideEffects())           break;

            // Pure instruction (arithmetic, GEP, cast, …): safe to hoist past.
            insertBefore = pred;
        }

        if (!insertBefore) return false;
        LI->moveBefore(insertBefore);
        return true;
    }

    static bool processBlock(llvm::BasicBlock& BB, llvm::AAResults& AA,
                             unsigned maxHoistDist) {
        // Snapshot loads before we start moving them to avoid iterator issues.
        llvm::SmallVector<llvm::LoadInst*, 16> loads;
        for (auto& I : BB)
            if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(&I))
                if (!LI->isVolatile() && !LI->isAtomic())
                    loads.push_back(LI);

        // Process loads in forward order.  Hoisting L1 shrinks the store gap,
        // which only helps subsequent loads (L2, L3, …) hoist further.
        bool changed = false;
        for (auto* LI : loads)
            changed |= tryHoistLoad(LI, AA, maxHoistDist);
        return changed;
    }
};

// ── Tree Height Reduction Pass (RPAR) ─────────────────────────────────────────
struct TreeHeightReductionPass
    : public llvm::PassInfoMixin<TreeHeightReductionPass> {

    // Minimum number of leaves to make tree balancing profitable.
    static constexpr unsigned kMinLeaves = 4;

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& TTI = FAM.getResult<llvm::TargetIRAnalysis>(F);
        const UnifiedExecutionModel model = buildExecutionModel(F, TTI);
        bool changed = false;
        for (auto& BB : F)
            changed |= processBlock(BB, model);
        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }

private:
    using BinOp = llvm::BinaryOperator;

    // Returns true if BO can be reassociated (associative + commutative, and
    // for FP ops, the 'reassoc' fast-math flag must be set).
    static bool isBalanceable(const BinOp* BO) {
        switch (BO->getOpcode()) {
        case llvm::Instruction::Add:
        case llvm::Instruction::Mul:
        case llvm::Instruction::And:
        case llvm::Instruction::Or:
        case llvm::Instruction::Xor:
            return true;
        case llvm::Instruction::FAdd:
        case llvm::Instruction::FMul:
            return BO->getFastMathFlags().allowReassoc();
        default:
            return false;
        }
    }

    // Collect all scalar leaves of the binary-op tree rooted at V into
    static void collectLeaves(llvm::Value* V, unsigned opcode,
                               llvm::BasicBlock* BB,
                               llvm::SmallVectorImpl<llvm::Value*>& leaves,
                               bool isRoot) {
        auto* BO = llvm::dyn_cast<BinOp>(V);
        if (!BO || BO->getOpcode() != opcode || BO->getParent() != BB
                || !isBalanceable(BO)
                || (!isRoot && !BO->hasOneUse())) {
            leaves.push_back(V);
            return;
        }
        collectLeaves(BO->getOperand(0), opcode, BB, leaves, /*isRoot=*/false);
        collectLeaves(BO->getOperand(1), opcode, BB, leaves, /*isRoot=*/false);
    }

    // Recursively build a balanced binary tree over leaves[lo..hi) using
    static llvm::Value* buildTree(llvm::SmallVectorImpl<llvm::Value*>& leaves,
                                   unsigned lo, unsigned hi,
                                   unsigned opcode,
                                   llvm::FastMathFlags fmf,
                                   llvm::IRBuilder<>& B) {
        if (hi - lo == 1) return leaves[lo];
        const unsigned mid = (lo + hi) / 2;
        auto* lhs = buildTree(leaves, lo,  mid, opcode, fmf, B);
        auto* rhs = buildTree(leaves, mid, hi,  opcode, fmf, B);
        llvm::Value* result = nullptr;
        switch (opcode) {
        case llvm::Instruction::Add:  result = B.CreateAdd (lhs, rhs); break;
        case llvm::Instruction::Mul:  result = B.CreateMul (lhs, rhs); break;
        case llvm::Instruction::And:  result = B.CreateAnd (lhs, rhs); break;
        case llvm::Instruction::Or:   result = B.CreateOr  (lhs, rhs); break;
        case llvm::Instruction::Xor:  result = B.CreateXor (lhs, rhs); break;
        case llvm::Instruction::FAdd:
            result = B.CreateFAdd(lhs, rhs);
            llvm::cast<llvm::Instruction>(result)->setFastMathFlags(fmf);
            break;
        case llvm::Instruction::FMul:
            result = B.CreateFMul(lhs, rhs);
            llvm::cast<llvm::Instruction>(result)->setFastMathFlags(fmf);
            break;
        default: llvm_unreachable("unexpected opcode in TreeHeightReduction::buildTree");
        }
        return result;
    }

    static bool processBlock(llvm::BasicBlock& BB, const UnifiedExecutionModel& model) {
        // Identify root BinOps: an instruction whose result is NOT consumed
        // by another same-opcode instruction in the same basic block.
        llvm::SmallVector<BinOp*, 16> roots;
        for (auto& I : BB) {
            auto* BO = llvm::dyn_cast<BinOp>(&I);
            if (!BO || !isBalanceable(BO)) continue;
            const unsigned opc = BO->getOpcode();
            bool isRoot = true;
            for (auto* U : BO->users()) {
                if (auto* UBO = llvm::dyn_cast<BinOp>(U))
                    if (UBO->getOpcode() == opc && UBO->getParent() == &BB) {
                        isRoot = false;
                        break;
                    }
            }
            if (isRoot) roots.push_back(BO);
        }

        bool changed = false;
        for (BinOp* root : roots) {
            const unsigned opc = root->getOpcode();
            llvm::SmallVector<llvm::Value*, 16> leaves;
            collectLeaves(root, opc, &BB, leaves, /*isRoot=*/true);
            if (leaves.size() < kMinLeaves) continue;
            // Guard against register-pressure blowups from very wide trees.
            if (leaves.size() > model.treeMaxLeaves) continue;
            // Multi-use roots often benefit less and can lengthen live ranges.
            if (!root->hasOneUse() && leaves.size() > 6) continue;

            // Build the balanced tree, inserting instructions just before root.
            llvm::IRBuilder<> builder(root);
            llvm::Value* balanced = buildTree(leaves, 0, leaves.size(),
                                               opc, root->getFastMathFlags(),
                                               builder);
            if (balanced == root) continue;
            root->replaceAllUsesWith(balanced);
            // The now-unused linear chain will be removed by ADCE/DCE.
            changed = true;
        }
        return changed;
    }
};

// ── Branch Entropy Reduction Pass (BER) ────────────────────────────────────────
struct BranchEntropyReductionPass
    : public llvm::PassInfoMixin<BranchEntropyReductionPass> {
    static constexpr uint32_t kProbDen = (1u << 31);

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& BPI = FAM.getResult<llvm::BranchProbabilityAnalysis>(F);
        auto& TTI = FAM.getResult<llvm::TargetIRAnalysis>(F);
        const UnifiedExecutionModel model = buildExecutionModel(F, TTI);
        bool changed = false;
        // Collect up-front to avoid iterator invalidation during CFG edits.
        llvm::SmallVector<llvm::BranchInst*, 16> candidates;
        for (auto& BB : F)
            if (auto* BI = llvm::dyn_cast<llvm::BranchInst>(BB.getTerminator()))
                if (BI->isConditional())
                    candidates.push_back(BI);
        for (auto* BI : candidates)
            changed |= tryConvertDiamond(BI, BPI, TTI, model);
        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }

private:
    // Returns the target-aware uop cost of pure (speculatable) non-terminator instructions in
    static unsigned estimatePureUops(const llvm::BasicBlock* BB,
                                     const llvm::TargetTransformInfo& TTI) {
        unsigned n = 0;
        for (const auto& I : *BB) {
            if (llvm::isa<llvm::DbgInfoIntrinsic>(&I)) continue;
            if (llvm::isa<llvm::BranchInst>(&I))       continue;
            if (llvm::isa<llvm::PHINode>(&I))           return UINT_MAX;
            if (I.mayHaveSideEffects() || I.mayReadOrWriteMemory())
                return UINT_MAX;
            n += instructionUopCost(I, TTI);
        }
        return n;
    }

    static bool tryConvertDiamond(llvm::BranchInst* BI,
                                   llvm::BranchProbabilityInfo& BPI,
                                   const llvm::TargetTransformInfo& TTI,
                                   const UnifiedExecutionModel& model) {
        llvm::BasicBlock* head    = BI->getParent();
        llvm::BasicBlock* trueBB  = BI->getSuccessor(0);
        llvm::BasicBlock* falseBB = BI->getSuccessor(1);
        if (trueBB == falseBB) return false;

        const uint32_t num = BPI.getEdgeProbability(head, trueBB).getNumerator();
        const double trueProb = static_cast<double>(num) / static_cast<double>(kProbDen);
        const double minProb = std::min(trueProb, 1.0 - trueProb);
        const bool hasProfWeights = BI->getMetadata(llvm::LLVMContext::MD_prof) != nullptr;

        // Both arms must have exactly one predecessor (head) — otherwise
        // cloning their instructions into head would create multiple definitions.
        if (!trueBB->getSinglePredecessor())  return false;
        if (!falseBB->getSinglePredecessor()) return false;

        // Both arms must end with an unconditional branch to the same merge block.
        auto* trueTerm  = llvm::dyn_cast<llvm::BranchInst>(trueBB->getTerminator());
        auto* falseTerm = llvm::dyn_cast<llvm::BranchInst>(falseBB->getTerminator());
        if (!trueTerm  || trueTerm->isConditional())  return false;
        if (!falseTerm || falseTerm->isConditional()) return false;
        llvm::BasicBlock* merge = trueTerm->getSuccessor(0);
        if (falseTerm->getSuccessor(0) != merge) return false;

        // Arms must be purely computational; check combined uop budget.
        const unsigned trueN  = estimatePureUops(trueBB, TTI);
        const unsigned falseN = estimatePureUops(falseBB, TTI);
        if (trueN  == UINT_MAX) return false;
        if (falseN == UINT_MAX) return false;
        if (trueN + falseN > model.branchSpeculationUopBudget) return false;
        if (trueN + falseN == 0)           return false;
        // Expected value model:
        const double confidenceScaledRate = hasProfWeights ? minProb : (minProb * 0.50);
        const double expectedMispredictCost =
            confidenceScaledRate * model.branchMispredictPenaltyUops;
        if (static_cast<double>(trueN + falseN) >= expectedMispredictCost)
            return false;

        // Every phi in the merge block must have exactly two incoming edges,
        // both from our arms.
        for (const auto& phi : merge->phis()) {
            if (phi.getNumIncomingValues() != 2)        return false;
            const auto* blk0 = phi.getIncomingBlock(0);
            const auto* blk1 = phi.getIncomingBlock(1);
            if ((blk0 != trueBB || blk1 != falseBB) &&
                (blk0 != falseBB || blk1 != trueBB)) return false;
        }

        // Verify that arm instructions are only used within their own arm or
        auto checkEscapes = [&](const llvm::BasicBlock* arm) {
            for (const auto& I : *arm) {
                if (llvm::isa<llvm::BranchInst>(&I)) continue;
                for (const auto* U : I.users()) {
                    const auto* UI = llvm::dyn_cast<llvm::Instruction>(U);
                    if (!UI) return false;
                    if (UI->getParent() == arm)   continue;
                    if (UI->getParent() == merge && llvm::isa<llvm::PHINode>(UI)) continue;
                    return false;
                }
            }
            return true;
        };
        if (!checkEscapes(trueBB) || !checkEscapes(falseBB)) return false;

        // ── Transformation ────────────────────────────────────────────────
        llvm::Value* cond = BI->getCondition();
        llvm::IRBuilder<> builder(BI);

        // Clone arm instructions into head, remapping intra-arm uses.
        llvm::ValueToValueMapTy trueVM, falseVM;
        auto cloneArm = [&](const llvm::BasicBlock* arm,
                             llvm::ValueToValueMapTy& vm) {
            for (const auto& I : *arm) {
                if (llvm::isa<llvm::BranchInst>(&I)) continue;
                auto* clone = I.clone();
                for (auto& op : clone->operands()) {
                    llvm::Value* mapped = vm.lookup(op.get());
                    if (mapped) op = mapped;
                }
                builder.Insert(clone);
                vm[&I] = clone;
            }
        };
        cloneArm(trueBB,  trueVM);
        cloneArm(falseBB, falseVM);

        // Replace phi nodes in merge with select instructions.
        llvm::SmallVector<llvm::PHINode*, 8> phis;
        for (auto& phi : merge->phis())
            phis.push_back(&phi);
        for (auto* phi : phis) {
            llvm::Value* tv = phi->getIncomingValueForBlock(trueBB);
            llvm::Value* fv = phi->getIncomingValueForBlock(falseBB);
            {
                llvm::Value* m = trueVM.lookup(tv);
                if (m) tv = m;
            }
            {
                llvm::Value* m = falseVM.lookup(fv);
                if (m) fv = m;
            }
            phi->replaceAllUsesWith(
                builder.CreateSelect(cond, tv, fv, phi->getName()));
            phi->eraseFromParent();
        }

        // Replace the conditional branch with an unconditional jump to merge.
        // builder is still positioned before BI, so CreateBr inserts there.
        builder.CreateBr(merge);
        BI->eraseFromParent();
        // trueBB and falseBB are now dead; SimplifyCFG will remove them.
        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct StoreWideningPass
    : public llvm::PassInfoMixin<StoreWideningPass> {

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& DL = F.getParent()->getDataLayout();
        (void)FAM;
        bool changed = false;

        for (auto& BB : F) {
            changed |= widenStoresInBlock(BB, DL);
        }
        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }

private:
    struct StoreInfo {
        llvm::StoreInst* SI;
        int64_t offset;      // byte offset from base pointer
        unsigned sizeBytes;  // store size in bytes
    };

    // Try to get a base pointer and constant byte offset from a pointer value.
    static bool decomposePointer(const llvm::Value* ptr, const llvm::DataLayout& DL,
                                 const llvm::Value*& base, int64_t& offset) {
        base = ptr;
        offset = 0;
        // Walk through GEPs accumulating constant offsets.
        while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(base)) {
            llvm::APInt gepOff(64, 0);
            if (!gep->accumulateConstantOffset(DL, gepOff))
                return true; // can't decompose further, base stays at GEP
            offset += gepOff.getSExtValue();
            base = gep->getPointerOperand();
        }
        base = base->stripPointerCasts();
        return true;
    }

    // Check if there's any instruction between two stores (in the same BB)
    // that could alias with the store target.
    static bool hasAliasingInstructionBetween(llvm::StoreInst* first,
                                              llvm::StoreInst* last,
                                              const llvm::Value* base) {
        auto it = first->getIterator();
        auto end = last->getIterator();
        for (++it; it != end; ++it) {
            // Loads from the same base could read partially-written data.
            if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(&*it)) {
                const llvm::Value* loadBase = LI->getPointerOperand()->stripPointerCasts();
                // Conservative: if we can't prove no-alias, bail.
                if (loadBase == base)
                    return true;
                // If the load is from an alloca and the store base is different, safe.
                if (!llvm::isa<llvm::AllocaInst>(loadBase) ||
                    !llvm::isa<llvm::AllocaInst>(base))
                    return true; // conservative for non-alloca
            }
            // Calls might alias anything.
            if (llvm::isa<llvm::CallBase>(&*it)) {
                // Allow pure intrinsics (dbg, lifetime, etc.)
                if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&*it)) {
                    if (CI->getCalledFunction() &&
                        CI->getCalledFunction()->isIntrinsic() &&
                        CI->getCalledFunction()->doesNotAccessMemory())
                        continue;
                }
                return true;
            }
            // Any other store to the same base is a conflict.
            if (auto* otherSI = llvm::dyn_cast<llvm::StoreInst>(&*it)) {
                if (otherSI != first && otherSI != last) {
                    const llvm::Value* storeBase =
                        otherSI->getPointerOperand()->stripPointerCasts();
                    if (storeBase == base)
                        continue; // same base = will be in our group, not a conflict
                }
            }
        }
        return false;
    }

    bool widenStoresInBlock(llvm::BasicBlock& BB, const llvm::DataLayout& DL) {
        // Collect all simple (non-volatile, non-atomic) stores in program order.
        llvm::SmallVector<StoreInfo, 16> stores;
        for (auto& I : BB) {
            auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I);
            if (!SI || SI->isVolatile() || SI->isAtomic()) continue;
            if (!SI->getValueOperand()->getType()->isIntegerTy() &&
                !SI->getValueOperand()->getType()->isFloatingPointTy())
                continue;

            const llvm::Value* base = nullptr;
            int64_t offset = 0;
            if (!decomposePointer(SI->getPointerOperand(), DL, base, offset))
                continue;

            unsigned size = DL.getTypeStoreSize(SI->getValueOperand()->getType());
            stores.push_back({SI, offset, size});
        }

        if (stores.size() < 2) return false;

        // Group stores by base pointer.
        llvm::DenseMap<const llvm::Value*, llvm::SmallVector<size_t, 8>> groups;
        for (size_t i = 0; i < stores.size(); ++i) {
            const llvm::Value* base = nullptr;
            int64_t off = 0;
            decomposePointer(stores[i].SI->getPointerOperand(), DL, base, off);
            groups[base].push_back(i);
        }

        bool changed = false;
        for (auto& [base, indices] : groups) {
            if (indices.size() < 2) continue;

            // Sort by offset within this group.
            std::sort(indices.begin(), indices.end(),
                      [&](size_t a, size_t b) {
                          return stores[a].offset < stores[b].offset;
                      });

            // Find runs of consecutive constant stores.
            size_t i = 0;
            while (i < indices.size()) {
                // Start a new run.
                size_t runStart = i;
                int64_t expectedOff = stores[indices[i]].offset +
                                      stores[indices[i]].sizeBytes;
                bool allConst = llvm::isa<llvm::ConstantInt>(
                    stores[indices[i]].SI->getValueOperand());

                size_t j = i + 1;
                while (j < indices.size()) {
                    auto& s = stores[indices[j]];
                    if (s.offset != expectedOff) break;
                    if (!llvm::isa<llvm::ConstantInt>(s.SI->getValueOperand()))
                        allConst = false;
                    expectedOff = s.offset + s.sizeBytes;
                    ++j;
                }

                size_t runLen = j - runStart;
                if (runLen >= 2 && allConst) {
                    // Calculate total size.
                    unsigned totalBytes = 0;
                    for (size_t k = runStart; k < j; ++k)
                        totalBytes += stores[indices[k]].sizeBytes;

                    // Only widen to power-of-2 sizes up to 8 bytes (i64).
                    unsigned targetBytes = 1;
                    while (targetBytes < totalBytes && targetBytes <= 8)
                        targetBytes <<= 1;

                    if (totalBytes == targetBytes && targetBytes >= 2 &&
                        targetBytes <= 8) {
                        // Build the merged constant value.
                        llvm::StoreInst* firstSI = stores[indices[runStart]].SI;
                        llvm::StoreInst* lastSI = stores[indices[j-1]].SI;

                        // Check for aliasing instructions between first and last store.
                        if (hasAliasingInstructionBetween(firstSI, lastSI, base)) {
                            i = j;
                            continue;
                        }

                        // Compose the wide constant.
                        llvm::APInt wideVal(targetBytes * 8, 0);
                        bool littleEndian = DL.isLittleEndian();
                        int64_t baseOff = stores[indices[runStart]].offset;

                        for (size_t k = runStart; k < j; ++k) {
                            auto* ci = llvm::cast<llvm::ConstantInt>(
                                stores[indices[k]].SI->getValueOperand());
                            unsigned bitWidth = stores[indices[k]].sizeBytes * 8;
                            llvm::APInt val = ci->getValue().zextOrTrunc(targetBytes * 8);
                            int64_t relOff = stores[indices[k]].offset - baseOff;
                            unsigned shift;
                            if (littleEndian) {
                                shift = static_cast<unsigned>(relOff * 8);
                            } else {
                                shift = (targetBytes - relOff -
                                         stores[indices[k]].sizeBytes) * 8;
                            }
                            (void)bitWidth;
                            wideVal |= val.shl(shift);
                        }

                        // Create the wide store at the last store's position.
                        llvm::IRBuilder<> builder(lastSI);
                        llvm::Type* wideType = llvm::IntegerType::get(
                            BB.getContext(), targetBytes * 8);
                        llvm::Value* wideConst =
                            llvm::ConstantInt::get(wideType, wideVal);

                        // The store pointer must point to the first (lowest-offset) location.
                        llvm::Value* storePtr = firstSI->getPointerOperand();

                        llvm::StoreInst* wideSI = builder.CreateStore(wideConst, storePtr);
                        wideSI->setAlignment(firstSI->getAlign());

                        // Erase the original narrow stores.
                        for (size_t k = runStart; k < j; ++k) {
                            stores[indices[k]].SI->eraseFromParent();
                        }
                        changed = true;
                    }
                }
                i = j;
            }
        }
        return changed;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct CrossIterLoadReusePass
    : public llvm::PassInfoMixin<CrossIterLoadReusePass> {

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& LI = FAM.getResult<llvm::LoopAnalysis>(F);
        auto& SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(F);
        auto& DL = F.getParent()->getDataLayout();        bool changed = false;

        for (auto* L : LI) {
            changed |= optimizeLoop(*L, SE, DL);
            // Also process sub-loops.
            for (auto* SubL : L->getSubLoops())
                changed |= optimizeLoop(*SubL, SE, DL);
        }

        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }

private:
    struct StridedLoad {
        llvm::LoadInst* LI;
        const llvm::SCEV* baseSCEV;  // loop-invariant base
        int64_t stride;               // bytes per iteration
        int64_t offsetInIter;         // additional constant offset within iteration
    };

    bool optimizeLoop(llvm::Loop& L, llvm::ScalarEvolution& SE,
                      const llvm::DataLayout& /*DL*/) {
        // Only handle single-block loops for now (innermost hot loops).
        if (!L.getLoopLatch() || L.getNumBlocks() != 1) return false;
        llvm::BasicBlock* body = L.getHeader();

        // Collect loads with affine SCEVs.
        llvm::SmallVector<StridedLoad, 8> stridedLoads;
        for (auto& I : *body) {
            auto* load = llvm::dyn_cast<llvm::LoadInst>(&I);
            if (!load || load->isVolatile() || load->isAtomic()) continue;
            if (!load->getType()->isIntegerTy() && !load->getType()->isFloatingPointTy())
                continue;

            const llvm::SCEV* ptrSCEV = SE.getSCEV(load->getPointerOperand());
            auto* addRec = llvm::dyn_cast<llvm::SCEVAddRecExpr>(ptrSCEV);
            if (!addRec || addRec->getLoop() != &L) continue;
            if (!addRec->isAffine()) continue;

            auto* stepSCEV = llvm::dyn_cast<llvm::SCEVConstant>(addRec->getStepRecurrence(SE));
            if (!stepSCEV) continue;
            int64_t stride = stepSCEV->getAPInt().getSExtValue();
            if (stride == 0) continue;

            // The start SCEV is the loop-invariant base + offset.
            const llvm::SCEV* startSCEV = addRec->getStart();

            stridedLoads.push_back({load, startSCEV, stride, 0});
        }

        if (stridedLoads.size() < 2) return false;

        // Group loads by (stride, type).  Within each group, look for
        bool changed = false;
        llvm::DenseMap<llvm::LoadInst*, bool> eliminated;

        for (size_t i = 0; i < stridedLoads.size(); ++i) {
            if (eliminated.count(stridedLoads[i].LI)) continue;

            for (size_t j = i + 1; j < stridedLoads.size(); ++j) {
                if (eliminated.count(stridedLoads[j].LI)) continue;
                auto& a = stridedLoads[i];
                auto& b = stridedLoads[j];

                // Must have same stride and same loaded type.
                if (a.stride != b.stride) continue;
                if (a.LI->getType() != b.LI->getType()) continue;

                // Check if b's start = a's start + stride.
                // That means b at iteration i is the same as a at iteration i+1.
                const llvm::SCEV* diff = SE.getMinusSCEV(b.baseSCEV, a.baseSCEV);
                auto* diffConst = llvm::dyn_cast<llvm::SCEVConstant>(diff);
                if (!diffConst) continue;
                int64_t diffVal = diffConst->getAPInt().getSExtValue();

                if (diffVal == a.stride) {
                    // b[i] == a[i+1]: we can carry a's value forward!

                    llvm::BasicBlock* preheader = L.getLoopPreheader();
                    if (!preheader) continue;

                    // Create prologue load in preheader for the initial value.
                    llvm::IRBuilder<> preBuilder(preheader->getTerminator());
                    // b's value at the first iteration = load from b's initial address.

                    llvm::PHINode* phi = llvm::PHINode::Create(
                        b.LI->getType(), 2, "reuse.phi");
                    phi->insertBefore(body->begin());

                    // From the latch (= body for single-block loops): carry a's value.
                    phi->addIncoming(a.LI, body);

                    // From preheader: load the initial value of b.
                    // The initial address is the preheader value of b's pointer.
                    llvm::Value* bPtr = b.LI->getPointerOperand();
                    // In the preheader, the loop IV hasn't started yet, so the
                    bool ptrAvailableInPreheader = true;
                    if (auto* inst = llvm::dyn_cast<llvm::Instruction>(bPtr)) {
                        if (L.contains(inst)) {
                            ptrAvailableInPreheader = false;
                        }
                    }

                    if (!ptrAvailableInPreheader) {
                        // Can't hoist the pointer computation — abort this pair.
                        phi->eraseFromParent();
                        continue;
                    }

                    llvm::Value* initLoad = preBuilder.CreateLoad(
                        b.LI->getType(), bPtr, "reuse.init");

                    phi->addIncoming(initLoad, preheader);

                    // Replace all uses of b in the loop body with the PHI.
                    b.LI->replaceAllUsesWith(phi);
                    eliminated[b.LI] = true;
                    changed = true;
                    break; // one replacement per load
                }
            }
        }

        // Erase eliminated loads (safe because we replaced all uses).
        for (auto& [load, _] : eliminated) {
            if (load->use_empty())
                load->eraseFromParent();
        }

        return changed;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct AdvancedBranchWeightAnnotationPass
    : public llvm::PassInfoMixin<AdvancedBranchWeightAnnotationPass> {

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& LI = FAM.getResult<llvm::LoopAnalysis>(F);
        auto& SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(F);
        llvm::MDBuilder MDB(F.getContext());
        bool changed = false;

        for (auto& BB : F) {
            auto* BI = llvm::dyn_cast<llvm::BranchInst>(BB.getTerminator());
            if (!BI || !BI->isConditional()) continue;
            // Skip already-annotated branches (PGO takes precedence).
            if (BI->getMetadata(llvm::LLVMContext::MD_prof)) continue;

            llvm::Value* cond = BI->getCondition();
            llvm::BasicBlock* trueBB  = BI->getSuccessor(0);
            llvm::BasicBlock* falseBB = BI->getSuccessor(1);

            uint32_t trueW = 0, falseW = 0;  // 0 = no opinion yet

            // ── 1. SCEV / loop exit heuristic ────────────────────────────────
            llvm::Loop* loop = LI.getLoopFor(&BB);
            if (loop && trueW == 0) {
                bool trueExits  = !loop->contains(trueBB);
                bool falseExits = !loop->contains(falseBB);
                if (trueExits != falseExits) {
                    // Try to get exact trip count from SCEV.
                    // getSmallConstantTripCount returns 0 if unknown.
                    uint32_t tc = SE.getSmallConstantTripCount(loop);
                    if (tc > 1 && tc <= 256) {
                        // Exact: exit fires once every tc iterations.
                        if (trueExits) {
                            trueW  = 1;
                            falseW = static_cast<uint32_t>(tc - 1);
                        } else {
                            trueW  = static_cast<uint32_t>(tc - 1);
                            falseW = 1;
                        }
                    }
                    // Fallback: generic 88/12 loop-continue heuristic.
                    if (trueW == 0) {
                        if (trueExits) { trueW = 12; falseW = 88; }
                        else           { trueW = 88; falseW = 12; }
                    }
                }
            }

            // ── 2. Null-pointer check heuristic ──────────────────────────────
            if (trueW == 0) {
                if (auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(cond)) {
                    bool isEqNull = false, isNeNull = false;
                    for (unsigned oi = 0; oi < 2; ++oi) {
                        if (llvm::isa<llvm::ConstantPointerNull>(
                                icmp->getOperand(oi))) {
                            if (icmp->getPredicate() == llvm::ICmpInst::ICMP_EQ)
                                isEqNull = true;
                            else if (icmp->getPredicate() == llvm::ICmpInst::ICMP_NE)
                                isNeNull = true;
                            break;
                        }
                    }
                    if (isEqNull)       { trueW =  1; falseW = 99; }
                    else if (isNeNull)  { trueW = 99; falseW =  1; }
                }
            }

            // ── 3. Value-range / constant-check heuristics ───────────────────
            if (trueW == 0) {
                if (auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(cond)) {
                    auto pred = icmp->getPredicate();

                    // Identify the constant operand (if any).
                    llvm::ConstantInt* constOp = nullptr;
                    for (unsigned oi = 0; oi < 2; ++oi) {
                        constOp = llvm::dyn_cast<llvm::ConstantInt>(
                            icmp->getOperand(oi));
                        if (constOp) break;
                    }

                    // eq -1 → error sentinel (very cold when true)
                    if (constOp && pred == llvm::ICmpInst::ICMP_EQ &&
                        constOp->isMinusOne()) {
                        trueW = 5; falseW = 95;
                    }
                    // eq 0 → zero/false check (usually false in hot code)
                    else if (constOp && pred == llvm::ICmpInst::ICMP_EQ &&
                             constOp->isZero()) {
                        trueW = 20; falseW = 80;
                    }
                    // ne 0 → non-zero check (usually true in hot code)
                    else if (constOp && pred == llvm::ICmpInst::ICMP_NE &&
                             constOp->isZero()) {
                        trueW = 80; falseW = 20;
                    }
                    // slt 0 → negative result check (overflow / error, cold)
                    else if (pred == llvm::ICmpInst::ICMP_SLT && constOp &&
                             constOp->isZero()) {
                        trueW = 10; falseW = 90;
                    }
                    // sge 0 → non-negative (usually true)
                    else if (pred == llvm::ICmpInst::ICMP_SGE && constOp &&
                             constOp->isZero()) {
                        trueW = 90; falseW = 10;
                    }
                }
            }

            // ── 4. Opcode / call-pattern heuristic ──────────────────────────
            if (trueW == 0) {
                auto isColdBlock = [](llvm::BasicBlock* blk) {
                    for (auto& I : *blk) {
                        if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                            auto* callee = CI->getCalledFunction();
                            if (!callee) continue;
                            llvm::StringRef nm = callee->getName();
                            if (nm == "abort" || nm == "_abort" ||
                                nm == "__assert_fail" ||
                                nm.starts_with("__ubsan_handle") ||
                                nm.starts_with("__asan_report") ||
                                callee->hasFnAttribute(llvm::Attribute::NoReturn))
                                return true;
                        }
                        if (!llvm::isa<llvm::PHINode>(&I)) break;
                    }
                    return false;
                };
                if (isColdBlock(trueBB))  { trueW =  1; falseW = 99; }
                else if (isColdBlock(falseBB)) { trueW = 99; falseW =  1; }
            }

            // ── 5. WithOverflow intrinsic check (overflow is cold) ──────────
            if (trueW == 0) {
                if (auto* EVI = llvm::dyn_cast<llvm::ExtractValueInst>(cond)) {
                    if (EVI->getNumIndices() == 1 && EVI->getIndices()[0] == 1) {
                        if (auto* srcTy =
                                llvm::dyn_cast<llvm::StructType>(
                                    EVI->getAggregateOperand()->getType())) {
                            (void)srcTy;
                            // Check if aggregate comes from a with-overflow op.
                            if (auto* CI = llvm::dyn_cast<llvm::CallBase>(
                                    EVI->getAggregateOperand())) {
                                auto id = CI->getIntrinsicID();
                                if (id == llvm::Intrinsic::sadd_with_overflow ||
                                    id == llvm::Intrinsic::uadd_with_overflow ||
                                    id == llvm::Intrinsic::ssub_with_overflow ||
                                    id == llvm::Intrinsic::usub_with_overflow ||
                                    id == llvm::Intrinsic::smul_with_overflow ||
                                    id == llvm::Intrinsic::umul_with_overflow) {
                                    // Overflow is almost never the case.
                                    trueW =  5; falseW = 95;
                                }
                            }
                        }
                    }
                }
            }

            if (trueW != 0) {
                llvm::MDNode* md = MDB.createBranchWeights(trueW, falseW);
                BI->setMetadata(llvm::LLVMContext::MD_prof, md);
                changed = true;
            }
        }

        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct ConditionalStoreSinkPass
    : public llvm::PassInfoMixin<ConditionalStoreSinkPass> {

    // Maximum number of hot-path blocks to scan for liveness analysis.
    static constexpr unsigned kMaxHotPathBlocks = 12;

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& BPI = FAM.getResult<llvm::BranchProbabilityAnalysis>(F);
        bool changed = false;

        // Collect candidates to avoid iterator invalidation.
        llvm::SmallVector<llvm::BranchInst*, 16> branches;
        for (auto& BB : F) {
            auto* BI = llvm::dyn_cast<llvm::BranchInst>(BB.getTerminator());
            if (BI && BI->isConditional())
                branches.push_back(BI);
        }

        for (auto* BI : branches) {
            llvm::BasicBlock* BB = BI->getParent();
            llvm::BasicBlock* succ0 = BI->getSuccessor(0);
            llvm::BasicBlock* succ1 = BI->getSuccessor(1);

            // Need branch-weight metadata to determine hot/cold;
            // without metadata we have no reliable probability.
            if (!BI->getMetadata(llvm::LLVMContext::MD_prof)) continue;

            auto prob0 = BPI.getEdgeProbability(BB, static_cast<unsigned>(0));
            auto prob1 = BPI.getEdgeProbability(BB, static_cast<unsigned>(1));

            // Identify the cold successor: probability < 20%.
            llvm::BasicBlock* coldBB = nullptr;
            llvm::BasicBlock* hotBB  = nullptr;
            if (prob0.getNumerator() * 5 < prob0.getDenominator()) {
                coldBB = succ0;
                hotBB  = succ1;
            } else if (prob1.getNumerator() * 5 < prob1.getDenominator()) {
                coldBB = succ1;
                hotBB  = succ0;
            }
            if (!coldBB) continue;

            // (c) coldBB must have exactly one predecessor (BB).
            if (coldBB->getSinglePredecessor() != BB) continue;

            // Collect stores that satisfy the correctness conditions.
            llvm::SmallVector<llvm::StoreInst*, 4> toSink;
            for (auto& I : *BB) {
                auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I);
                if (!SI || SI->isVolatile() || SI->isAtomic()) continue;

                // (a) Only store to alloca-derived addresses (stack locals).
                llvm::Value* ptr = SI->getPointerOperand()->stripPointerCasts();
                if (!llvm::isa<llvm::AllocaInst>(ptr)) continue;

                // Only simple scalar types (not aggregate stores).
                if (!SI->getValueOperand()->getType()->isIntegerTy() &&
                    !SI->getValueOperand()->getType()->isFloatingPointTy() &&
                    !SI->getValueOperand()->getType()->isPointerTy())
                    continue;

                // Values used by the store must be available in coldBB.
                bool canSink = true;
                for (auto* op : {SI->getValueOperand(),
                                  SI->getPointerOperand()}) {
                    auto* opInst = llvm::dyn_cast<llvm::Instruction>(op);
                    if (!opInst) continue;
                    if (opInst->getParent() == BB) {
                        // Must come before SI in BB.
                        bool foundBefore = false;
                        for (auto& check : *BB) {
                            if (&check == opInst) { foundBefore = true; break; }
                            if (&check == SI) break;
                        }
                        if (!foundBefore) { canSink = false; break; }
                    }
                }
                if (!canSink) continue;

                // Check that there is no read of ptr between SI and the
                // branch terminator in the same block.
                bool readBetween = false;
                {
                    auto it = SI->getIterator();
                    for (++it; &*it != BI; ++it) {
                        if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(&*it)) {
                            if (LI->getPointerOperand()->stripPointerCasts() == ptr) {
                                readBetween = true; break;
                            }
                        }
                    }
                }
                if (readBetween) continue;

                // (b) Hot-path liveness check: BFS from hotBB, stop at
                bool liveOnHotPath = false;
                {
                    llvm::SmallVector<llvm::BasicBlock*, 16> worklist;
                    llvm::SmallPtrSet<llvm::BasicBlock*, 16> visited;
                    worklist.push_back(hotBB);
                    visited.insert(hotBB);
                    unsigned blocksChecked = 0;
                    while (!worklist.empty() && !liveOnHotPath &&
                           blocksChecked < kMaxHotPathBlocks) {
                        auto* cur = worklist.pop_back_val();
                        ++blocksChecked;
                        for (auto& I2 : *cur) {
                            if (auto* LI2 = llvm::dyn_cast<llvm::LoadInst>(&I2)) {
                                if (LI2->getPointerOperand()
                                         ->stripPointerCasts() == ptr) {
                                    liveOnHotPath = true; break;
                                }
                            }
                            // A new store to ptr kills liveness — hot path
                            // will get its own fresh value.
                            if (auto* SI2 =
                                    llvm::dyn_cast<llvm::StoreInst>(&I2)) {
                                if (SI2->getPointerOperand()
                                        ->stripPointerCasts() == ptr) {
                                    goto next_block;  // ptr overwritten, no liveness
                                }
                            }
                        }
                        // Enqueue successors.
                        for (auto* succ : llvm::successors(cur)) {
                            if (succ != coldBB && visited.insert(succ).second)
                                worklist.push_back(succ);
                        }
                        next_block:;
                    }
                    // If we exceeded the budget, conservatively assume live.
                    if (blocksChecked >= kMaxHotPathBlocks)
                        liveOnHotPath = true;
                }
                if (liveOnHotPath) continue;

                toSink.push_back(SI);
            }

            // Perform the sinking.
            for (auto* SI : toSink) {
                SI->moveBefore(coldBB->getFirstNonPHI());
                changed = true;
            }
        }
        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct RangeBoundsCheckHoistPass
    : public llvm::PassInfoMixin<RangeBoundsCheckHoistPass> {

    // Returns true if BB is a trivial abort block:
    //   PHINodes... printf... abort... unreachable   (or just abort + unreachable).
    static bool isAbortBlock(llvm::BasicBlock* BB) {
        bool sawAbort = false;
        for (auto& I : *BB) {
            if (llvm::isa<llvm::PHINode>(I)) continue;
            if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                auto* callee = CI->getCalledFunction();
                if (!callee) continue;
                llvm::StringRef nm = callee->getName();
                if (nm == "abort" || nm == "_abort" ||
                    nm.starts_with("__ubsan") || nm.starts_with("__asan") ||
                    callee->hasFnAttribute(llvm::Attribute::NoReturn))
                    sawAbort = true;
                continue;
            }
            if (llvm::isa<llvm::UnreachableInst>(I)) return sawAbort;
            // Any other non-call instruction means the block isn't a trivial abort.
            if (!llvm::isa<llvm::CallInst>(I)) return false;
        }
        return false;
    }

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& LI  = FAM.getResult<llvm::LoopAnalysis>(F);
        auto& SE  = FAM.getResult<llvm::ScalarEvolutionAnalysis>(F);
        llvm::MDBuilder MDB(F.getContext());
        bool changed = false;

        // Collect innermost loops.
        llvm::SmallVector<llvm::Loop*, 16> innerLoops;
        for (auto& topLevel : LI) {
            for (auto* L : topLevel->getLoopsInPreorder()) {
                if (L->isInnermost())
                    innerLoops.push_back(L);
            }
        }

        for (auto* loop : innerLoops) {
            // Need a preheader to insert the combined check.
            auto* preheader = loop->getLoopPreheader();
            if (!preheader) continue;

            // Need a single exiting block for exit count computation.
            auto* exitingBB = loop->getExitingBlock();
            if (!exitingBB) continue;

            // Collect bounds-check branches: icmp ult IV, len → condBr ok, fail.
            llvm::Value* sharedLen = nullptr;
            llvm::SmallVector<std::pair<llvm::BranchInst*, llvm::BasicBlock*>, 8> checkBranches;

            bool compatible = true;
            for (auto* BB : loop->blocks()) {
                auto* BI = llvm::dyn_cast<llvm::BranchInst>(BB->getTerminator());
                if (!BI || !BI->isConditional()) continue;
                auto* cmpInst = llvm::dyn_cast<llvm::ICmpInst>(BI->getCondition());
                if (!cmpInst) continue;
                if (cmpInst->getPredicate() != llvm::ICmpInst::ICMP_ULT) continue;

                // Identify ok / fail successors.
                llvm::BasicBlock* okBB   = BI->getSuccessor(0);
                llvm::BasicBlock* failBB = BI->getSuccessor(1);
                if (!isAbortBlock(failBB)) {
                    if (!isAbortBlock(okBB)) continue;
                    std::swap(okBB, failBB);
                    // Swapped: predicate is ULT and fail is succ(0) — we need
                    (void)okBB;
                    continue;
                }

                // The LHS (index) should be loop-varying; RHS (len) invariant.
                llvm::Value* lhsV = cmpInst->getOperand(0);
                llvm::Value* rhsV = cmpInst->getOperand(1);
                if (!loop->isLoopInvariant(rhsV)) {
                    std::swap(lhsV, rhsV);
                    if (!loop->isLoopInvariant(rhsV)) continue;
                }
                if (loop->isLoopInvariant(lhsV)) continue;

                // LHS must be an affine add-recurrence with positive step.
                const llvm::SCEV* lhsSCEV = SE.getSCEV(lhsV);
                if (!lhsSCEV || llvm::isa<llvm::SCEVCouldNotCompute>(lhsSCEV)) continue;
                auto* AR = llvm::dyn_cast<llvm::SCEVAddRecExpr>(lhsSCEV);
                if (!AR || !AR->isAffine() || AR->getLoop() != loop) continue;
                if (!SE.isKnownPositive(AR->getStepRecurrence(SE))) continue;

                // All checks must use the same len value.
                if (!sharedLen) {
                    sharedLen = rhsV;
                } else if (sharedLen != rhsV) {
                    compatible = false;
                    break;
                }

                checkBranches.push_back({BI, BI->getSuccessor(0) == failBB ? BI->getSuccessor(1) : BI->getSuccessor(0)});
            }

            if (!compatible || checkBranches.empty() || !sharedLen) continue;

            // Determine the SCEV for the AR in the first bounds check.
            auto* firstBI = checkBranches[0].first;
            auto* firstCmp = llvm::cast<llvm::ICmpInst>(firstBI->getCondition());
            llvm::Value* ivVal = firstCmp->getOperand(0);
            if (loop->isLoopInvariant(ivVal))
                ivVal = firstCmp->getOperand(1);
            const llvm::SCEV* ivSCEV = SE.getSCEV(ivVal);
            auto* AR = llvm::dyn_cast<llvm::SCEVAddRecExpr>(ivSCEV);
            if (!AR) continue;

            // Compute end = start + exitCount (exclusive upper bound of IV).
            const llvm::SCEV* exitCountSCEV = SE.getExitCount(loop, exitingBB);
            if (!exitCountSCEV || llvm::isa<llvm::SCEVCouldNotCompute>(exitCountSCEV))
                continue;
            const llvm::SCEV* endSCEV =
                SE.getAddExpr(AR->getStart(), exitCountSCEV);
            if (!endSCEV || llvm::isa<llvm::SCEVCouldNotCompute>(endSCEV)) continue;

            // Materialise `end` in the preheader.
            llvm::SCEVExpander expander(SE, F.getParent()->getDataLayout(), "rbch");
            llvm::Value* endVal = expander.expandCodeFor(
                endSCEV, sharedLen->getType(),
                preheader->getTerminator());

            // Insert pre-loop guard: `br (end <= len) ? loopHeader : failBB`
            auto* existingFailBB = checkBranches[0].first->getSuccessor(
                isAbortBlock(checkBranches[0].first->getSuccessor(1)) ? 1 : 0);

            llvm::IRBuilder<> phBuilder(preheader->getTerminator());
            llvm::Value* safeCheck = phBuilder.CreateICmpULE(
                endVal, sharedLen, "rbch.safe");
            llvm::MDNode* weights = MDB.createBranchWeights(1000000, 1);

            // Create a new "check" block that contains the original preheader
            // branch (→ loop header).  The preheader will branch to it when safe.
            llvm::BasicBlock* loopHeader = loop->getHeader();
            llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(
                F.getContext(), "rbch.check", &F, loopHeader);
            // Move the preheader's original terminator into checkBB.
            auto* origBr = preheader->getTerminator();
            origBr->removeFromParent();
            llvm::IRBuilder<> checkBBBuilder(checkBB);
            checkBB->getTerminator(); // ensure empty
            origBr->insertInto(checkBB, checkBB->end());
            // New preheader terminator: condBr safe → checkBB, failBB.
            phBuilder.CreateCondBr(safeCheck, checkBB, existingFailBB, weights);

            // Remove per-iteration bounds checks.
            for (auto& [BI, okBB] : checkBranches) {
                // Replace the conditional branch with unconditional → okBB.
                llvm::BranchInst::Create(okBB, BI);
                BI->eraseFromParent();
                changed = true;
            }
        }

        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct MACPatternPass
    : public llvm::PassInfoMixin<MACPatternPass> {

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& LI = FAM.getResult<llvm::LoopAnalysis>(F);
        bool changed = false;

        // Process innermost loops first.
        llvm::SmallVector<llvm::Loop*, 8> worklist;
        for (auto* L : LI)
            for (auto* SubL : llvm::depth_first(L))
                worklist.push_back(SubL);

        for (auto* L : worklist) {
            changed |= optimizeMAC(*L);
        }

        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }

private:
    bool optimizeMAC(llvm::Loop& L) {
        // Look for PHI nodes that are reduction accumulators.
        llvm::BasicBlock* header = L.getHeader();
        if (!header) return false;

        bool changed = false;

        for (auto& phi : header->phis()) {
            if (!phi.getType()->isFloatingPointTy()) continue;

            // Check if this PHI is a reduction: phi(init, update) where
            // update = fadd(phi, something) or update = fadd(something, phi).
            llvm::Value* loopVal = nullptr;
            for (unsigned i = 0; i < phi.getNumIncomingValues(); ++i) {
                if (L.contains(phi.getIncomingBlock(i))) {
                    loopVal = phi.getIncomingValue(i);
                    break;
                }
            }
            if (!loopVal) continue;

            auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(loopVal);
            if (!addInst || addInst->getOpcode() != llvm::Instruction::FAdd)
                continue;

            // One operand should be the PHI (accumulator), the other is the addend.
            llvm::Value* addend = nullptr;
            if (addInst->getOperand(0) == &phi) {
                addend = addInst->getOperand(1);
            } else if (addInst->getOperand(1) == &phi) {
                addend = addInst->getOperand(0);
            } else {
                continue;
            }

            // Check if addend is fmul(a, b) — the multiply-accumulate pattern.
            auto* mulInst = llvm::dyn_cast<llvm::BinaryOperator>(addend);
            if (!mulInst || mulInst->getOpcode() != llvm::Instruction::FMul)
                continue;

            // Only convert if the fadd has fast-math flags (or reassoc at minimum).
            if (!addInst->hasAllowReassoc()) continue;

            // Convert fadd(phi, fmul(a, b)) → fmuladd(a, b, phi).
            // This generates a single FMA instruction on CPUs that support it.
            llvm::Module* M = addInst->getModule();
            llvm::Function* fmuladd = llvm::Intrinsic::getDeclaration(
                M, llvm::Intrinsic::fmuladd, {phi.getType()});

            llvm::IRBuilder<> builder(addInst);
            llvm::FastMathFlags FMF = addInst->getFastMathFlags();
            builder.setFastMathFlags(FMF);

            llvm::Value* fmaResult = builder.CreateCall(
                fmuladd,
                {mulInst->getOperand(0), mulInst->getOperand(1), &phi},
                "mac");

            addInst->replaceAllUsesWith(fmaResult);
            addInst->eraseFromParent();

            // If the fmul has no other uses, it's now dead.
            if (mulInst->use_empty())
                mulInst->eraseFromParent();

            changed = true;
        }

        return changed;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct LoopInvariantReciprocalHoistPass
    : public llvm::PassInfoMixin<LoopInvariantReciprocalHoistPass> {

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM) {
        auto& LI = FAM.getResult<llvm::LoopAnalysis>(F);

        // Function-level fast-math attribute permits reciprocal substitution
        // even when individual fdiv instructions don't carry the arcp flag.
        const bool funcUnsafeFPMath =
            F.getFnAttribute("unsafe-fp-math").getValueAsString() == "true";

        bool changed = false;

        // Process all loops (innermost first via depth_first iteration).
        llvm::SmallVector<llvm::Loop*, 8> worklist;
        for (auto* L : LI)
            for (auto* SubL : llvm::depth_first(L))
                worklist.push_back(SubL);

        for (auto* L : worklist) {
            changed |= processLoop(*L, funcUnsafeFPMath);
        }

        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }

private:
    static bool fdivAllowsReciprocal(const llvm::BinaryOperator& BO,
                                     bool funcUnsafeFPMath) {
        const llvm::FastMathFlags FMF = BO.getFastMathFlags();
        return FMF.allowReciprocal() || FMF.allowReassoc() || funcUnsafeFPMath;
    }

    bool processLoop(llvm::Loop& L, bool funcUnsafeFPMath) {
        // We need a single-entry preheader to safely host the hoisted
        llvm::BasicBlock* preheader = L.getLoopPreheader();
        if (!preheader) return false;

        // Bucket eligible fdivs by (divisor, type).  Type is part of the key
        using Key = std::pair<llvm::Value*, llvm::Type*>;
        llvm::DenseMap<Key, llvm::SmallVector<llvm::BinaryOperator*, 4>> buckets;

        for (auto* BB : L.blocks()) {
            for (auto& I : *BB) {
                auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(&I);
                if (!binOp) continue;
                if (binOp->getOpcode() != llvm::Instruction::FDiv) continue;
                if (!fdivAllowsReciprocal(*binOp, funcUnsafeFPMath)) continue;

                llvm::Value* divisor = binOp->getOperand(1);
                if (!L.isLoopInvariant(divisor)) continue;

                // Skip constants — InstCombine already substitutes a folded
                // reciprocal for `fdiv x, C` under arcp.
                if (llvm::isa<llvm::Constant>(divisor)) continue;

                buckets[{divisor, binOp->getType()}].push_back(binOp);
            }
        }

        bool changed = false;
        for (auto& kv : buckets) {
            auto& divs = kv.second;
            // Threshold = 2: with a single fdiv there's no win — we'd just
            if (divs.size() < 2) continue;

            llvm::Value* divisor = kv.first.first;
            llvm::Type* ty       = kv.first.second;

            // Build "1.0" with matching shape.  ConstantFP::get on a vector
            // type produces the splat <1.0, 1.0, ...> automatically.
            llvm::Constant* one = llvm::ConstantFP::get(ty, 1.0);

            // Insert the hoisted reciprocal right before the preheader's
            llvm::IRBuilder<> preBuilder(preheader->getTerminator());

            // The hoisted instruction is brand new; give it the union of the
            llvm::FastMathFlags hoistedFMF;
            hoistedFMF.setFast();
            preBuilder.setFastMathFlags(hoistedFMF);

            llvm::Value* inv = preBuilder.CreateFDiv(one, divisor, "lirh.recip");

            // Replace each in-loop fdiv with an fmul by the hoisted reciprocal.
            for (auto* div : divs) {
                llvm::IRBuilder<> b(div);
                b.setFastMathFlags(div->getFastMathFlags());
                llvm::Value* mul = b.CreateFMul(div->getOperand(0), inv,
                                                "lirh.mul");
                div->replaceAllUsesWith(mul);
                div->eraseFromParent();
            }

            changed = true;
        }

        return changed;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct ConstArgPropagationPass
    : public llvm::PassInfoMixin<ConstArgPropagationPass> {

    llvm::PreservedAnalyses run(llvm::Module& M,
                                llvm::ModuleAnalysisManager& /*MAM*/) {
        bool changed = false;

        for (auto& F : M) {
            if (F.isDeclaration() || !F.hasLocalLinkage()) continue;
            if (F.use_empty()) continue;
            if (F.arg_empty()) continue;

            // For each argument, check if all call sites pass the same constant.
            for (auto& arg : F.args()) {
                llvm::Constant* commonVal = nullptr;
                bool allSame = true;
                bool allConst = true;

                for (auto* user : F.users()) {
                    auto* CB = llvm::dyn_cast<llvm::CallBase>(user);
                    if (!CB || CB->getCalledFunction() != &F) {
                        allSame = false;
                        break;
                    }

                    llvm::Value* passedVal = CB->getArgOperand(arg.getArgNo());
                    auto* constVal = llvm::dyn_cast<llvm::Constant>(passedVal);
                    if (!constVal) {
                        allConst = false;
                        allSame = false;
                        break;
                    }

                    if (!commonVal) {
                        commonVal = constVal;
                    } else if (commonVal != constVal) {
                        allSame = false;
                        break;
                    }
                }

                if (allSame && allConst && commonVal && !arg.use_empty()) {
                    arg.replaceAllUsesWith(commonVal);
                    changed = true;
                }
            }
        }

        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct InterproceduralNullabilityPass
    : public llvm::PassInfoMixin<InterproceduralNullabilityPass> {

    llvm::PreservedAnalyses run(llvm::Module& M,
                                llvm::ModuleAnalysisManager& /*MAM*/) {
        bool changed = false;
        const llvm::DataLayout& DL = M.getDataLayout();

        for (auto& F : M) {
            if (F.isDeclaration() || !F.hasLocalLinkage()) continue;
            if (F.use_empty() || F.arg_empty()) continue;

            for (auto& arg : F.args()) {
                if (!arg.getType()->isPointerTy()) continue;
                if (arg.hasAttribute(llvm::Attribute::NonNull)) continue;

                bool allNonNull = true;
                bool hasAnyCallSite = false;

                for (auto* user : F.users()) {
                    auto* CB = llvm::dyn_cast<llvm::CallBase>(user);
                    if (!CB || CB->getCalledFunction() != &F) {
                        allNonNull = false;
                        break;
                    }
                    hasAnyCallSite = true;
                    llvm::Value* passed = CB->getArgOperand(arg.getArgNo());
                    // isKnownNonZero from ValueTracking uses the entire
                    // value-tracking machinery (alloca, global, nonnull args, etc.).
                    if (!llvm::isKnownNonZero(passed, DL)) {
                        allNonNull = false;
                        break;
                    }
                }

                if (allNonNull && hasAnyCallSite) {
                    arg.addAttr(llvm::Attribute::NonNull);
                    changed = true;
                }
            }
        }

        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
    }
};

void CodeGenerator::resolveTargetCPU(std::string& cpu, std::string& features) const {
    const bool isNative = marchCpu_.empty() || marchCpu_ == "native";
    if (isNative) {
        cpu = llvm::sys::getHostCPUName().str();
        llvm::SubtargetFeatures featureSet;
#if LLVM_VERSION_MAJOR >= 19
        llvm::StringMap<bool> hostFeatures = llvm::sys::getHostCPUFeatures();
#else
        llvm::StringMap<bool> hostFeatures;
        llvm::sys::getHostCPUFeatures(hostFeatures);
#endif
        for (auto& feature : hostFeatures) {
            featureSet.AddFeature(feature.first(), feature.second);
        }
        features = featureSet.getString();
    } else {
        // Use the specified CPU; LLVM derives features from the CPU name.
        cpu = marchCpu_;
        features = "";
    }

    // -mtune overrides the CPU used for scheduling when -march is native or
    if (!mtuneCpu_.empty() && isNative) {
        if (mtuneCpu_ == "native") {
            cpu = llvm::sys::getHostCPUName().str();
        } else {
            cpu = mtuneCpu_;
        }
    }
}

// ---------------------------------------------------------------------------

std::unique_ptr<llvm::TargetMachine> CodeGenerator::createTargetMachine() const {
    const std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
#endif

    std::string error;
#if LLVM_VERSION_MAJOR >= 19
    auto* target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
#else
    auto* target = llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
#endif
    if (!target)
        return nullptr;

    llvm::TargetOptions opt;
    if (useFastMath_) {
#if LLVM_VERSION_MAJOR < 22
        opt.UnsafeFPMath = true;
#endif
        opt.NoInfsFPMath = true;
        opt.NoNaNsFPMath = true;
        opt.NoSignedZerosFPMath = true;
    }
    // At O2+, enable FP operation fusion (fmul+fadd → fma) at the backend
    if (optimizationLevel >= OptimizationLevel::O2) {
        opt.AllowFPOpFusion = llvm::FPOpFusion::Fast;
    }

    // ── Section granularity ────────────────────────────────────────────────
    if (optimizationLevel >= OptimizationLevel::O2) {
        opt.FunctionSections = true;
        opt.DataSections = true;
    }

    // ── Machine-level interprocedural / outlining optimizations ────────────
    if (optimizationLevel >= OptimizationLevel::O2) {
        opt.EnableMachineOutliner = true;
        if (!pgoUsePath_.empty()) {
            opt.EnableMachineFunctionSplitter = true;
        }
    }
    if (optimizationLevel >= OptimizationLevel::O3) {
        opt.EnableIPRA = true;
    }

    const std::optional<llvm::Reloc::Model> RM = usePIC_ ? llvm::Reloc::PIC_ : llvm::Reloc::Static;

    std::string cpu;
    std::string features;
    resolveTargetCPU(cpu, features);

    // Map the compiler's optimization level to LLVM's backend CodeGenOpt level
#if LLVM_VERSION_MAJOR >= 18
    llvm::CodeGenOptLevel cgOpt = llvm::CodeGenOptLevel::Default;
    switch (optimizationLevel) {
    case OptimizationLevel::O0:
        cgOpt = llvm::CodeGenOptLevel::None;
        break;
    case OptimizationLevel::O1:
        cgOpt = llvm::CodeGenOptLevel::Less;
        break;
    case OptimizationLevel::O2:
        cgOpt = llvm::CodeGenOptLevel::Default;
        break;
    case OptimizationLevel::O3:
        cgOpt = llvm::CodeGenOptLevel::Aggressive;
        break;
    }
#else
    llvm::CodeGenOpt::Level cgOpt = llvm::CodeGenOpt::Default;
    switch (optimizationLevel) {
    case OptimizationLevel::O0:
        cgOpt = llvm::CodeGenOpt::None;
        break;
    case OptimizationLevel::O1:
        cgOpt = llvm::CodeGenOpt::Less;
        break;
    case OptimizationLevel::O2:
        cgOpt = llvm::CodeGenOpt::Default;
        break;
    case OptimizationLevel::O3:
        cgOpt = llvm::CodeGenOpt::Aggressive;
        break;
    }
#endif

#if LLVM_VERSION_MAJOR >= 19
    return std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(targetTriple, cpu, features, opt, RM, std::nullopt, cgOpt));
#else
    return std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(targetTripleStr, cpu, features, opt, RM, std::nullopt, cgOpt));
#endif
}

// ── Canonical loop barrier helper ─────────────────────────────────────────────
static void addCanonicalLoopBarrier(llvm::FunctionPassManager& FPM) {
    FPM.addPass(llvm::LoopSimplifyPass());
    FPM.addPass(llvm::LCSSAPass());
}

// ── Canonical post-transformation cleanup helper ───────────────────────────────
// Adds the standard redundancy-elimination + dead-code-removal + CFG-cleanup
// sequence that follows any IR-mutating transformation (inlining, specialisation,
// superoptimiser, constant propagation, …).
//   withEarlyCSE : prepend EarlyCSEPass(UseMemorySSA=true) — enables memory-
//                  SSA-based load/store redundancy elimination before GVN.
//   withMemCpy   : include MemCpyOptPass after DSEPass — needed when the
//                  transformation may have introduced memcpy/memmove patterns.
static void addCanonicalCleanup(llvm::FunctionPassManager& FPM,
                                bool withEarlyCSE = true,
                                bool withMemCpy   = false) {
    if (withEarlyCSE)
        FPM.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
    FPM.addPass(llvm::SCCPPass());
    FPM.addPass(llvm::CorrelatedValuePropagationPass());
    FPM.addPass(llvm::NewGVNPass());
    FPM.addPass(llvm::DSEPass());
    if (withMemCpy)
        FPM.addPass(llvm::MemCpyOptPass());
    FPM.addPass(llvm::InstCombinePass());
    FPM.addPass(llvm::ADCEPass());
    FPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
}

void CodeGenerator::runOptimizationPasses() {
    // Ensure the native target is initialized before we try to create a
    // TargetMachine.  These calls are idempotent and fast after the first.
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    // ── OptimizationManager setup ─────────────────────────────────────────
    if (!optMgr_) {
        optMgr_ = std::make_unique<omscript::OptimizationManager>();
        optMgr_->setCostModel(omscript::createDefaultCostModel());
    }
    omscript::OptimizationManager& optMgr = *optMgr_;

    // Set the target triple on the module.
    const std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
    module->setTargetTriple(targetTriple);
#else
    module->setTargetTriple(targetTripleStr);
#endif

    auto targetMachine = createTargetMachine();
    if (targetMachine) {
        module->setDataLayout(targetMachine->createDataLayout());
    } else if (optimizationLevel != OptimizationLevel::O0) {
        // Data layout is required for correct optimization; warn but continue.
        llvm::errs() << "omsc: warning: could not create target machine; "
                        "optimization passes may produce suboptimal code\n";
    }

    if (optimizationLevel == OptimizationLevel::O0) {
        if (verbose_) {
            std::cout << "    Optimization level O0: skipping all passes" << '\n';
        }
        return;
    }

    // All optimization levels (O1, O2, O3) use the new pass manager's
    llvm::PipelineTuningOptions PTO;
    // At O1, keep vectorization and unrolling disabled to match LLVM's
    const bool atLeastO2 = optimizationLevel >= OptimizationLevel::O2;
    PTO.LoopVectorization = atLeastO2 && enableVectorize_;
    PTO.SLPVectorization  = atLeastO2 && enableVectorize_;
    // Re-enable LLVM's cost-model-driven loop unrolling.  The standard O3
    PTO.LoopUnrolling    = atLeastO2 && enableUnrollLoops_;
    PTO.LoopInterleaving = atLeastO2 && enableVectorize_;

    if (verbose_) {
        const char* levelStr =
            (optimizationLevel == OptimizationLevel::O1) ? "O1" :
            (optimizationLevel == OptimizationLevel::O3) ? "O3" : "O2";
        std::cout << "    Optimization level: " << levelStr << '\n';
        std::cout << "    Pipeline options:"
                  << " vectorize=" << (atLeastO2 && enableVectorize_ ? "on" : "off")
                  << ", unroll=" << (atLeastO2 && enableUnrollLoops_ ? "on" : "off")
                  << ", loop-optimize=" << (enableLoopOptimize_ ? "on" : "off")
                  << '\n';
        if (!pgoGenPath_.empty()) std::cout << "    PGO instrumentation: " << pgoGenPath_ << '\n';
        if (!pgoUsePath_.empty()) std::cout << "    PGO profile-use: " << pgoUsePath_ << '\n';
    }
    // Enable cross-function optimizations at O2 and above:
    if (optimizationLevel >= OptimizationLevel::O2) {
        PTO.MergeFunctions = true;
        PTO.CallGraphProfile = true;
        // Increase inliner threshold from the LLVM default (225) to account
        PTO.InlinerThreshold = 500;
    }
    if (optimizationLevel == OptimizationLevel::O3) {
        PTO.InlinerThreshold = 3000; // aggressive inlining for maximum IPC (compile time not a concern)
    }
    // ForgetAllSCEVInLoopUnroll forces SCEV to recompute trip counts after
    if (optimizationLevel >= OptimizationLevel::O2) {
        PTO.ForgetAllSCEVInLoopUnroll = true;
    }

    // ---------------------------------------------------------------------------
    std::optional<llvm::PGOOptions> pgoOpt;
    if (!pgoGenPath_.empty()) {
        // Instrumentation generation: insert counters, write .profraw on exit.
#if LLVM_VERSION_MAJOR >= 22
        pgoOpt = llvm::PGOOptions(pgoGenPath_,                // ProfileFile: output path for raw profile
                                  "",                         // CSProfileGenFile: context-sensitive profile (unused)
                                  "",                         // ProfileRemappingFile: symbol remapping (unused)
                                  "",                         // MemoryProfile: memory profile (unused)
                                  llvm::PGOOptions::IRInstr); // Action: IR-level instrumentation
#else
        pgoOpt = llvm::PGOOptions(pgoGenPath_, // ProfileFile: output path for raw profile
                                  "",          // CSProfileGenFile: context-sensitive profile (unused)
                                  "",          // ProfileRemappingFile: symbol remapping (unused)
                                  "",          // MemoryProfile: memory profile (unused)
                                  llvm::vfs::getRealFileSystem(),
                                  llvm::PGOOptions::IRInstr); // Action: IR-level instrumentation
#endif
    } else if (!pgoUsePath_.empty()) {
        // Profile-use: feed collected data into the optimization pipeline.
#if LLVM_VERSION_MAJOR >= 22
        pgoOpt = llvm::PGOOptions(pgoUsePath_,              // ProfileFile: input .profdata path
                                  "",                       // CSProfileGenFile: unused
                                  "",                       // ProfileRemappingFile: unused
                                  "",                       // MemoryProfile: unused
                                  llvm::PGOOptions::IRUse); // Action: apply IR-level profile
#else
        pgoOpt = llvm::PGOOptions(pgoUsePath_, // ProfileFile: input .profdata path
                                  "",          // CSProfileGenFile: unused
                                  "",          // ProfileRemappingFile: unused
                                  "",          // MemoryProfile: unused
                                  llvm::vfs::getRealFileSystem(),
                                  llvm::PGOOptions::IRUse); // Action: apply IR-level profile
#endif
    }

    llvm::PassBuilder PB(targetMachine.get(), PTO, pgoOpt);

    // At O1+, register pipeline-start passes for all optimization levels.
    if (optimizationLevel >= OptimizationLevel::O1) {
        const bool isO2start = optimizationLevel >= OptimizationLevel::O2;
        PB.registerPipelineStartEPCallback(
            [isO2start](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            MPM.addPass(llvm::InferFunctionAttrsPass());
            llvm::FunctionPassManager EarlyFPM;
            EarlyFPM.addPass(llvm::LowerExpectIntrinsicPass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(EarlyFPM)));
#if LLVM_VERSION_MAJOR < 20
            if (isO2start)
                MPM.addPass(llvm::SyntheticCountsPropagation());
#else
            (void)isO2start;
#endif
        });
    }

    // At O1+, register Reassociate and EarlyCSE early in the function pipeline.
    if (optimizationLevel >= OptimizationLevel::O1) {
        const bool useMemSSA = optimizationLevel >= OptimizationLevel::O2;
        PB.registerPeepholeEPCallback([useMemSSA](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            FPM.addPass(llvm::EarlyCSEPass(useMemSSA));
        });
    }

    // At O2+, infer memory attributes (readnone, readonly, argmemonly, etc.)
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool earlyO3 = optimizationLevel >= OptimizationLevel::O3;
        PB.registerOptimizerEarlyEPCallback(
            [earlyO3](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            // O2+: Interprocedural Nullability — scan all call sites; if a pointer
            MPM.addPass(InterproceduralNullabilityPass());
            // O2+: Bidirectional function-attribute inference.
            // Bottom-up: propagate readnone/readonly from leaves to callers.
            MPM.addPass(llvm::createModuleToPostOrderCGSCCPassAdaptor(
                llvm::PostOrderFunctionAttrsPass()));
            // Top-down: propagate constraints from callers to callees.
            MPM.addPass(llvm::ReversePostOrderFunctionAttrsPass());
            // O3: Superblock / hyperblock formation.
            if (earlyO3) {
                llvm::FunctionPassManager SuperblockFPM;
                // Phase 1: Superblock formation — duplicate blocks along
                // frequently-taken paths to form single-entry traces.
                SuperblockFPM.addPass(llvm::JumpThreadingPass());
                SuperblockFPM.addPass(llvm::DFAJumpThreadingPass());
                // Phase 2: Sharpen value ranges exposed by block duplication.
                SuperblockFPM.addPass(llvm::CorrelatedValuePropagationPass());
                // Phase 3: Hoist cheap instructions above remaining branches.
                SuperblockFPM.addPass(llvm::SpeculativeExecutionPass());
                // Phase 4: Hyperblock formation — convert branches to selects.
                SuperblockFPM.addPass(llvm::SimplifyCFGPass(hyperblockCFGOpts()));
                // Phase 5: Cleanup — combine and eliminate dead code.
                SuperblockFPM.addPass(llvm::InstCombinePass());
                SuperblockFPM.addPass(llvm::ADCEPass());
                MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(SuperblockFPM)));
            }
        });
    }

    // At O2+, register CallSiteSplitting in the scalar optimizer.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerScalarOptimizerLateEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            FPM.addPass(llvm::CallSiteSplittingPass());
        });
    }

    // ── Auto-parallelization pipeline configuration ─────────────────────

    // At O2+ with -floop-optimize, load the LLVM Polly polyhedral optimizer
#ifdef POLLY_LIB_PATH
    if (optimizationLevel >= OptimizationLevel::O2 && enableLoopOptimize_) {
        if (verbose_) {
            std::cout << "    Loading Polly polyhedral loop optimizer plugin..." << '\n';
        }
        auto pollyPlugin = llvm::PassPlugin::Load(POLLY_LIB_PATH);
        if (pollyPlugin) {
            pollyPlugin->registerPassBuilderCallbacks(PB);
            // Now that Polly has registered its cl::opt entries, configure
            if (enableParallelize_) {
                const char* pollyArgs[] = {
                    "omsc",
                    "-polly-parallel",
                    "-polly-vectorizer=stripmine",
                    "-polly-run-dce",
                    "-polly-run-inliner",
                    "-polly-invariant-load-hoisting",
                    "-polly-scheduling=dynamic",
                    "-polly-scheduling-chunksize=1",
                };
                llvm::cl::ParseCommandLineOptions(
                    sizeof(pollyArgs) / sizeof(pollyArgs[0]), pollyArgs,
                    "OmScript Polly auto-parallelization\n");
            }
            if (verbose_) {
                std::cout << "    Polly plugin loaded successfully"
                          << (enableParallelize_ ? " (parallel mode)" : "")
                          << '\n';
            }
        } else {
            // Polly not available in this environment — not actionable by the
            // user, so only report it in verbose mode.
            if (verbose_) {
                llvm::errs() << "omsc: note: Polly plugin not found; "
                                "polyhedral loop optimizations disabled\n";
            }
            llvm::consumeError(pollyPlugin.takeError());
        }
    }
#endif

    // NOTE: InferFunctionAttrsPass is intentionally NOT registered here.

    // At O3, register ArgumentPromotionPass late in the CGSCC pipeline
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerCGSCCOptimizerLateEPCallback(
            [](llvm::CGSCCPassManager& CGPM, llvm::OptimizationLevel /*Level*/) {
            CGPM.addPass(llvm::ArgumentPromotionPass());
        });
    }
    // AttributorCGSCCPass performs context-sensitive, call-graph-aware
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerCGSCCOptimizerLateEPCallback(
            [](llvm::CGSCCPassManager& CGPM, llvm::OptimizationLevel /*Level*/) {
            CGPM.addPass(llvm::PostOrderFunctionAttrsPass());
            CGPM.addPass(llvm::AttributorCGSCCPass());
        });
    }

    // At O2+ with -floop-optimize, register LoopDistributePass to run just
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool loopOpt = enableLoopOptimize_;
        const bool loopOptOrO3 = loopOpt || (optimizationLevel >= OptimizationLevel::O3);
        // Capture service pointers from the shared OptimizationManager.
        const omscript::LegalityService* legalitySvc = &optMgr.legality();
        const omscript::CostModel*       costModelPtr = optMgr.costModel();
        PB.registerVectorizerStartEPCallback(
            [loopOptOrO3, legalitySvc, costModelPtr](
                llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            if (loopOptOrO3) FPM.addPass(llvm::LoopDistributePass());
            FPM.addPass(llvm::LoopLoadEliminationPass());
            if (loopOptOrO3) FPM.addPass(llvm::LoopFusePass());
            FPM.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
            FPM.addPass(llvm::PromotePass());
            FPM.addPass(llvm::InstCombinePass());
            addCanonicalLoopBarrier(FPM);
            // OmPolyOpt: polyhedral loop optimizer.  Runs after loop normalization
            if (loopOptOrO3) {
                omscript::polyopt::PolyOptConfig polyConfig;
                polyConfig.legality  = legalitySvc;
                polyConfig.costModel = costModelPtr;
                FPM.addPass(omscript::polyopt::OmPolyOptFunctionPass(polyConfig));
            }
            FPM.addPass(llvm::createFunctionToLoopPassAdaptor(llvm::LoopRotatePass()));
            FPM.addPass(llvm::createFunctionToLoopPassAdaptor(
                llvm::LICMPass(llvm::LICMOptions()), /*UseMemorySSA=*/true));
            // Reassociate reorders commutative/associative expressions to
            // expose more CSE opportunities for GVN.
            FPM.addPass(llvm::ReassociatePass());
            FPM.addPass(llvm::NewGVNPass());
            FPM.addPass(llvm::InstSimplifyPass());
            FPM.addPass(llvm::AlignmentFromAssumptionsPass());
            // CorrelatedValuePropagation uses value-range information from
            FPM.addPass(llvm::CorrelatedValuePropagationPass());
            // ConstraintElimination uses a system of linear constraints derived
            FPM.addPass(llvm::ConstraintEliminationPass());
            // InductiveRangeCheckElimination removes bounds checks inside loops
            FPM.addPass(llvm::IRCEPass());
            // SimplifyCFG merges trivially-redundant blocks and eliminates
            FPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
        });
    }

    // At O2+ with -floop-optimize, register LoopInterchangePass at the end of
    if (optimizationLevel >= OptimizationLevel::O2 && enableLoopOptimize_) {
        PB.registerLoopOptimizerEndEPCallback(
            [](llvm::LoopPassManager& LPM, llvm::OptimizationLevel /*Level*/) { LPM.addPass(llvm::LoopInterchangePass()); });
    }

    // NOTE: AggressiveInstCombine is already part of the default O2/O3

    // At O2+, register all extra loop optimizer passes in consolidated callbacks.
    {
        const bool isO3 = optimizationLevel >= OptimizationLevel::O3;
        const bool loopOpt = enableLoopOptimize_;

        if (optimizationLevel >= OptimizationLevel::O2) {
            PB.registerLateLoopOptimizationsEPCallback([isO3, loopOpt](llvm::LoopPassManager& LPM, llvm::OptimizationLevel /*Level*/) {
                LPM.addPass(llvm::SimpleLoopUnswitchPass(/*NonTrivial=*/true));
                // LoopVersioningLICM creates a versioned copy of the loop
                if (loopOpt || isO3)
                    LPM.addPass(llvm::LoopVersioningLICMPass());
            });
        }

        if (optimizationLevel >= OptimizationLevel::O2) {
            PB.registerLoopOptimizerEndEPCallback([isO3, loopOpt](llvm::LoopPassManager& LPM, llvm::OptimizationLevel /*Level*/) {
                // LoopIdiomRecognize detects loop patterns that implement
                LPM.addPass(llvm::LoopIdiomRecognizePass());
                // IndVarSimplify canonicalizes induction variables for
                // vectorization and unrolling.
                LPM.addPass(llvm::IndVarSimplifyPass());
                // LoopDeletion removes provably dead loops.
                LPM.addPass(llvm::LoopDeletionPass());
                LPM.addPass(llvm::LoopInstSimplifyPass());
                LPM.addPass(llvm::LoopSimplifyCFGPass());
                // CanonicalizeFreezeInLoops moves freeze instructions out of
                LPM.addPass(llvm::CanonicalizeFreezeInLoopsPass());
                // LoopFlatten collapses nested loops with a simple inner trip
                if (isO3 || loopOpt) {
                    LPM.addPass(llvm::LoopFlattenPass());
                    // LoopUnrollAndJam unrolls an outer loop and fuses (jams)
                    LPM.addPass(llvm::LoopUnrollAndJamPass(/*OptLevel=*/isO3 ? 3 : 2));
                }
                // LoopPredication converts bounds checks inside loops into
                LPM.addPass(llvm::LoopPredicationPass());
                if (isO3) {
#if LLVM_VERSION_MAJOR >= 20
                    // LoopBoundSplit splits loops by bounds to enable better
                    LPM.addPass(llvm::LoopBoundSplitPass());
#else
                    // LoopReroll recognizes manually-unrolled loop patterns and
                    LPM.addPass(llvm::LoopRerollPass());
#endif
                }
            });
        }
    }

    // At O3, inject LoopDataPrefetchPass to insert software prefetch
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool isO3 = optimizationLevel >= OptimizationLevel::O3;
        const bool loopOpt = enableLoopOptimize_;
        PB.registerScalarOptimizerLateEPCallback([isO3, loopOpt](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            // LoopDataPrefetch inserts software prefetch instructions for loops
            if (isO3 || loopOpt) FPM.addPass(llvm::LoopDataPrefetchPass());
            // CVP uses value-range info to sharpen comparisons and convert
            // signed ops to unsigned.
            FPM.addPass(llvm::CorrelatedValuePropagationPass());
            FPM.addPass(llvm::BDCEPass());
            // ADCE removes dead code more aggressively than DCE.
            FPM.addPass(llvm::ADCEPass());
            // DSE removes stores whose values are never read.
            FPM.addPass(llvm::DSEPass());
            // MemCpyOpt optimizes memory transfer operations.
            FPM.addPass(llvm::MemCpyOptPass());
            // Float2Int converts float operations to integer when possible.
            FPM.addPass(llvm::Float2IntPass());
            // TailCallElim converts tail-recursive calls into loops.
            FPM.addPass(llvm::TailCallElimPass());
            FPM.addPass(llvm::MergeICmpsPass());
            FPM.addPass(llvm::MergedLoadStoreMotionPass());
            FPM.addPass(llvm::StraightLineStrengthReducePass());
            FPM.addPass(llvm::NaryReassociatePass());
            FPM.addPass(llvm::ReassociatePass());
            FPM.addPass(llvm::InferAlignmentPass());
            // SCCP: sparse conditional constant propagation — more powerful
            FPM.addPass(llvm::SCCPPass());
            FPM.addPass(llvm::InstCombinePass());
            // ConstraintElimination uses branch conditions to narrow value
            FPM.addPass(llvm::ConstraintEliminationPass());
            // JumpThreading threads branches through basic blocks with
            FPM.addPass(llvm::JumpThreadingPass());
            // DivRemPairs reuses quotient from div for rem, saving an
            FPM.addPass(llvm::DivRemPairsPass());
            // ConstantHoisting materializes expensive constants once and
            FPM.addPass(llvm::ConstantHoistingPass());
            // IRCE (Inductive Range Check Elimination) removes bounds checks
            FPM.addPass(llvm::IRCEPass());
            // PartiallyInlineLibCalls inlines the fast path of math library
            FPM.addPass(llvm::PartiallyInlineLibCallsPass());
            // SeparateConstOffsetFromGEP canonicalizes GEP chains by factoring
            FPM.addPass(llvm::SeparateConstOffsetFromGEPPass());
            // GuardWidening merges multiple guard checks into a single
            FPM.addPass(llvm::GuardWideningPass());
            if (isO3) {
                // LibCallsShrinkWrap wraps math library calls (sqrt, exp2, pow,
                FPM.addPass(llvm::LibCallsShrinkWrapPass());
                FPM.addPass(llvm::SpeculativeExecutionPass());
                FPM.addPass(llvm::DFAJumpThreadingPass());
                FPM.addPass(llvm::SinkingPass());
                // NewGVN is a graph-based GVN that catches redundancies
                // classic GVN misses (e.g. through PHI nodes and memory).
                FPM.addPass(llvm::NewGVNPass());
            }
            // Aggressive SimplifyCFG at the end to convert if-else chains
            FPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
        });
    }

    // At O2+, register VectorCombinePass and LoopSinkPass as one of the last
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            llvm::FunctionPassManager FPM;
            FPM.addPass(llvm::VectorCombinePass());
            FPM.addPass(llvm::LoopSinkPass());
            // LoadStoreVectorizer combines adjacent scalar loads/stores into
            FPM.addPass(llvm::LoadStoreVectorizerPass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
            // CalledValuePropagation propagates known function pointer values
            // through indirect calls, enabling devirtualization and inlining.
            MPM.addPass(llvm::CalledValuePropagationPass());
            // ConstantMerge deduplicates identical global constants across the
            MPM.addPass(llvm::ConstantMergePass());
        });
    }

    // ── Loop Re-Optimization Pass ──────────────────────────────────
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            llvm::FunctionPassManager ReOptFPM;
            // EarlyCSE with MemorySSA catches common redundancies.
            ReOptFPM.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
            // Re-run LICM to hoist newly-invariant code out of loops.
            llvm::LoopPassManager ReOptLPM;
            ReOptLPM.addPass(llvm::LICMPass(llvm::LICMOptions()));
            ReOptLPM.addPass(llvm::LoopInstSimplifyPass());
            ReOptLPM.addPass(llvm::IndVarSimplifyPass());
            ReOptFPM.addPass(llvm::createFunctionToLoopPassAdaptor(
                std::move(ReOptLPM), /*UseMemorySSA=*/true));
            // Clean up with InstCombine after loop re-optimization.
            ReOptFPM.addPass(llvm::InstCombinePass());
            // Re-run DSE to catch stores made dead by loop transformations.
            ReOptFPM.addPass(llvm::DSEPass());
            // Re-run SLP vectorizer to catch new vectorization
            // opportunities in inlined code.
            ReOptFPM.addPass(llvm::SLPVectorizerPass());
            // NewGVN catches deep redundancies using MemorySSA for more
            // precise memory dependency analysis than classic GVN.
            ReOptFPM.addPass(llvm::NewGVNPass());
            // Final CFG cleanup.
            ReOptFPM.addPass(llvm::SimplifyCFGPass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(ReOptFPM)));
        });
    }

    // At O3, add a post-vectorizer superblock formation phase.  After the
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            llvm::FunctionPassManager PostVecFPM;
            PostVecFPM.addPass(llvm::JumpThreadingPass());
            PostVecFPM.addPass(llvm::SpeculativeExecutionPass());
            PostVecFPM.addPass(llvm::SimplifyCFGPass(hyperblockCFGOpts()));
            PostVecFPM.addPass(llvm::InstCombinePass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(PostVecFPM)));
        });
    }

    // At O3, run dead argument elimination to remove unused function
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool isO3 = optimizationLevel >= OptimizationLevel::O3;
        PB.registerOptimizerLastEPCallback(
            [isO3](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            // IPSCCPPass performs inter-procedural sparse conditional constant
            MPM.addPass(llvm::IPSCCPPass());
            MPM.addPass(llvm::DeadArgumentEliminationPass());
            // PartialInlinerPass outlines cold regions (error handling, slow
            MPM.addPass(llvm::PartialInlinerPass());

            // ── Second IPO constant-propagation round (O3 only) ────────────
            if (isO3) {
                MPM.addPass(llvm::IPSCCPPass());
                llvm::FunctionPassManager IPO2FPM;
                IPO2FPM.addPass(llvm::SCCPPass());
                IPO2FPM.addPass(llvm::InstCombinePass());
                IPO2FPM.addPass(llvm::NewGVNPass());
                IPO2FPM.addPass(llvm::ADCEPass());
                MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(IPO2FPM)));
                MPM.addPass(llvm::DeadArgumentEliminationPass());

                // After the second IPSCCP + constant folding round, re-run
                MPM.addPass(llvm::createModuleToPostOrderCGSCCPassAdaptor(
                    llvm::PostOrderFunctionAttrsPass()));
                MPM.addPass(llvm::ReversePostOrderFunctionAttrsPass());
                // InferFunctionAttrsPass re-annotates library functions whose
                MPM.addPass(llvm::InferFunctionAttrsPass());
            }

            // EliminateAvailableExternallyPass removes bodies of externally-
            MPM.addPass(llvm::EliminateAvailableExternallyPass());
        });
    }

    // At O3, add a module-level AttributorPass after the main IPSCCP/inliner
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            MPM.addPass(llvm::AttributorPass());
            // After Attributor propagates new attributes (e.g. marks functions
            llvm::FunctionPassManager PostAttrFPM;
            llvm::LoopPassManager PostAttrLPM;
            PostAttrLPM.addPass(llvm::LICMPass(llvm::LICMOptions()));
            PostAttrFPM.addPass(llvm::createFunctionToLoopPassAdaptor(
                std::move(PostAttrLPM), /*UseMemorySSA=*/true));
            PostAttrFPM.addPass(llvm::InstCombinePass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(PostAttrFPM)));
        });
    }

    // Run srem→urem and sdiv→udiv conversion as an OptimizerLast
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            struct SRemToURemPass : public llvm::PassInfoMixin<SRemToURemPass> {
                llvm::PreservedAnalyses run(llvm::Module& M, llvm::ModuleAnalysisManager&) {
                    const unsigned total = runSignedToUnsignedConverge(M, 4);
                    return total > 0 ? llvm::PreservedAnalyses::none()
                                     : llvm::PreservedAnalyses::all();
                }
            };
            MPM.addPass(SRemToURemPass());
        });
    }

    // ── Loop-structure annotation + hot/cold splitting ───────────────────────
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool isO3last = optimizationLevel >= OptimizationLevel::O3;
        PB.registerOptimizerLastEPCallback(
            [isO3last](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            // Rule 4: annotate over-large loop bodies for µop-cache partitioning.
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(OpcacheLoopPartitionPass()));
            // Rule 5: promote large streaming-write loops to non-temporal stores.
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(LargeArrayStreamingStorePass()));
            // HotColdSplitting outlines cold code regions (error handlers, assertion
            MPM.addPass(llvm::HotColdSplittingPass());
            // StripDeadPrototypes removes unused external function declarations.
            MPM.addPass(llvm::StripDeadPrototypesPass());
            // GlobalSplit splits internal globals with multiple fields into
            MPM.addPass(llvm::GlobalSplitPass());
            if (isO3last) {
                // MergeFunctions deduplicates identical function bodies, reducing
                MPM.addPass(llvm::MergeFunctionsPass());
            }
            // AlwaysInlinerPass at the very end: PartialInlinerPass can create
            MPM.addPass(llvm::AlwaysInlinerPass());
        });
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::OptimizationLevel newPMLevel;
    switch (optimizationLevel) {
    case OptimizationLevel::O1:
        newPMLevel = llvm::OptimizationLevel::O1;
        break;
    case OptimizationLevel::O3:
        newPMLevel = llvm::OptimizationLevel::O3;
        break;
    default:
        newPMLevel = llvm::OptimizationLevel::O2;
        break;
    }
    llvm::ModulePassManager MPM;
    if (lto_) {
        // LTO mode: use the pre-link pipeline that runs lighter intra-procedural
        if (verbose_) {
            std::cout << "    Building LTO pre-link default pipeline..." << '\n';
        }
        MPM = PB.buildLTOPreLinkDefaultPipeline(newPMLevel);
    } else {
        if (verbose_) {
            const char* levelStr =
                (optimizationLevel == OptimizationLevel::O1) ? "O1" :
                (optimizationLevel == OptimizationLevel::O3) ? "O3" : "O2";
            std::cout << "    Building per-module default pipeline at " << levelStr << "..." << '\n';
        }
        MPM = PB.buildPerModuleDefaultPipeline(newPMLevel);
    }
    // At O1+, prepend AlwaysInlinerPass before the module pipeline so that
    if (optimizationLevel >= OptimizationLevel::O1 && !optMaxFunctions.empty()) {
        MPM.addPass(llvm::AlwaysInlinerPass());
    }

    // At O2+, append GlobalOptPass after the standard pipeline to constant-fold
    if (optimizationLevel >= OptimizationLevel::O2) {
        if (verbose_) {
            std::cout << "    Adding GlobalOpt + GlobalDCE passes..." << '\n';
        }
        MPM.addPass(llvm::GlobalOptPass());
        MPM.addPass(llvm::GlobalDCEPass());
        // Final StripDeadPrototypes cleanup: after GlobalDCE removes dead
        // functions, their forward declarations may linger.
        MPM.addPass(llvm::StripDeadPrototypesPass());
    }
    if (verbose_) {
        std::cout << "    Running LLVM module pass pipeline..." << '\n';
    }
    // Pre-pipeline srem→urem conversion: run BEFORE the LLVM pipeline so the
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        for (auto& func : *module) {
            superopt::convertSRemToURem(func);
            superopt::convertSDivToUDiv(func);
        }
    }

    // ── Pre-pipeline: CF-CTRE uniform-return IR folding ───────────────────
    if (optCtx_ && optimizationLevel >= OptimizationLevel::O2) {
        for (auto& [fnName, ctVal] : optCtx_->uniformReturnValues()) {
            if (!optCtx_->isCTPure(fnName)) continue;
            auto* F = module->getFunction(fnName);
            if (!F || F->isDeclaration() || F->use_empty()) continue;
            llvm::Type* retType = F->getReturnType();
            if (retType->isVoidTy()) continue;

            // Build the LLVM constant matching the CF-CTRE result type.
            llvm::Constant* C = nullptr;
            if (ctVal.isInt() && retType->isIntegerTy())
                C = llvm::ConstantInt::get(retType,
                        static_cast<uint64_t>(ctVal.asI64()), /*isSigned=*/true);
            else if (ctVal.isFloat() && retType->isFloatingPointTy())
                C = llvm::ConstantFP::get(retType, ctVal.asF64());

            if (!C) continue;

            // Collect call sites first (iterator-invalidation safety).
            llvm::SmallVector<llvm::CallBase*, 16> toFold;
            for (auto* user : llvm::make_early_inc_range(F->users())) {
                if (auto* CB = llvm::dyn_cast<llvm::CallBase>(user))
                    if (CB->getCalledFunction() == F)
                        toFold.push_back(CB);
            }
            for (auto* CB : toFold) {
                // Replace all uses of the call result with the constant.
                if (!CB->getType()->isVoidTy())
                    CB->replaceAllUsesWith(C);
                // Erase the call — safe because the function is pure.
                CB->eraseFromParent();
            }
        }
    }
    // Pre-pipeline HGOE loop annotation: set target-optimal unroll count,
    MPM.run(*module, MAM);
    if (verbose_) {
        std::cout << "    LLVM pass pipeline complete" << '\n';
    }

    // Strip `cold` and `minsize` attributes from user-defined functions.
    if (optimizationLevel >= OptimizationLevel::O2) {
        for (auto& F : *module) {
            if (F.isDeclaration()) continue;
            if (userAnnotatedColdFunctions_.count(F.getName())) continue; // preserve @cold
            F.removeFnAttr(llvm::Attribute::Cold);
            F.removeFnAttr(llvm::Attribute::MinSize);
        }
    }

    // Strip `nofree` from any non-declaration function that directly or
    if (optimizationLevel >= OptimizationLevel::O2) {
        for (auto& F : *module) {
            if (F.isDeclaration()) continue;
            if (!F.hasFnAttribute(llvm::Attribute::NoFree)) continue;
            bool callsFree = false;
            for (auto& BB : F) {
                for (auto& I : BB) {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!call) continue;
                    auto* callee = call->getCalledFunction();
                    if (!callee) continue;
                    // Check if callee is a free-like function (allockind "free").
                    if (callee->hasFnAttribute("allockind")) {
                        auto val = callee->getFnAttribute("allockind").getValueAsString();
                        if (val.contains("free")) { callsFree = true; break; }
                    }
                    // Also match by name: free / __libc_free / cfree etc.
                    llvm::StringRef name = callee->getName();
                    if (name == "free" || name == "cfree" || name.starts_with("__libc_free")) {
                        callsFree = true; break;
                    }
                }
                if (callsFree) break;
            }
            if (callsFree)
                F.removeFnAttr(llvm::Attribute::NoFree);
        }
    }

    // ── Shared post-pipeline analysis infrastructure ──────────────────────
    llvm::PipelineTuningOptions PTOPost;
    PTOPost.LoopVectorization = atLeastO2 && enableVectorize_;
    PTOPost.SLPVectorization  = atLeastO2 && enableVectorize_;
    PTOPost.LoopInterleaving  = atLeastO2 && enableVectorize_;
    PTOPost.LoopUnrolling     = false;
    llvm::PassBuilder PostPB(targetMachine.get(), PTOPost);
    llvm::LoopAnalysisManager  PostLAM;
    llvm::FunctionAnalysisManager PostFAM;
    llvm::CGSCCAnalysisManager PostCGAM;
    llvm::ModuleAnalysisManager PostMAM;
    PostPB.registerModuleAnalyses(PostMAM);
    PostPB.registerCGSCCAnalyses(PostCGAM);
    PostPB.registerFunctionAnalyses(PostFAM);
    PostPB.registerLoopAnalyses(PostLAM);
    PostPB.crossRegisterProxies(PostLAM, PostFAM, PostCGAM, PostMAM);

    // Helper: run an FPM on a specific subset of functions using PostMAM.
    auto runPostFPMOnFuncs = [&](llvm::FunctionPassManager FPM,
                                 std::vector<llvm::Function*> funcs) {
        if (funcs.empty()) return;
        struct FilteredFuncPass : public llvm::PassInfoMixin<FilteredFuncPass> {
            llvm::FunctionPassManager FPM;
            std::vector<llvm::Function*> funcs;
            FilteredFuncPass(llvm::FunctionPassManager fpm,
                             std::vector<llvm::Function*> f)
                : FPM(std::move(fpm)), funcs(std::move(f)) {}
            llvm::PreservedAnalyses run(llvm::Module& M,
                                        llvm::ModuleAnalysisManager& MAM) {
                auto& FAM =
                    MAM.getResult<llvm::FunctionAnalysisManagerModuleProxy>(M)
                       .getManager();
                bool changed = false;
                for (auto* F : funcs) {
                    if (F && !F->isDeclaration()) {
                        auto PA = FPM.run(*F, FAM);
                        changed |= !PA.areAllPreserved();
                    }
                }
                return changed ? llvm::PreservedAnalyses::none()
                               : llvm::PreservedAnalyses::all();
            }
        };
        llvm::ModulePassManager MPMPost;
        MPMPost.addPass(FilteredFuncPass(std::move(FPM), std::move(funcs)));
        MPMPost.run(*module, PostMAM);
    };

    // Helper: run an FPM on ALL non-declaration functions using PostMAM.
    auto runPostFPMOnAll = [&](llvm::FunctionPassManager FPM) {
        llvm::ModulePassManager MPMPost;
        MPMPost.addPass(
            llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
        MPMPost.run(*module, PostMAM);
    };

    // Bounded recursive inlining: replicate GCC's deep recursive inlining
    if (optimizationLevel >= OptimizationLevel::O3) {
        static constexpr unsigned kRecursiveInlineDepth = 6;
        // Conservative size limit: the function BEFORE inlining must be
        static constexpr unsigned kMaxPreInlineSize = 350;
        llvm::SmallVector<llvm::Function*, 4> inlinedFuncs;
        for (auto& F : *module) {
            if (F.isDeclaration() || F.getName() == "main") continue;
            const unsigned preSize = F.getInstructionCount();
            if (preSize > kMaxPreInlineSize) continue;

            // Only inline functions that still contain a self-call.
            bool hasSelfCall = false;
            for (auto& BB : F) {
                for (auto& I : BB) {
                    if (auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
                        if (CB->getCalledFunction() == &F) {
                            hasSelfCall = true;
                            break;
                        }
                    }
                }
                if (hasSelfCall) break;
            }
            if (!hasSelfCall) continue;

            for (unsigned level = 0; level < kRecursiveInlineDepth; ++level) {
                llvm::SmallVector<llvm::CallBase*, 4> selfCalls;
                for (auto& BB : F) {
                    for (auto& I : BB) {
                        if (auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
                            if (CB->getCalledFunction() == &F) {
                                selfCalls.push_back(CB);
                            }
                        }
                    }
                }
                if (selfCalls.empty()) break;

                // Only inline the first self-call per level to avoid
                unsigned inlined = 0;
                for (auto* CB : selfCalls) {
                    if (F.getInstructionCount() > preSize * 8) break;
                    llvm::InlineFunctionInfo IFI;
                    auto result = llvm::InlineFunction(*CB, IFI);
                    if (result.isSuccess()) ++inlined;
                }
                if (inlined == 0) break;
            }
            // Only functions with successful inlining need cleanup
            inlinedFuncs.push_back(&F);
        }
        if (verbose_) {
            std::cout << "    Bounded recursive inlining complete" << '\n';
        }
        // Post-recursive-inlining cleanup (new-PM).
        if (!inlinedFuncs.empty()) {
            llvm::FunctionPassManager PostInlineFPM;
            addCanonicalCleanup(PostInlineFPM);
            runPostFPMOnFuncs(std::move(PostInlineFPM),
                              {inlinedFuncs.begin(), inlinedFuncs.end()});
        }
    }

    // ── Function specialization for constant arguments ──────────────────
    if (optimizationLevel >= OptimizationLevel::O3) {
        unsigned totalSpecialized = 0;
        constexpr unsigned kMaxSpecsPerFunc = 8;
        constexpr unsigned kMaxFuncSizeForSpec = 400;

        // Collect call sites with constant arguments
        struct SpecCandidate {
            llvm::CallBase* callSite;
            llvm::Function* callee;
            llvm::SmallVector<unsigned, 4> constArgIndices;
        };
        std::vector<SpecCandidate> candidates;

        // Build a set of (caller, callee) pairs explicitly requested via
        std::unordered_map<std::string, std::unordered_set<std::string>> specFilter;
        for (auto& [fnName, cfg] : optMaxFunctionConfigs_) {
            if (!cfg.specialize.empty()) {
                specFilter[fnName] = {cfg.specialize.begin(), cfg.specialize.end()};
            }
        }

        for (auto& F : *module) {
            if (F.isDeclaration()) continue;
            const std::string callerName = F.getName().str();
            auto filterIt = specFilter.find(callerName);
            for (auto& BB : F) {
                for (auto& I : BB) {
                    auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!CB) continue;
                    auto* callee = CB->getCalledFunction();
                    if (!callee || callee->isDeclaration()) continue;
                    if (callee->hasExactDefinition() == false) continue;
                    if (callee->getInstructionCount() > kMaxFuncSizeForSpec) continue;
                    if (callee == &F) continue;  // skip self-recursion
                    // If this caller has a specialize list, only target listed callees.
                    if (filterIt != specFilter.end()) {
                        if (!filterIt->second.count(callee->getName().str())) continue;
                    }
                    llvm::SmallVector<unsigned, 4> constArgs;
                    for (unsigned i = 0; i < CB->arg_size(); ++i) {
                        if (llvm::isa<llvm::ConstantInt>(CB->getArgOperand(i)) ||
                            llvm::isa<llvm::ConstantFP>(CB->getArgOperand(i))) {
                            constArgs.push_back(i);
                        }
                    }
                    if (!constArgs.empty()) {
                        candidates.push_back({CB, callee, constArgs});
                    }
                }
            }
        }

        // Group candidates by callee and specialize
        std::unordered_map<llvm::Function*, unsigned> specCount;
        for (auto& cand : candidates) {
            if (specCount[cand.callee] >= kMaxSpecsPerFunc) continue;

            // Create the specialized clone
            llvm::ValueToValueMapTy VMap;
            auto* clone = llvm::CloneFunction(cand.callee, VMap);
            if (!clone) continue;

            // Set constant arguments in the clone
            for (unsigned argIdx : cand.constArgIndices) {
                auto argIt = clone->arg_begin();
                std::advance(argIt, argIdx);
                llvm::Argument& cloneArg = *argIt;
                llvm::Value* constVal = cand.callSite->getArgOperand(argIdx);
                cloneArg.replaceAllUsesWith(constVal);
            }

            // Give the clone a unique name and internal linkage
            clone->setName(cand.callee->getName() + ".spec." +
                          std::to_string(specCount[cand.callee]));
            clone->setLinkage(llvm::GlobalValue::InternalLinkage);

            // Redirect the call site to use the specialized version
            cand.callSite->setCalledFunction(clone);

            specCount[cand.callee]++;
            totalSpecialized++;
        }

        if (verbose_ && totalSpecialized > 0) {
            std::cout << "    Function specialization: " << totalSpecialized
                      << " call sites specialized" << '\n';
        }

        // Run a new-PM cleanup pass on specialized functions.
        if (totalSpecialized > 0) {
            std::vector<llvm::Function*> specFuncList;
            for (auto& func : *module) {
                if (!func.isDeclaration() && func.getName().contains(".spec."))
                    specFuncList.push_back(&func);
            }
            if (!specFuncList.empty()) {
                llvm::FunctionPassManager SpecFPM;
                SpecFPM.addPass(llvm::InstSimplifyPass());
                SpecFPM.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
                SpecFPM.addPass(llvm::PromotePass());
                addCanonicalCleanup(SpecFPM);
                runPostFPMOnFuncs(std::move(SpecFPM), std::move(specFuncList));
            }
        }
    }

    // Constant argument propagation: for internal functions where ALL call sites
    if (optimizationLevel >= OptimizationLevel::O2) {
        llvm::ModulePassManager ConstPropMPM;
        ConstPropMPM.addPass(ConstArgPropagationPass());
        // Follow up with function-level cleanup to fold the newly-constant args.
        llvm::FunctionPassManager ConstPropFPM;
        ConstPropFPM.addPass(llvm::SCCPPass());
        ConstPropFPM.addPass(llvm::InstCombinePass());
        ConstPropFPM.addPass(llvm::ADCEPass());
        ConstPropFPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
        ConstPropMPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(ConstPropFPM)));
        ConstPropMPM.run(*module, PostMAM);
    }

    // Superoptimizer: run after the standard LLVM pipeline to catch patterns
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        if (verbose_) {
            std::cout << "    Running superoptimizer (level " << superoptLevel_
                      << ": idiom recognition, algebraic simplification"
                      << (superoptLevel_ >= 2 ? ", synthesis, branch-opt" : "")
                      << ")..." << '\n';
        }
        superopt::SuperoptimizerConfig superConfig;
        // Configure based on superopt level:
        if (superoptLevel_ <= 1) {
            superConfig.enableSynthesis = false;
            superConfig.enableBranchOpt = false;
        } else if (superoptLevel_ >= 3 || optimizationLevel >= OptimizationLevel::O3) {
            superConfig.synthesis.maxInstructions = 5;
            superConfig.synthesis.costThreshold = 0.9;
        }
        // Unified cost model: when a hardware profile is available, install a
        if (hgoe::shouldActivate(hgoe::HGOEConfig{marchCpu_, mtuneCpu_})) {
            std::string resolvedCpu = marchCpu_.empty() ? mtuneCpu_ : marchCpu_;
            if (resolvedCpu == "native")
                resolvedCpu = llvm::sys::getHostCPUName().str();
            auto profileOpt = hgoe::lookupMicroarch(resolvedCpu);
            if (profileOpt) {
                // Capture profile by value — no lifetime dependency on any graph.
                hgoe::MicroarchProfile capturedProfile = *profileOpt;
                // Install into the OptimizationManager as the shared cost oracle.
                optMgr.setCostModel(std::make_unique<HGOECostModel>(capturedProfile));
                // Wire the shared model into the superoptimizer config.
                superConfig.costModel = optMgr.costModel();
            }
        }
        auto superStats = superopt::superoptimizeModule(*module, superConfig);
        const unsigned totalSuperOpts = superStats.idiomsReplaced + superStats.synthReplacements +
                                 superStats.algebraicSimplified + superStats.branchesSimplified +
                                 superStats.deadCodeEliminated;
        if (verbose_) {
            std::cout << "    Superoptimizer complete: "
                      << superStats.idiomsReplaced << " idioms replaced, "
                      << superStats.algebraicSimplified << " algebraic simplifications, "
                      << superStats.synthReplacements << " synthesis replacements, "
                      << superStats.branchesSimplified << " branches simplified, "
                      << superStats.deadCodeEliminated << " dead instructions eliminated"
                      << " (" << totalSuperOpts << " total optimizations)" << '\n';
        }

        // Post-superoptimizer cleanup (new-PM, functions ≤2000 instructions).
        if (totalSuperOpts > 0) {
            // Helper: collect all non-declaration functions up to a given size.
            auto collectSmallFuncs = [&](unsigned maxInstrs) {
                std::vector<llvm::Function*> out;
                for (auto& func : *module)
                    if (!func.isDeclaration() && func.getInstructionCount() <= maxInstrs)
                        out.push_back(&func);
                return out;
            };

            auto smallFuncs = collectSmallFuncs(2000);
            if (!smallFuncs.empty()) {
                llvm::FunctionPassManager PSuperFPM;
                addCanonicalCleanup(PSuperFPM, /*withEarlyCSE=*/false, /*withMemCpy=*/true);
                runPostFPMOnFuncs(std::move(PSuperFPM), std::move(smallFuncs));
            }

            // At O3, run a second superoptimizer pass after the cleanup.
            if (optimizationLevel >= OptimizationLevel::O3) {
                superopt::SuperoptimizerConfig superConfig2 = superConfig;
                superConfig2.enableSynthesis = false;  // skip expensive synthesis on pass 2
                auto stats2 = superopt::superoptimizeModule(*module, superConfig2);
                const unsigned total2 = stats2.idiomsReplaced + stats2.algebraicSimplified +
                                        stats2.branchesSimplified + stats2.deadCodeEliminated;
                if (verbose_ && total2 > 0) {
                    std::cout << "    Superoptimizer pass 2: "
                              << total2 << " additional optimizations" << '\n';
                }

                // Lightweight cleanup after pass 2.  Without this, pass-2
                if (total2 > 0) {
                    auto smallFuncs2 = collectSmallFuncs(2000);
                    if (!smallFuncs2.empty()) {
                        llvm::FunctionPassManager PSuperFPM2;
                        PSuperFPM2.addPass(llvm::SCCPPass());
                        PSuperFPM2.addPass(llvm::InstCombinePass());
                        PSuperFPM2.addPass(llvm::ADCEPass());
                        PSuperFPM2.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
                        runPostFPMOnFuncs(std::move(PSuperFPM2), std::move(smallFuncs2));
                    }
                }
            }
        }
    } else if (verbose_ && optimizationLevel >= OptimizationLevel::O2 && !enableSuperopt_) {
        std::cout << "    Superoptimizer disabled (-fno-superopt)" << '\n';
    }

    // Post-pipeline srem→urem and sdiv→udiv conversion.  The pre-pipeline
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        // Each round first infers nuw flags (exposing more conversion targets),
        const unsigned postConvCount = runSignedToUnsignedConverge(*module, 3);
        if (verbose_ && postConvCount > 0) {
            std::cout << "    Post-pipeline signed→unsigned: "
                      << postConvCount << " conversions" << '\n';
        }
    }

    // Hardware Graph Optimization Engine: run after the superoptimizer when
    if (enableHGOE_ && optimizationLevel >= OptimizationLevel::O2) {
        if (verbose_) {
            std::cout << "    Running Hardware Graph Optimization Engine";
            if (!marchCpu_.empty()) std::cout << " (march=" << marchCpu_ << ")";
            if (!mtuneCpu_.empty()) std::cout << " (mtune=" << mtuneCpu_ << ")";
            std::cout << "..." << '\n';
        }
        hgoe::HGOEConfig hgoeConfig;
        hgoeConfig.marchCpu = marchCpu_;
        hgoeConfig.mtuneCpu = mtuneCpu_;
        // Disable loop annotation when LTO is active — the LTO linker runs
        hgoeConfig.enableLoopAnnotation = !lto_;
        auto hgoeStats = hgoe::optimizeModule(*module, hgoeConfig);
        if (verbose_) {
            if (hgoeStats.activated) {
                std::cout << "    HGOE complete: arch=" << hgoeStats.resolvedArch
                          << ", " << hgoeStats.functionsOptimized << " functions optimized"
                          << '\n';
            } else {
                std::cout << "    HGOE not activated (no explicit -march/-mtune)" << '\n';
            }
        }
    }

    // Post-HGOE srem→urem and sdiv→udiv conversion.
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2
        && enableHGOE_ && !marchCpu_.empty()) {
        const unsigned postHGOECount = runSignedToUnsignedConverge(*module, 2);
        if (verbose_ && postHGOECount > 0) {
            std::cout << "    Post-HGOE signed→unsigned: "
                      << postHGOECount << " conversions" << '\n';
        }
    }

    // ── Speculative Devectorization & Revectorization (SDR) ────────────────
    if (enableSDR_ && optimizationLevel >= OptimizationLevel::O2) {
        if (verbose_) std::cout << "    Running SDR pass..." << '\n';
        sdr::SdrConfig sdrCfg;
        // At O3 / optmax, allow more aggressive widening.
        if (optimizationLevel >= OptimizationLevel::O3) {
            sdrCfg.maxWidenLanes    = 16;
            sdrCfg.costThreshold    = 0.85;
            sdrCfg.partialUseFraction = 0.75;
        }
        auto sdrStats = sdr::runSDR(*module,
            [&](llvm::Function& F) -> llvm::TargetTransformInfo {
                return targetMachine ? targetMachine->getTargetTransformInfo(F)
                                     : llvm::TargetTransformInfo(F.getParent()->getDataLayout());
            },
            sdrCfg);
        if (verbose_) {
            std::cout << "    SDR complete: "
                      << sdrStats.regionsDetected << " regions detected, "
                      << sdrStats.narrowed        << " narrowed, "
                      << sdrStats.widened         << " widened, "
                      << sdrStats.reductionsReplaced << " reductions replaced"
                      << " (" << sdrStats.totalTransformed() << " total)"
                      << '\n';
        }
    } else if (verbose_ && optimizationLevel >= OptimizationLevel::O2 && !enableSDR_) {
        std::cout << "    SDR disabled (-fno-sdr)" << '\n';
    }

    // ── Implicit Phase Ordering Fixer (IPOF) ────────────────────────────────
    if (enableIPOF_ && optimizationLevel >= OptimizationLevel::O2) {
        // Derive aggression level from optimization level unless explicitly set.
        const unsigned level = (ipofLevel_ > 0) ? ipofLevel_
            : (optimizationLevel >= OptimizationLevel::O3 ? 2u : 1u);

        if (verbose_) {
            std::cout << "    Running IPOF (level " << level << ")..." << '\n';
        }
        ipof::IpofConfig ipofCfg;
        ipofCfg.aggressionLevel = level;
        ipofCfg.maxIterations   = (level >= 3) ? 3u : (level == 2 ? 2u : 1u);
        ipofCfg.enableNearVectorizable = (level >= 3);
        ipofCfg.enableCallWithConst    = (level >= 2);

        auto ipofStats = ipof::runIPOF(*module, ipofCfg);

        if (verbose_) {
            std::cout << "    IPOF complete: "
                      << ipofStats.opportunitiesFound << " opportunities found, "
                      << ipofStats.acceptedImprovements << " accepted"
                      << " (constants=" << ipofStats.foldedConstants
                      << " cse="        << ipofStats.eliminatedCSE
                      << " dead="       << ipofStats.eliminatedDead
                      << " loads="      << ipofStats.eliminatedLoads
                      << " inline="     << ipofStats.inlinedAndFolded
                      << ") net -"      << ipofStats.netInstrReduction
                      << " instrs" << '\n';
        }
    } else if (verbose_ && optimizationLevel >= OptimizationLevel::O2 && !enableIPOF_) {
        std::cout << "    IPOF disabled (-fno-ipof)" << '\n';
    }

    // -----------------------------------------------------------------------
    if (optimizationLevel >= OptimizationLevel::O2) {
        unsigned prefetchesRemoved = 0;
        for (auto& F : *module) {
            if (F.isDeclaration()) continue;
            llvm::SmallVector<llvm::CallInst*, 8> toRemove;
            for (auto& BB : F) {
                for (auto& I : BB) {
                    auto* call = llvm::dyn_cast<llvm::CallInst>(&I);
                    if (!call) continue;
                    auto* callee = call->getCalledFunction();
                    if (!callee || callee->getIntrinsicID() != llvm::Intrinsic::prefetch)
                        continue;

                    llvm::Value* ptr = call->getArgOperand(0);
                    bool shouldRemove = false;

                    // Rule 1: Target is a constant or inttoptr of a constant.
                    if (llvm::isa<llvm::Constant>(ptr)) {
                        shouldRemove = true;
                    }

                    // Rule 1b: inttoptr of a constant integer.
                    if (!shouldRemove) {
                        if (auto* i2p = llvm::dyn_cast<llvm::IntToPtrInst>(ptr)) {
                            if (llvm::isa<llvm::ConstantInt>(i2p->getOperand(0))) {
                                shouldRemove = true;
                            }
                        }
                        // Also check ConstantExpr inttoptr
                        if (auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(ptr)) {
                            if (ce->getOpcode() == llvm::Instruction::IntToPtr) {
                                shouldRemove = true;
                            }
                        }
                    }

                    // Rule 2: Target is a stack allocation (alloca).
                    if (!shouldRemove) {
                        llvm::Value* underlying = ptr->stripPointerCasts();
                        if (llvm::isa<llvm::AllocaInst>(underlying)) {
                            if (!call->getMetadata("omscript.memory_prefetch")) {
                                shouldRemove = true;
                            }
                        }
                    }

                    // Rule 3: No remaining memory-accessing users of the
                    // prefetch target (the prefetch itself is not useful).
                    if (!shouldRemove) {
                        llvm::Value* underlying = ptr->stripPointerCasts();
                        bool hasMemoryUser = false;
                        for (auto* user : underlying->users()) {
                            if (user == call) continue; // skip the prefetch itself
                            if (llvm::isa<llvm::LoadInst>(user) ||
                                llvm::isa<llvm::StoreInst>(user)) {
                                hasMemoryUser = true;
                                break;
                            }
                            // GEPs with load/store users also count
                            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
                                for (auto* gepUser : gep->users()) {
                                    if (llvm::isa<llvm::LoadInst>(gepUser) ||
                                        llvm::isa<llvm::StoreInst>(gepUser)) {
                                        hasMemoryUser = true;
                                        break;
                                    }
                                }
                            }
                            if (hasMemoryUser) break;
                        }
                        if (!hasMemoryUser) {
                            shouldRemove = true;
                        }
                    }

                    // Rule 5: Prefetch inside a function with no memory side
                    // effects (readnone/pure) is meaningless.
                    if (!shouldRemove) {
                        if (F.doesNotAccessMemory()) {
                            shouldRemove = true;
                        }
                    }

                    if (shouldRemove) {
                        toRemove.push_back(call);
                    }
                }
            }
            for (auto* call : toRemove) {
                call->eraseFromParent();
                ++prefetchesRemoved;
            }
        }
        if (verbose_ && prefetchesRemoved > 0) {
            std::cout << "    Post-optimization prefetch cleanup: "
                      << prefetchesRemoved << " redundant prefetch(es) removed"
                      << '\n';
        }

        // ── Unified new-PM closing pipeline ──────────────────────────────────
        if (optimizationLevel >= OptimizationLevel::O2) {
            llvm::FunctionPassManager CloseFPM;
            // Phase 0: Advanced branch weight annotation.
            CloseFPM.addPass(AdvancedBranchWeightAnnotationPass());
            // Phase 1: constant propagation + value-range refinement.
            CloseFPM.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
            CloseFPM.addPass(llvm::SCCPPass());
            CloseFPM.addPass(llvm::CorrelatedValuePropagationPass());
            CloseFPM.addPass(llvm::BDCEPass());
            // Phase 2: value numbering + memory cleanup.
            CloseFPM.addPass(llvm::NewGVNPass());
            CloseFPM.addPass(llvm::DSEPass());
            CloseFPM.addPass(llvm::MemCpyOptPass());
            // Store widening: merge consecutive narrow constant stores into
            CloseFPM.addPass(StoreWideningPass());
            // MAPR: hoist independent loads above non-aliasing preceding stores
            // to maximise memory-level parallelism in the closing pipeline.
            CloseFPM.addPass(MemoryAccessPhaseReorderPass());
            CloseFPM.addPass(llvm::InstCombinePass());
            // Phase 3: loop normalization + invariant hoisting.
            addCanonicalLoopBarrier(CloseFPM);
            {
                llvm::LoopPassManager LPMClose;
                LPMClose.addPass(llvm::IndVarSimplifyPass());
                LPMClose.addPass(llvm::LICMPass(llvm::LICMOptions()));
                CloseFPM.addPass(llvm::createFunctionToLoopPassAdaptor(
                    std::move(LPMClose), /*UseMemorySSA=*/true));
            }
            // Cross-iteration load reuse: detect stencil/sliding-window patterns
            CloseFPM.addPass(CrossIterLoadReusePass());
            // MAC pattern detection: convert fadd(fmul(a,b), acc) → fmuladd
            CloseFPM.addPass(MACPatternPass());
            CloseFPM.addPass(llvm::IRCEPass());
            // Range bounds-check hoist: convert repeated per-iteration bounds
            CloseFPM.addPass(RangeBoundsCheckHoistPass());
            CloseFPM.addPass(llvm::ConstraintEliminationPass());
            // Phase 3b: late loop unrolling.  After superoptimizer + HGOE,
            CloseFPM.addPass(llvm::LoopUnrollPass(llvm::LoopUnrollOptions()));
            // Post-unroll redundancy elimination.  Loop unrolling duplicates
            CloseFPM.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
            // Phase 4: strength reduction + final peephole.
            CloseFPM.addPass(llvm::InstCombinePass());
            // RPAR: rebalance linear associative chains into O(log n) depth
            // trees to expose ILP for the OoO execution unit.
            CloseFPM.addPass(TreeHeightReductionPass());
            CloseFPM.addPass(llvm::AggressiveInstCombinePass());
            // Phase 5: second vectorization pass (after srem→urem + superopt).
            if (enableVectorize_) {
                CloseFPM.addPass(llvm::SLPVectorizerPass());
                CloseFPM.addPass(llvm::VectorCombinePass());
            }
            // Final peephole + dead-code removal.
            CloseFPM.addPass(llvm::InstCombinePass());
            CloseFPM.addPass(llvm::ADCEPass());
            // Conditional store sinking: move stores from BB to the cold successor
            CloseFPM.addPass(ConditionalStoreSinkPass());
            // BER: convert near-50/50 small diamond CFGs to select instructions,
            // eliminating branch-mispredict flushes before the final CFG cleanup.
            CloseFPM.addPass(BranchEntropyReductionPass());
            CloseFPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
            runPostFPMOnAll(std::move(CloseFPM));
            if (verbose_) {
                std::cout << "    Unified closing new-PM pipeline complete" << '\n';
            }
        }

        // Post-final-cleanup modulo strength reduction: expand urem-by-constant
        if (enableSuperopt_) {
            unsigned moduloExpanded = 0;
            for (auto& func : *module) {
                if (!func.isDeclaration())
                    moduloExpanded += superopt::constantModuloStrengthReduce(func);
            }
            if (verbose_ && moduloExpanded > 0) {
                std::cout << "    Post-pipeline modulo strength reduction: "
                          << moduloExpanded << " urem instructions expanded" << '\n';
            }
        }
    }
}

void CodeGenerator::optimizeFunction(llvm::Function* func) {
    // Per-function optimization using the new pass manager.
    if (!func || func->isDeclaration()) return;
    auto tm = createTargetMachine();
    llvm::PassBuilder PBFunc(tm.get());
    llvm::LoopAnalysisManager  LAMFunc;
    llvm::FunctionAnalysisManager FAMFunc;
    llvm::CGSCCAnalysisManager CGAMFunc;
    llvm::ModuleAnalysisManager MAMFunc;
    PBFunc.registerModuleAnalyses(MAMFunc);
    PBFunc.registerCGSCCAnalyses(CGAMFunc);
    PBFunc.registerFunctionAnalyses(FAMFunc);
    PBFunc.registerLoopAnalyses(LAMFunc);
    PBFunc.crossRegisterProxies(LAMFunc, FAMFunc, CGAMFunc, MAMFunc);

    // Build the per-function pipeline.
    llvm::FunctionPassManager FPMFunc;
    FPMFunc.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    FPMFunc.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
    FPMFunc.addPass(llvm::PromotePass());
    FPMFunc.addPass(llvm::InstCombinePass());
    FPMFunc.addPass(llvm::ReassociatePass());
    FPMFunc.addPass(llvm::NewGVNPass());
    FPMFunc.addPass(llvm::DSEPass());
    FPMFunc.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
    FPMFunc.addPass(llvm::ADCEPass());

    // Run via module-level adapter so TargetIRAnalysis and library-info
    // analyses are properly initialized through the ModuleAnalysisManager proxy.
    struct SingleFuncModulePass
        : public llvm::PassInfoMixin<SingleFuncModulePass> {
        llvm::FunctionPassManager FPM;
        llvm::Function* Target;
        SingleFuncModulePass(llvm::FunctionPassManager fpm, llvm::Function* F)
            : FPM(std::move(fpm)), Target(F) {}
        llvm::PreservedAnalyses run(llvm::Module& M,
                                    llvm::ModuleAnalysisManager& MAM) {
            auto& FAM =
                MAM.getResult<llvm::FunctionAnalysisManagerModuleProxy>(M)
                   .getManager();
            FPM.run(*Target, FAM);
            return llvm::PreservedAnalyses::none();
        }
    };
    llvm::ModulePassManager MPMFunc;
    MPMFunc.addPass(SingleFuncModulePass(std::move(FPMFunc), func));
    MPMFunc.run(*module, MAMFunc);
}


void CodeGenerator::optimizeOptMaxFunctions() {
    // Phase 0: Apply aggressive function attributes to OPTMAX functions.
    static constexpr unsigned kAlwaysInlineThreshold = 500; // instruction count
    llvm::SmallVector<llvm::Function*, 16> optMaxFuncs;
    for (auto& func : module->functions()) {
        if (func.isDeclaration())
            continue;
        const llvm::StringRef name = func.getName();
        if (!optMaxFunctions.count(name))
            continue;
        func.addFnAttr(llvm::Attribute::NoUnwind);
        // WillReturn: OPTMAX functions always return (no infinite loops or
        // exceptions), enabling LLVM to speculate calls and eliminate dead ones.
        func.addFnAttr(llvm::Attribute::WillReturn);
        // NoSync: OPTMAX functions don't synchronize (no atomics, locks, or
        // thread-related operations), enabling aggressive reordering.
        func.addFnAttr(llvm::Attribute::NoSync);
        // NoFree: OPTMAX functions don't free heap memory, enabling the
        // optimizer to sink/hoist loads past calls to these functions.
        func.addFnAttr(llvm::Attribute::NoFree);
        // Mark small OPTMAX helpers as always-inline candidates.
        if (func.getInstructionCount() < kAlwaysInlineThreshold
                && !func.hasFnAttribute(llvm::Attribute::NoInline)) {
            func.addFnAttr(llvm::Attribute::AlwaysInline);
        }

        // aggressiveVec=true: hint to the back-end to use the widest available
        const std::string nameStr = name.str();
        auto cfgIt = optMaxFunctionConfigs_.find(nameStr);
        if (cfgIt != optMaxFunctionConfigs_.end() && cfgIt->second.aggressiveVec) {
            func.addFnAttr("prefer-vector-width", "512");
            // min-legal-vector-width=0 lets the vectorizer choose width freely.
            func.addFnAttr("min-legal-vector-width", "0");
        }

        // fast_math=true: sweep all existing FP instructions and apply full
        if (cfgIt != optMaxFunctionConfigs_.end() && cfgIt->second.fastMath) {
            llvm::FastMathFlags FMF;
            FMF.setFast();
            for (auto& BB : func) {
                for (auto& I : BB) {
                    if (auto* fpOp = llvm::dyn_cast<llvm::FPMathOperator>(&I)) {
                        if (auto* fpInst = llvm::dyn_cast<llvm::Instruction>(fpOp)) {
                            fpInst->setFastMathFlags(FMF);
                        }
                    }
                }
            }
            // Also set function-level attributes for LTO and backend.
            func.addFnAttr("unsafe-fp-math", "true");
            func.addFnAttr("no-nans-fp-math", "true");
            func.addFnAttr("no-infs-fp-math", "true");
            func.addFnAttr("no-signed-zeros-fp-math", "true");
            func.addFnAttr("approx-func-fp-math", "true");
            func.addFnAttr("no-trapping-math", "true");
        }

        // memory.noalias=true: annotate all pointer-type parameters as noalias
        if (cfgIt != optMaxFunctionConfigs_.end() && cfgIt->second.memory.noalias) {
            unsigned argIdx = 0;
            for (auto& arg : func.args()) {
                if (arg.getType()->isPointerTy()) {
                    func.addParamAttr(argIdx, llvm::Attribute::NoAlias);
                    func.addParamAttr(argIdx, llvm::Attribute::NoCapture);
                    func.addParamAttr(argIdx, llvm::Attribute::NonNull);
                }
                ++argIdx;
            }
        }

        optMaxFuncs.push_back(&func);
    }

    if (optMaxFuncs.empty()) return;

    // ── Unified new-PM OPTMAX Pipeline ────────────────────────────────────────
    auto tm = createTargetMachine();
    if (!tm) {
        if (verbose_)
            llvm::errs() << "omsc: warning: could not create TargetMachine for OPTMAX pipeline\n";
        return;
    }

    llvm::PipelineTuningOptions PTOMax;
    // Enable vectorization cost-model infrastructure in the PassBuilder so that
    // LoopVectorizePass and SLPVectorizerPass inside the FPM get accurate TTI.
    PTOMax.LoopVectorization = true;
    PTOMax.SLPVectorization  = true;
    PTOMax.LoopInterleaving  = true;
    // Unrolling is handled explicitly in Phase 2.5 to control the threshold.
    PTOMax.LoopUnrolling     = false;

    llvm::PassBuilder PBMax(tm.get(), PTOMax);
    llvm::LoopAnalysisManager  LAMMax;
    llvm::FunctionAnalysisManager FAMMax;
    llvm::CGSCCAnalysisManager CGAMMax;
    llvm::ModuleAnalysisManager MAMMax;
    PBMax.registerModuleAnalyses(MAMMax);
    PBMax.registerCGSCCAnalyses(CGAMMax);
    PBMax.registerFunctionAnalyses(FAMMax);
    PBMax.registerLoopAnalyses(LAMMax);
    PBMax.crossRegisterProxies(LAMMax, FAMMax, CGAMMax, MAMMax);

    // ── Phase 1: Memory promotion and early canonicalization ──────────────
    llvm::FunctionPassManager FPMMax;
    FPMMax.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    FPMMax.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
    FPMMax.addPass(llvm::PromotePass());
    FPMMax.addPass(llvm::InstCombinePass());
    FPMMax.addPass(llvm::AggressiveInstCombinePass());
    FPMMax.addPass(llvm::ReassociatePass());
    FPMMax.addPass(llvm::NewGVNPass());
    FPMMax.addPass(llvm::SCCPPass());
    FPMMax.addPass(llvm::CorrelatedValuePropagationPass());
    FPMMax.addPass(llvm::BDCEPass());
    FPMMax.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
    FPMMax.addPass(llvm::ADCEPass());
    // CallSiteSplitting: clone call sites at branch points so that the two
    // clones receive different constant arguments, enabling SCCP / InstCombine
    // to fold each clone individually (e.g. f(cond ? 1 : 0) → two specialised
    // direct calls that are then individually constant-folded).
    FPMMax.addPass(llvm::CallSiteSplittingPass());
    // InstSimplify: a faster and more conservatively correct complement to
    // InstCombine that fires in more contexts (e.g. after attribute deduction
    // from the PreIPO).  Run after CallSiteSplitting so the split clones benefit.
    FPMMax.addPass(llvm::InstSimplifyPass());

    // ── Phase 2: Loop canonicalization and invariant hoisting ─────────────
    addCanonicalLoopBarrier(FPMMax);
    {
        llvm::LoopPassManager LPM;
        LPM.addPass(llvm::LoopRotatePass());
        LPM.addPass(llvm::LICMPass(llvm::LICMOptions()));
        // LoopInterchange: reorder loop nesting for better cache behaviour when
        // traversing multi-dimensional arrays (e.g., row-major vs column-major).
        // Running before unswitch lets the reordered loops be unswitched too.
        LPM.addPass(llvm::LoopInterchangePass());
        LPM.addPass(llvm::SimpleLoopUnswitchPass(/*NonTrivial=*/true));
        LPM.addPass(llvm::LoopIdiomRecognizePass());
        LPM.addPass(llvm::IndVarSimplifyPass());
        LPM.addPass(llvm::LoopPredicationPass());
        // LoopBoundSplit: split a loop at its bounds checks to create a hot
        // checked-out-of-range path and a fast in-bounds path; the fast path
        // then becomes vectorisable because it has no conditional guard.
        LPM.addPass(llvm::LoopBoundSplitPass());
        LPM.addPass(llvm::LoopDeletionPass());
        LPM.addPass(llvm::LoopInstSimplifyPass());
        LPM.addPass(llvm::LoopSimplifyCFGPass());
        LPM.addPass(llvm::CanonicalizeFreezeInLoopsPass());
        FPMMax.addPass(llvm::createFunctionToLoopPassAdaptor(
            std::move(LPM), /*UseMemorySSA=*/true));
    }
    // Software prefetch for predictable-stride loops.
    FPMMax.addPass(llvm::LoopDataPrefetchPass());

    // ── Phase 2.5: Loop unrolling ─────────────────────────────────────────
    addCanonicalLoopBarrier(FPMMax);
    {
        llvm::LoopPassManager LPM;
        LPM.addPass(llvm::LoopFlattenPass());
        LPM.addPass(llvm::LoopUnrollAndJamPass(/*OptLevel=*/3));
        FPMMax.addPass(llvm::createFunctionToLoopPassAdaptor(std::move(LPM)));
    }
    // LoopUnrollPass is a function pass in LLVM 18, not a loop pass.
    FPMMax.addPass(llvm::LoopUnrollPass(
        llvm::LoopUnrollOptions(/*OptLevel=*/3, /*OnlyWhenForced=*/false,
                                /*ForgetSCEV=*/true)));
    // Post-unroll cleanup: NewGVN merges values made equal by unrolled IVs
    FPMMax.addPass(llvm::NewGVNPass());
    // EarlyCSE: eliminate common sub-expressions exposed by unrolling before
    // InstCombine sees them — catches memory-based redundancies that NewGVN
    // does not handle (e.g., repeated loads from the same address across the
    // duplicated iteration bodies).
    FPMMax.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
    FPMMax.addPass(llvm::InstCombinePass());
    FPMMax.addPass(llvm::CorrelatedValuePropagationPass());
    FPMMax.addPass(llvm::JumpThreadingPass());
    FPMMax.addPass(llvm::DFAJumpThreadingPass());
    FPMMax.addPass(llvm::SpeculativeExecutionPass());
    FPMMax.addPass(llvm::SimplifyCFGPass(hyperblockCFGOpts()));
    FPMMax.addPass(llvm::InstCombinePass());
    FPMMax.addPass(llvm::ADCEPass());

    // ── Phase 3: Scalar optimizations ────────────────────────────────────
    FPMMax.addPass(llvm::SinkingPass());
    FPMMax.addPass(llvm::StraightLineStrengthReducePass());
    FPMMax.addPass(llvm::NaryReassociatePass());
    FPMMax.addPass(llvm::ReassociatePass());
    FPMMax.addPass(llvm::TailCallElimPass());
    FPMMax.addPass(llvm::ConstantHoistingPass());
    FPMMax.addPass(llvm::SeparateConstOffsetFromGEPPass());
    FPMMax.addPass(llvm::PartiallyInlineLibCallsPass());
    FPMMax.addPass(llvm::LibCallsShrinkWrapPass());
    FPMMax.addPass(llvm::DSEPass());
    FPMMax.addPass(llvm::MemCpyOptPass());
    // MergedLoadStoreMotion: hoist loads from and sink stores into the post-
    // dominator when both branches of an if-then-else produce/consume the same
    // address (diamond CFG pattern).  Reduces dynamic memory bandwidth and
    // exposes GVN opportunities on the hoisted loads.
    FPMMax.addPass(llvm::MergedLoadStoreMotionPass());
    // Store widening: merge consecutive narrow stores into wide stores.
    FPMMax.addPass(StoreWideningPass());
    // MAPR: hoist independent loads above non-aliasing preceding stores to
    FPMMax.addPass(MemoryAccessPhaseReorderPass());
    FPMMax.addPass(llvm::Float2IntPass());
    FPMMax.addPass(llvm::DivRemPairsPass());
    FPMMax.addPass(llvm::InferAlignmentPass());
    FPMMax.addPass(llvm::MergeICmpsPass());
    // Cross-iteration load reuse in OPTMAX loops: carry stencil/window values
    // across iterations in registers to eliminate redundant loads.
    FPMMax.addPass(CrossIterLoadReusePass());
    // LIRH: hoist 1.0/d outside loops where the loop-invariant divisor d is
    FPMMax.addPass(LoopInvariantReciprocalHoistPass());
    // MAC pattern detection: convert fadd(fmul(a,b), acc) → fmuladd for FMA.
    FPMMax.addPass(MACPatternPass());
    FPMMax.addPass(llvm::IRCEPass());
    // Range bounds-check hoist: hoist repeated per-iteration bounds checks to
    FPMMax.addPass(RangeBoundsCheckHoistPass());
    FPMMax.addPass(llvm::ConstraintEliminationPass());
    FPMMax.addPass(llvm::GuardWideningPass());
    FPMMax.addPass(llvm::SCCPPass());
    FPMMax.addPass(llvm::InstCombinePass());
    // RPAR: rebalance linear associative chains into O(log n) depth balanced
    FPMMax.addPass(TreeHeightReductionPass());
    FPMMax.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
    // Advanced branch weight annotation for OPTMAX functions: emit !prof metadata
    // so that BER and ConditionalStoreSink see accurate probabilities.
    FPMMax.addPass(AdvancedBranchWeightAnnotationPass());
    // Conditional store sinking: powered by the advanced branch weights above.
    FPMMax.addPass(ConditionalStoreSinkPass());
    // BER: convert near-50/50 diamond CFGs to select instructions, removing
    // mispredict flushes from unpredictable branches in hot OPTMAX loops.
    FPMMax.addPass(BranchEntropyReductionPass());
    FPMMax.addPass(llvm::ADCEPass());

    // ── Phase 4: Vectorization ────────────────────────────────────────────
    addCanonicalLoopBarrier(FPMMax);
    FPMMax.addPass(llvm::AlignmentFromAssumptionsPass());
    {
        llvm::LoopPassManager LPM;
        // Re-rotate loops before LICM.  Phase 2.5 (LoopUnroll/UnrollAndJam)
        LPM.addPass(llvm::LoopRotatePass());
        LPM.addPass(llvm::IndVarSimplifyPass());
        // LoopInterchange (second run): after unrolling and scalar optimizations,
        // the vectorizer cost model may be able to make a better nesting decision
        // than it could in Phase 2.  A second interchange pass here ensures the
        // innermost loop has the smallest stride before LoopVectorize fires.
        LPM.addPass(llvm::LoopInterchangePass());
        LPM.addPass(llvm::LoopVersioningLICMPass());
        LPM.addPass(llvm::LICMPass(llvm::LICMOptions()));
        FPMMax.addPass(llvm::createFunctionToLoopPassAdaptor(
            std::move(LPM), /*UseMemorySSA=*/true));
    }
    FPMMax.addPass(llvm::LoopDistributePass());
    FPMMax.addPass(llvm::LoopFusePass());
    // Eliminate loads of values that were stored earlier in the same loop
    // iteration (stencil / window patterns).  This pass is in the main O3
    // pipeline's vectorizer EP callback but was missing from the OPTMAX pipeline.
    FPMMax.addPass(llvm::LoopLoadEliminationPass());
    // Sharpen value ranges one final time before the cost model runs.
    FPMMax.addPass(llvm::CorrelatedValuePropagationPass());
    FPMMax.addPass(llvm::BDCEPass());
    FPMMax.addPass(llvm::InstCombinePass());
    FPMMax.addPass(llvm::IRCEPass());
    // Loop vectorizer: the central transformation of this phase.
    FPMMax.addPass(llvm::LoopVectorizePass(
        llvm::LoopVectorizeOptions(/*InterleaveOnlyWhenForced=*/false,
                                   /*VectorizeOnlyWhenForced=*/false)));
    // SLP vectorizer: pack independent scalar operations into SIMD ops.
    FPMMax.addPass(llvm::SLPVectorizerPass());
    // VectorCombine: merge redundant extract/insert sequences introduced by the vectorizers.
    FPMMax.addPass(llvm::VectorCombinePass());
    // LoadStoreVectorizer: combine adjacent scalar loads/stores into vectors.
    FPMMax.addPass(llvm::LoadStoreVectorizerPass());

    // ── Phase 5: Post-vectorization cleanup ──────────────────────────────
    FPMMax.addPass(llvm::AggressiveInstCombinePass());
    FPMMax.addPass(llvm::InstCombinePass());
    addCanonicalLoopBarrier(FPMMax);
    {
        llvm::LoopPassManager LPM;
        LPM.addPass(llvm::LICMPass(llvm::LICMOptions()));
        FPMMax.addPass(llvm::createFunctionToLoopPassAdaptor(
            std::move(LPM), /*UseMemorySSA=*/true));
    }
    FPMMax.addPass(llvm::DSEPass());
    FPMMax.addPass(llvm::MemCpyOptPass());
    FPMMax.addPass(llvm::NewGVNPass());
    // After NewGVN merges values from across the unrolled/vectorized body,
    FPMMax.addPass(llvm::InstCombinePass());
    FPMMax.addPass(llvm::VectorCombinePass());
    FPMMax.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
    FPMMax.addPass(llvm::LoopSinkPass());
    FPMMax.addPass(llvm::ADCEPass());

    // ── Run the OPTMAX pipeline on each OPTMAX function ───────────────────

    // ── Module-level IPO pre-pass ─────────────────────────────────────────
    // Run before the per-function pipeline to materialise alwaysinline callees,
    // propagate constants across call boundaries, and sharpen function attributes
    // so the per-function passes see fully inlined bodies and better value ranges.
    {
        llvm::ModulePassManager PreIPO;
        // AlwaysInlinerPass: inline callees marked alwaysinline (set above for
        // OPTMAX helper functions smaller than kAlwaysInlineThreshold).
        PreIPO.addPass(llvm::AlwaysInlinerPass(/*InsertLifetimeIntrinsics=*/false));
        // IPSCCP: propagate constants across all call boundaries in the module
        // now that small helpers are inlined.
        PreIPO.addPass(llvm::IPSCCPPass());
        // InferFunctionAttrs: deduce NoUnwind/WillReturn/NoFree/NoSync for any
        // remaining callees so the per-function pipeline sees their effects.
        PreIPO.addPass(llvm::InferFunctionAttrsPass());
        // ConstArgPropagation: for internal functions where every call site
        // passes the same concrete constant, replace the formal argument with
        // that constant — enabling SCCP / InstCombine to fold much deeper.
        PreIPO.addPass(ConstArgPropagationPass());
        // InterproceduralNullability: annotate pointer arguments as nonnull
        // when every call site passes a provably non-null value, unlocking
        // GetElementPtr folding and bounds-check elimination.
        PreIPO.addPass(InterproceduralNullabilityPass());
        // PostOrderFunctionAttrs: infer nounwind / readonly / writeonly / pure
        // attributes in bottom-up CGSCC order so callers can reason about effects.
        PreIPO.addPass(llvm::createModuleToPostOrderCGSCCPassAdaptor(
            llvm::PostOrderFunctionAttrsPass()));
        // ReversePostOrderFunctionAttrs: propagate attribute information
        // top-down so callees inherit caller assumptions (e.g. nounwind context).
        PreIPO.addPass(llvm::ReversePostOrderFunctionAttrsPass());
        PreIPO.run(*module, MAMMax);
    }

    constexpr int optMaxIterations = 5;
    for (llvm::Function* func : optMaxFuncs) {
        unsigned prevCount = func->getInstructionCount();
        for (int i = 0; i < optMaxIterations; ++i) {
            auto PA = FPMMax.run(*func, FAMMax);
            const unsigned newCount = func->getInstructionCount();
            if (PA.areAllPreserved() || newCount == prevCount) break;
            prevCount = newCount;
        }

        // report=true: print a compact per-function optimization summary to
        // stdout so the programmer can see the result of the OPTMAX pipeline.
        const std::string fname = func->getName().str();
        auto cfgIt2 = optMaxFunctionConfigs_.find(fname);
        if (cfgIt2 != optMaxFunctionConfigs_.end() && cfgIt2->second.report) {
            const OptMaxConfig& cfg = cfgIt2->second;
            std::cout << "[optmax report] " << fname << ": "
                      << func->getInstructionCount() << " instructions after optimization";
            if (cfg.fastMath)             std::cout << ", fast_math";
            if (cfg.aggressiveVec)        std::cout << ", aggressive_vec";
            if (cfg.safety == SafetyLevel::Off)     std::cout << ", safety=off";
            else if (cfg.safety == SafetyLevel::Relaxed) std::cout << ", safety=relaxed";
            if (cfg.memory.noalias)       std::cout << ", memory.noalias";
            if (cfg.memory.prefetch)      std::cout << ", memory.prefetch";
            if (!cfg.assumes.empty()) {
                std::cout << ", assumes=[";
                for (size_t i = 0; i < cfg.assumes.size(); ++i) {
                    if (i) std::cout << ", ";
                    std::cout << "\"" << cfg.assumes[i] << "\"";
                }
                std::cout << "]";
            }
            std::cout << "\n";
        }
    }
    if (verbose_) {
        std::cout << "    OPTMAX unified new-PM pipeline complete" << '\n';
    }

    // ── Post-OPTMAX module-level cleanup ─────────────────────────────────
    // After aggressively inlining + optimising each OPTMAX function individually,
    // run a final module-level IPO sweep to reclaim dead code and globals that
    // the per-function passes could not see.
    {
        llvm::ModulePassManager PostIPO;
        // GlobalOpt: constant-fold and internalize simple global variables that
        // now have known initializer values after inlining and IPSCCP.
        PostIPO.addPass(llvm::GlobalOptPass());
        // DeadArgumentElimination: remove function arguments that are never
        // used after specialisation / constant propagation.
        PostIPO.addPass(llvm::DeadArgumentEliminationPass());
        // GlobalDCE: remove dead global variables and functions (e.g., helpers
        // that were fully inlined and have no remaining callers).
        PostIPO.addPass(llvm::GlobalDCEPass());
        PostIPO.run(*module, MAMMax);
    }
}

void CodeGenerator::writeObjectFile(const std::string& filename) {
    // Initialize only native target
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    const std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
    module->setTargetTriple(targetTriple);
#else
    module->setTargetTriple(targetTripleStr);
#endif

    auto targetMachine = createTargetMachine();
    if (!targetMachine) {
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "Failed to create target machine"});
    }

    module->setDataLayout(targetMachine->createDataLayout());

    std::error_code EC;
    llvm::raw_fd_ostream dest(filename, EC, llvm::sys::fs::OF_None);

    if (EC) {
        throw DiagnosticError(Diagnostic{
            DiagnosticSeverity::Error, {"", 0, 0}, "Could not open file '" + filename + "': " + EC.message()});
    }

    llvm::legacy::PassManager pass;
#if LLVM_VERSION_MAJOR >= 18
    auto fileType = llvm::CodeGenFileType::ObjectFile;
#else
    auto fileType = llvm::CGFT_ObjectFile;
#endif

    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "TargetMachine can't emit a file of this type"});
    }

    pass.run(*module);
    dest.flush();
    // Detect errors during close (e.g. I/O errors that occurred during write).
    if (dest.has_error()) {
        const std::string errMsg = "Error writing object file '" + filename + "': " + dest.error().message();
        dest.clear_error();
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, errMsg});
    }
}

void CodeGenerator::writeBitcodeFile(const std::string& filename) {
    // Initialize native target so the data layout can be set correctly.
    llvm::InitializeNativeTarget();

    const std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
    module->setTargetTriple(targetTriple);
#else
    module->setTargetTriple(targetTripleStr);
#endif

    auto targetMachine = createTargetMachine();
    if (targetMachine) {
        module->setDataLayout(targetMachine->createDataLayout());
    }

    std::error_code EC;
    llvm::raw_fd_ostream dest(filename, EC, llvm::sys::fs::OF_None);
    if (EC) {
        throw DiagnosticError(Diagnostic{
            DiagnosticSeverity::Error, {"", 0, 0}, "Could not open file '" + filename + "': " + EC.message()});
    }

    llvm::WriteBitcodeToFile(*module, dest);
    dest.flush();
    if (dest.has_error()) {
        const std::string errMsg = "Error writing bitcode file '" + filename + "': " + dest.error().message();
        dest.clear_error();
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, errMsg});
    }
}

} // namespace omscript
