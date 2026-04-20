#include "codegen.h"
#include "diagnostic.h"
#include "hardware_graph.h"
#include "superoptimizer.h"
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
#include <llvm/Analysis/BranchProbabilityInfo.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/IRBuilder.h>
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
    // Streaming stores are profitable when the written working set exceeds L1D,
    // causing cache pollution on every iteration.  Query TTI for the actual L1D
    // size; fall back to the widely-observed 32 KB default.
    // Element size is conservatively assumed at 8 bytes (int64/double).
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
    // Modern Intel CPUs have 12 WC buffers; AMD Zen has 8–12.
    // 64-byte cache lines correlate with the generation of CPUs that have
    // more WC buffers.  Use that as a lightweight proxy.
    model.writeCombiningStoreBudget = (cacheLineBytes >= 64u) ? 10u : 8u;

    // ── Register-file–derived budgets ─────────────────────────────────────
    const unsigned scalarRegs = TTI.getNumberOfRegisters(/*Vector=*/false);
    if (scalarRegs > 0) {
        // Tree rebalancing max width: reserve 6 registers for values live outside
        // the tree (loop induction variable, accumulator, pointers, flags) so that
        // widening the tree does not spill those live-on-exit values to the stack.
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
// Each round infers non-negative flags first, then converts srem/sdiv to urem/udiv.
// Returns the total number of conversions performed across all rounds.
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
// Converts if-else chains to selects, hoists/sinks common instructions,
// converts switches to lookup tables, and speculatively simplifies blocks.
// bonusInstThreshold=10 allows up to 10 extra instructions to be speculated
// when converting branches to selects — enough for cascading-if classify()
// patterns and multi-condition guards without over-speculating complex branches.
// Raised from 6 to 10 to match GCC's -O3 default for if-conversion: GCC uses
// an instruction count limit of ~10 before deciding not to predicate a branch.
// 10 is safe because OmScript OPTMAX functions have nounwind+nosync semantics,
// and the benchmark suite showed no register-pressure regressions at this value.
// convertSwitchRangeToICmp: converts switch statements with contiguous case
// ranges to icmp+branch sequences, which are more efficient on modern OoO CPUs
// with branch prediction than the switch dispatch table.
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
// Uses a higher bonusInstThreshold (24) to convert more branches to predicated
// (select) instructions, creating larger basic blocks that give the scheduler
// and register allocator more freedom.  This is the IR-level equivalent of
// hyperblock formation in traditional compilers.
//
// Threshold=24 is twice the previous value of 12; it covers 4-6 chained
// comparisons with arithmetic (classify() patterns, multi-range checks) without
// the register-pressure blowup of unlimited speculation.  Since OmScript does
// not care about compile time, the extra SimplifyCFG analysis cost is worthwhile
// for the I-cache density benefit of predicated regions.
//
// needCanonicalLoops(false) allows SimplifyCFG to break canonical loop form
// when it enables more aggressive if-conversion — the loop canonicalization
// passes that run later will restore the form if needed.
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
//
// Microarchitectural motivation (Zen 3 / Intel µop cache):
//   Zen 3 has a 4096-entry micro-op cache that supplies decoded instructions
//   to the backend at up to 4 µops/cycle without involving the decoders.
//   When the hot loop body (including any unrolled prologue/epilogue) exceeds
//   the µop cache capacity, instructions overflow to the decoders, reducing
//   front-end bandwidth and increasing pipeline bubbles.
//
// Strategy:
//   Walk every loop in the function.  Estimate target-aware frontend pressure
//   using TTI user-cost ("uop-like" cost) over loop-body IR instructions.
//   If the estimated cost exceeds the unified execution model's
//   opcacheLoopUopBudget:
//     1. Attach `llvm.loop.unroll.disable = true` to suppress further
//        unrolling (which would make the body even larger).
//     2. Attach `llvm.loop.distribute.enable = true` so LoopDistributePass
//        can split independent memory access patterns into separate loops,
//        each of which fits in the µop cache.
//
// The budget defaults to 256 uop-cost units and is architecture-aware through
// TTI costing, so vectorized/scalar instruction mixes are accounted for by
// target cost tables rather than fixed IR-instruction counts.
//
// This pass runs at OptimizerLastEP so it sees fully-optimised IR.
// ─────────────────────────────────────────────────────────────────────────────
struct OpcacheLoopPartitionPass
    : public llvm::PassInfoMixin<OpcacheLoopPartitionPass> {

    // Attach loop metadata to the back-edge branch of `L`.
    // Adds `llvm.loop.unroll.disable` and `llvm.loop.distribute.enable`
    // while preserving any existing loop metadata operands.
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
        // independent memory-access sub-groups it will fission the body into
        // smaller loops, each fitting the µop cache.  Only effective when the
        // loop has genuinely independent partitions; otherwise it is a no-op.
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
//
// Microarchitectural motivation:
//   For loops that write to large contiguous arrays with no data reuse
//   (e.g. zero-fill, copy-out, accumulate-to-new-buffer), regular writes
//   evict useful hot data from L1/L2 cache, causing cache pollution.
//   Non-temporal (streaming) stores use write-combining buffers to bypass
//   the cache and go directly to memory, preserving cache bandwidth for
//   the data that actually needs to stay cached.
//
// Detection heuristic:
//   A store is eligible for non-temporal promotion if:
//     a) It is inside a loop whose estimated trip count (from SCEV) is ≥
//        the unified execution model's streamingMinElements (default
//        4096 elements × 8 bytes = 32 KB,
//        larger than a typical L1D), AND
//     b) The stored address has TBAA metadata indicating it is an array
//        element (not a length header or struct field — those are small).
//
// Implementation:
//   Walk all stores in the function.  For stores marked with array-element
//   TBAA and inside a loop with a large trip count, set the `!nontemporal`
//   metadata to 1.  LLVM's backend lowers non-temporal stores to MOVNT
//   instructions on x86 and equivalent on other targets.
//
//   An SFENCE is not explicitly emitted here — LLVM inserts the appropriate
//   memory fence after streaming store sequences during lowering.
//
// Additional guards: skip loops that read back from the same base object and
// cap promotions per loop to avoid write-combining-buffer pressure.
// ─────────────────────────────────────────────────────────────────────────────
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
        // We check for this name substring in TBAA metadata to restrict
        // streaming stores to array element writes (not length headers etc.).
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
                // SE can prove the trip count is ≥ threshold.
                // This avoids false-positive streaming stores on small loops
                // with unknown bounds (e.g. user-input sizes).
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
//
// Within each basic block, hoists independent loads above immediately preceding
// stores.  Clustering all independent loads as early as possible lets the OoO
// execution unit issue them in parallel, hiding multiple cache-miss latencies
// simultaneously — maximising memory-level parallelism (MLP).
//
// Correctness guarantee:
//   A load L may be hoisted above a store S only when:
//   (a) AA proves L's address and S's target are NoAlias.
//   (a2) Pointer provenance is strong (distinct allocas/globals, or explicit
//        noalias-annotated argument provenance) so AA facts are trustworthy.
//   (b) No instruction between S and L transitively computes L's address
//       (data dependency: loading through a pointer written by S would be
//        incorrect if we reorder the load before the write).
//   (c) No volatile / atomic / fence / call appears in the gap.
//
// Microarchitectural motivation:
//   Modern x86/ARM OoO cores maintain a load queue (Zen4: 192, M-series: 256+
//   entries) and a store buffer (~72–96 entries).  When loads appear after
//   stores in program order, the LSU must perform a store-to-load forwarding
//   check — typically 2–4 cycles overhead even for non-aliasing pairs.
//   Hoisting loads above non-aliasing stores eliminates this overhead and
//   allows multiple cache misses to be issued in the same dispatch window.
//
//   MAPR operates at IR level where MemorySSA alias information is most
//   precise and is complementary to the backend instruction scheduler (which
//   operates on machine instructions after instruction selection).
//
//   Placement: after DSE+MemCpyOpt (dead stores already removed, shrinking
//   the set of stores that could block hoisting), before InstCombine so that
//   newly-adjacent load pairs are visible for potential load-combining.
// ─────────────────────────────────────────────────────────────────────────────
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
    // computing LI's memory address (data-dependency frontier).  Used to
    // detect whether hoisting LI past instruction P would create a
    // use-before-definition (P computes part of LI's address).
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
//
// Rebalances linear chains of identical associative binary operations into
// balanced binary trees, reducing critical path depth from O(n) to O(log₂ n).
// This exposes instruction-level parallelism (ILP) that the OoO core can
// exploit by executing independent sub-trees in parallel.
//
// Motivation:
//   LLVM's ReassociatePass focuses on exposing CSE by canonicalising operand
//   order, not on minimising dependency depth.  For pure arithmetic chains:
//
//     t1 = a + b          depth 1
//     t2 = t1 + c         depth 2
//     t3 = t2 + d         depth 3
//     t4 = t3 + e         depth 4
//
//   the balanced form:
//
//     t12 = a + b         depth 1 (parallel with t34)
//     t34 = c + d         depth 1
//     t1234 = t12 + t34   depth 2
//     result = t1234 + e  depth 3
//
//   has depth ⌈log₂ n⌉ instead of n−1.  On a 4-wide OoO core, a 4-element
//   chain goes from depth 3 → depth 2 — a 1-cycle saving per reduction, per
//   OPTMAX function call.  For longer chains (8, 16 elements), savings are
//   3 and 4 cycles respectively, compounding across loop iterations.
//
// Correctness:
//   Integer add/mul/and/or/xor are unconditionally associative and commutative.
//   FP fadd/fmul require the 'reassoc' fast-math flag (set by OmScript's
//   @OPTMAX fast_math annotation) to allow reassociation without changing
//   rounding behaviour.
//
//   Intermediate nodes used outside the chain are treated as leaves to avoid
//   duplicating computation.  Only chains with ≥ kMinLeaves leaves are
//   rebalanced: 4 leaves → depth 2 instead of 3 (guaranteed 1-cycle saving).
//
// Placement: between InstCombine and AggressiveInstCombine.  InstCombine
// canonicalises operand ordering first (exposing uniform-opcode chains);
// AggressiveInstCombine later removes any dead intermediates.
// ─────────────────────────────────────────────────────────────────────────────
struct TreeHeightReductionPass
    : public llvm::PassInfoMixin<TreeHeightReductionPass> {

    // Minimum number of leaves to make tree balancing profitable.
    // 4 leaves: linear depth 3 → balanced depth 2 (+1 cycle saved).
    // 3 leaves: linear depth 2 → balanced depth 2 (no gain).
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
    // `leaves`.  A leaf is any value that is NOT a single-use BinOp with
    // the same opcode in the current basic block.  The single-use constraint
    // ensures we never duplicate computation for values used elsewhere.
    // `isRoot=true` bypasses the use-count check for the tree root itself
    // (the root may have multiple external users and still be expandable).
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
    // `opcode`.  New instructions are inserted via the IRBuilder (which is
    // positioned immediately before the original root instruction).
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
//
// Converts small, high-entropy (near-50/50 probability) conditional branches
// into select instructions, eliminating branch-mispredict flushes (~15–20
// cycle penalty on modern cores) at the cost of unconditionally executing
// both branch arms.
//
// This differs from SimplifyCFG's SpeculateBlocks in two key ways:
//
//   1. BER checks branch-probability metadata: if one direction has
//      probability ≥ 75% (a "biased" branch), the CPU's branch predictor
//      already handles it well at near-zero mispredict rate.  Converting such
//      a branch to select wastes ALU cycles on the cold path's computation.
//      BER skips biased branches and leaves them as-is.
//
//   2. BER uses a µop-budgeted arm threshold: the conversion is profitable
//      only when both arms together fit within the unified model's
//      branchSpeculationUopBudget.
//      Above this limit, the speculative execution cost exceeds the average
//      mispredict penalty.  SimplifyCFG's bonusInstThreshold applies a
//      per-arm limit; BER limits the combined cost, which is a tighter bound.
//
// BER uses a unified expected-value model:
//   convert only when speculative arm uop-cost is lower than expected
//   mispredict penalty (probability-weighted).  Without profile weights,
//   BER scales confidence down and becomes much more conservative.
//
// Pattern detected (diamond CFG):
//
//   head:
//     br cond, %trueBB, %falseBB
//   trueBB:   [small pure arm]  br %merge
//   falseBB:  [small pure arm]  br %merge
//   merge:
//     %phi = phi [v_true, %trueBB], [v_false, %falseBB]
//
// After transformation:
//   head:
//     [trueBB instrs cloned here]
//     [falseBB instrs cloned here]
//     %phi = select cond, v_true, v_false
//     br %merge   (unconditional)
//
// Placement: immediately before SimplifyCFG so that SimplifyCFG can
// eliminate the now-empty trueBB and falseBB blocks.
// ─────────────────────────────────────────────────────────────────────────────
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
    // BB, or UINT_MAX if any instruction has side effects, memory accesses, or
    // is a PHI node (PHI nodes cannot be cloned into the head block without
    // violating LLVM IR's invariant that PHIs must be grouped at block tops).
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
        //   convert iff speculative compute cost is lower than expected
        //   branch-mispredict penalty.
        // Scale factor: with profile weights we trust the probability exactly;
        // without, we use 0.5 (conservative assumption: modern CPUs achieve
        // ~93-97% prediction accuracy for structured code, so the minority
        // side has ~3-7% probability, but for truly unpredictable branches
        // minProb ≈ 0.5 and the penalty dominates).  The old 0.35 was too
        // pessimistic, causing us to keep branches that modern OoO CPUs
        // would mispredict frequently in tight loops.
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
        // in the merge block's phi nodes (no escaping uses that would require
        // the cloned instruction to be visible outside head).
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
    // unset.  When an explicit -march is given, it takes precedence because
    // LLVM's createTargetMachine uses a single CPU parameter for both
    // instruction selection and scheduling.
    // "native" is resolved to the host CPU name just like -march=native.
    if (!mtuneCpu_.empty() && isNative) {
        if (mtuneCpu_ == "native") {
            cpu = llvm::sys::getHostCPUName().str();
        } else {
            cpu = mtuneCpu_;
        }
    }
}

// ---------------------------------------------------------------------------
// Shared TargetMachine construction
// ---------------------------------------------------------------------------
// Both runOptimizationPasses() and writeObjectFile() need a configured
// TargetMachine.  This helper consolidates the duplicated triple setup,
// target lookup, TargetOptions configuration, and version-conditional
// createTargetMachine() call into a single place.

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
    // level.  This is independent of fast-math flags — it tells the backend
    // code generator to fuse FP operations when the hardware supports it
    // (e.g. x86 FMA3, ARM NEON).  FMA fusion produces a more precise result
    // (one rounding instead of two) and uses fewer cycles (1 fma vs 1 fmul +
    // 1 fadd), so it is always beneficial.  Without this, the backend only
    // fuses when fast-math is globally enabled.
    if (optimizationLevel >= OptimizationLevel::O2) {
        opt.AllowFPOpFusion = llvm::FPOpFusion::Fast;
    }
    const std::optional<llvm::Reloc::Model> RM = usePIC_ ? llvm::Reloc::PIC_ : llvm::Reloc::Static;

    std::string cpu;
    std::string features;
    resolveTargetCPU(cpu, features);

    // Map the compiler's optimization level to LLVM's backend CodeGenOpt level
    // so that instruction selection, scheduling, and register allocation use
    // the appropriate aggressiveness.
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

void CodeGenerator::runOptimizationPasses() {
    // Ensure the native target is initialized before we try to create a
    // TargetMachine.  These calls are idempotent and fast after the first.
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

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
    // standard pipeline which includes interprocedural optimizations such as
    // function inlining, IPSCCP (sparse conditional constant propagation),
    // GlobalDCE (dead function removal), jump threading, correlated value
    // propagation, loop vectorization (O2+), SLP vectorization (O2+), and more.
    //
    // PipelineTuningOptions controls vectorization and loop unrolling globally.
    // This is the correct way to enable/disable these passes in the new PM;
    // it replaces the previous approach of injecting per-loop metadata hints
    // with llvm.loop.vectorize.enable/llvm.loop.unroll.enable, which caused
    // "unable to perform the requested transformation" warnings when the
    // optimizer could not satisfy the forced hints.
    llvm::PipelineTuningOptions PTO;
    // At O1, keep vectorization and unrolling disabled to match LLVM's
    // standard O1 behavior.  Enabling them at O1 (via the global flags below)
    // causes a compiler hang: LLVM's loop vectorizer inlines inner functions
    // (e.g. collatz, fib) marked inlinehint into outer benchmark loops, then
    // tries to vectorize the resulting complex nested while-loops.  With
    // large loop-trip-count constants (640000 x collatz(837799)) the SLP /
    // loop vectorizer analysis doesn't terminate within a reasonable time.
    // At O2+, the inliner runs before the vectorizer in a dedicated phase, so
    // the patterns that trigger the slow analysis are already eliminated.
    const bool atLeastO2 = optimizationLevel >= OptimizationLevel::O2;
    PTO.LoopVectorization = atLeastO2 && enableVectorize_;
    PTO.SLPVectorization  = atLeastO2 && enableVectorize_;
    // Re-enable LLVM's cost-model-driven loop unrolling.  The standard O3
    // pipeline has excellent register-pressure-aware unrolling heuristics.
    // We previously disabled this because the HGOE pre-pipeline was injecting
    // bad unroll metadata that caused over-unrolling; now that pre-pipeline
    // annotation is disabled, LLVM's own unroller makes good decisions.
    // Restrict to O2+ for the same reason as vectorization above.
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
    // MergeFunctions deduplicates identical function bodies, shrinking
    // I-cache footprint. CallGraphProfile biases the inliner toward
    // functions on hot call-graph edges (useful even without explicit PGO).
    if (optimizationLevel >= OptimizationLevel::O2) {
        PTO.MergeFunctions = true;
        PTO.CallGraphProfile = true;
        // Increase inliner threshold from the LLVM default (225) to account
        // for OmScript's small-function style.  Most OmScript functions are
        // short (10-30 statements), so a higher threshold enables full inlining
        // of typical helper functions and loop bodies.  The threshold is the
        // cost budget (LLVM 18 default is 225 at O2): instructions below this
        // cost are always inlined.
        PTO.InlinerThreshold = 500;
    }
    if (optimizationLevel == OptimizationLevel::O3) {
        PTO.InlinerThreshold = 3000; // aggressive inlining for maximum IPC (compile time not a concern)
    }
    // ForgetAllSCEVInLoopUnroll forces SCEV to recompute trip counts after
    // each unrolling step.  Without this, stale trip-count information from
    // before unrolling can cause the unroller to make suboptimal decisions
    // on subsequent iterations (e.g. under-unrolling a loop whose trip
    // count became statically known after partial unrolling).
    // Promoted from O3-only to O2+: the compile-time cost is small (one
    // extra SCEV query per unroll iteration), and accurate trip counts are
    // critical for OmScript's counted-loop patterns.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PTO.ForgetAllSCEVInLoopUnroll = true;
    }

    // ---------------------------------------------------------------------------
    // PGO (Profile-Guided Optimization) support
    // ---------------------------------------------------------------------------
    // Two-phase workflow:
    //
    //   Phase 1 – Instrumentation generation (--pgo-gen=<path>):
    //     The compiler inserts LLVM IR instrumentation counters into every
    //     basic block and function entry.  At program exit the runtime writes
    //     a raw profile file (.profraw) to the specified path.  Run the binary
    //     with representative workloads to collect profile data, then convert:
    //       llvm-profdata merge -output=prog.profdata prog.profraw
    //
    //   Phase 2 – Profile-use optimization (--pgo-use=<path>):
    //     The optimizer reads the merged .profdata file and uses the branch
    //     and call-count data to guide: function inlining decisions (hot
    //     callees get larger inline budgets), branch layout (hot side goes
    //     in the fall-through path to improve I-cache density), loop
    //     trip-count estimation (enables better vectorization and unrolling),
    //     and cold-code sinking (rare error paths moved out of hot regions).
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
    // Merged into one callback to guarantee a single, ordered execution:
    //   1. InferFunctionAttrs — annotate library functions before any IPO runs.
    //   2. LowerExpectIntrinsic — convert llvm.expect to branch-weight metadata
    //      so SimplifyCFG / JumpThreading / LoopRotate can use likely/unlikely hints.
    //   3. SyntheticCountsPropagation (O2+, LLVM < 20) — propagate estimated call
    //      counts so the inliner prioritises functions on hot call-graph edges.
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
    // Reassociate canonicalizes expression trees (e.g., (a+b)+c → a+(b+c))
    // enabling better CSE and constant folding.  EarlyCSE eliminates trivially
    // redundant computations before the heavier optimization passes run,
    // reducing IR size and compilation time for the downstream pipeline.
    // The PeepholeEP callback runs after EVERY InstCombine invocation in
    // the pipeline.  Keep this extremely lightweight — only EarlyCSE which
    // is cheap and catches redundancies InstCombine creates.  Reassociate
    // is already in the main pipeline and is too expensive to run repeatedly.
    if (optimizationLevel >= OptimizationLevel::O1) {
        const bool useMemSSA = optimizationLevel >= OptimizationLevel::O2;
        PB.registerPeepholeEPCallback([useMemSSA](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            FPM.addPass(llvm::EarlyCSEPass(useMemSSA));
        });
    }

    // At O2+, infer memory attributes (readnone, readonly, argmemonly, etc.)
    // on functions using the full call graph in reverse post-order.  Running
    // this early — before inlining and other IPO passes — gives alias analysis
    // and LICM accurate function-level memory effects, which propagates to
    // more precise results for every downstream function-level optimisation.
    //
    // Also run PostOrderFunctionAttrsPass (bottom-up direction) alongside the
    // top-down ReversePostOrder pass.  Together they form a bidirectional
    // attribute inference: PostOrder propagates readnone/readonly up through
    // callees first (a callee with no memory accesses makes its caller
    // eligible for readonly if the caller only calls it and other readonly
    // functions), while ReversePostOrder propagates constraints top-down.
    //
    // At O3, superblock/hyperblock formation is appended in the same callback
    // so both transformations share one ordered OptimizerEarlyEP invocation.
    //
    // Superblock formation (trace scheduling): JumpThreading duplicates blocks
    // along hot paths to create single-entry multiple-exit regions, eliminating
    // branches that break instruction-level parallelism.
    //
    // Hyperblock formation (if-conversion): aggressive SimplifyCFG converts
    // diamond-shaped branch patterns into predicated (select) instructions,
    // creating wider basic blocks for the scheduler and register allocator.
    // This is particularly effective for OmScript's pattern-matching style
    // (cascading if-else classify() functions).
    //
    // Registered at OptimizerEarlyEP so both run after inlining has created
    // the full intra-procedural CFG but before the loop optimizer and
    // vectorizer — giving them larger, straighter basic blocks to work with.
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool earlyO3 = optimizationLevel >= OptimizationLevel::O3;
        PB.registerOptimizerEarlyEPCallback(
            [earlyO3](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
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
                // FlattenCFGPass is intentionally omitted: it collapses all if-else
                // chains into sequential code without TTI guidance, which increases
                // register pressure and can lengthen critical dependency chains on
                // out-of-order CPUs.  SimplifyCFG with hyperblockCFGOpts already
                // performs targeted if-conversion where profitable.
                SuperblockFPM.addPass(llvm::SimplifyCFGPass(hyperblockCFGOpts()));
                // Phase 5: Cleanup — combine and eliminate dead code.
                SuperblockFPM.addPass(llvm::InstCombinePass());
                SuperblockFPM.addPass(llvm::ADCEPass());
                MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(SuperblockFPM)));
            }
        });
    }

    // At O2+, register CallSiteSplitting in the scalar optimizer.
    // NOTE: SCCP is already part of the default O2/O3 pipeline — only
    // CallSiteSplitting, which is not in the default pipeline, is added.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerScalarOptimizerLateEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            FPM.addPass(llvm::CallSiteSplittingPass());
        });
    }

    // ── Auto-parallelization pipeline configuration ─────────────────────
    // OmScript supports automatic loop parallelization via two complementary
    // mechanisms:
    //
    //   1. LLVM Loop Vectorizer: uses !llvm.access.group + parallel_accesses
    //      metadata (attached by codegen_stmt.cpp) to vectorize loops that
    //      are provably parallel.
    //
    //   2. Polly Polyhedral Optimizer: for affine loop nests, Polly performs
    //      high-level transformations (tiling, interchange, fusion) and can
    //      generate OpenMP parallel loops.  We configure Polly's parallelism
    //      options via LLVM's cl::opt mechanism after loading the plugin.
    //
    // The -fparallelize / -fno-parallelize flag controls both mechanisms,
    // and the @parallel / @noparallel function annotations provide per-
    // function control.

    // At O2+ with -floop-optimize, load the LLVM Polly polyhedral optimizer
    // plugin.  Polly provides high-level loop transformations (tiling, fusion,
    // interchange) based on the polyhedral model, improving data locality and
    // enabling automatic parallelization for affine loop nests.  The plugin
    // registers its analysis passes and pipeline extension-point callbacks so
    // that the standard buildPerModuleDefaultPipeline() automatically invokes
    // Polly at the appropriate point in the optimization pipeline.
#ifdef POLLY_LIB_PATH
    if (optimizationLevel >= OptimizationLevel::O2 && enableLoopOptimize_) {
        if (verbose_) {
            std::cout << "    Loading Polly polyhedral loop optimizer plugin..." << '\n';
        }
        auto pollyPlugin = llvm::PassPlugin::Load(POLLY_LIB_PATH);
        if (pollyPlugin) {
            pollyPlugin->registerPassBuilderCallbacks(PB);
            // Now that Polly has registered its cl::opt entries, configure
            // automatic parallelization if -fparallelize is active.
            //
            // -polly-parallel: generate OpenMP parallel loops for profitable
            //   outer loops in affine loop nests
            // -polly-vectorizer=stripmine: use Polly's strip-mining vectorizer
            //   for inner loops (complementing LLVM's LoopVectorizer)
            // -polly-run-dce: dead code elimination after Polly transforms
            // -polly-run-inliner: inline small helper functions Polly creates
            // -polly-invariant-load-hoisting: hoist loop-invariant loads for
            //   better data locality
            // -polly-scheduling=dynamic: dynamic OpenMP scheduling for uneven
            //   workloads (more robust than static for user code)
            // -polly-scheduling-chunksize=1: fine-grained chunks for balance
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
    // LLVM's default pipeline already includes it, and we previously added it
    // again as an explicit pipeline-start callback.  However, this pass auto-
    // infers aggressive attributes (allocsize, allockind, inaccessibleMemOnly)
    // on well-known C library functions like malloc.  Those attributes interact
    // poorly with the loop unroller/optimizer when the allocated buffer is
    // immediately written to in a loop (e.g. string repetition via strcat or
    // memcpy): the optimizer can incorrectly eliminate the loop exit condition,
    // producing an infinite loop and segfault.  The attributes we manually set
    // on library function declarations are sufficient.

    // At O3, register ArgumentPromotionPass late in the CGSCC pipeline
    // to promote by-reference (pointer) arguments to by-value when the
    // callee only reads through the pointer.  This eliminates pointer
    // chasing and memory loads at call sites, and the promoted values
    // become SSA registers that downstream passes can optimise freely.
    // Only at O3 because argument promotion can increase register
    // pressure and code size when the promoted values are large.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerCGSCCOptimizerLateEPCallback(
            [](llvm::CGSCCPassManager& CGPM, llvm::OptimizationLevel /*Level*/) {
            CGPM.addPass(llvm::ArgumentPromotionPass());
        });
    }
    // AttributorCGSCCPass performs context-sensitive, call-graph-aware
    // attribute inference.  Unlike InferFunctionAttrsPass (which only
    // infers library function attributes), Attributor propagates:
    // noalias, nofree, nosync, nounwind, readnone, readonly, willreturn,
    // nocapture, and more across the entire call graph.  This enables
    // downstream passes (LICM, DSE, vectorizer) to make stronger
    // assumptions about function behavior.
    // Promoted from O3 to O2: OmScript's ownership model already
    // guarantees many of these attributes, but Attributor can infer
    // additional ones (e.g. readnone for pure functions, nocapture for
    // non-escaping parameters) that the manual codegen annotations miss.
    //
    // PostOrderFunctionAttrsPass (bottom-up SCC traversal) runs in the same
    // CGSCC callback to infer readnone/readonly/nosync/nounwind from the IR
    // of each SCC.  Unlike Attributor (which uses a fixpoint lattice),
    // PostOrderFunctionAttrsPass is fast and directly marks functions based
    // on whether they contain load/store/call instructions.  Running it
    // alongside Attributor provides a complementary signal that helps the
    // inliner's cost model even when Attributor can't converge.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerCGSCCOptimizerLateEPCallback(
            [](llvm::CGSCCPassManager& CGPM, llvm::OptimizationLevel /*Level*/) {
            CGPM.addPass(llvm::PostOrderFunctionAttrsPass());
            CGPM.addPass(llvm::AttributorCGSCCPass());
        });
    }

    // At O2+ with -floop-optimize, register LoopDistributePass to run just
    // before the vectorizer starts.  Loop distribution splits a loop with
    // multiple independent memory access patterns into separate loops, each
    // touching a smaller working set — this improves cache locality and
    // At O2+, register all pre-vectorization passes in a single callback.
    // This runs LoopDistribute, LoopLoadElim, LoopFuse (if enabled), and
    // then re-canonicalizes loops + promotes reductions to SSA form.
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool loopOpt = enableLoopOptimize_;
        const bool loopOptOrO3 = loopOpt || (optimizationLevel >= OptimizationLevel::O3);
        PB.registerVectorizerStartEPCallback(
            [loopOptOrO3](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            if (loopOptOrO3) FPM.addPass(llvm::LoopDistributePass());
            FPM.addPass(llvm::LoopLoadEliminationPass());
            if (loopOptOrO3) FPM.addPass(llvm::LoopFusePass());
            FPM.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
            FPM.addPass(llvm::PromotePass());
            FPM.addPass(llvm::InstCombinePass());
            FPM.addPass(llvm::LoopSimplifyPass());
            FPM.addPass(llvm::LCSSAPass());
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
            // branch conditions to tighten comparisons, convert signed ops
            // to unsigned, and prove non-negativity.  Running it before the
            // vectorizer gives the cost model more precise value ranges,
            // enabling vectorization of loops that would otherwise be rejected
            // due to conservative overflow assumptions.
            FPM.addPass(llvm::CorrelatedValuePropagationPass());
            // ConstraintElimination uses a system of linear constraints derived
            // from branch conditions to prove or disprove comparisons.  Running
            // it here (before the vectorizer) lets it eliminate induction-variable
            // bounds checks that CVP left behind, giving the vectorizer a cleaner
            // loop body — no scalar epilogue is needed for checks that are proven
            // always-true.  It also runs later in ScalarOptimizerLateEPCallback
            // for post-vectorization cleanup; the two placements are complementary.
            FPM.addPass(llvm::ConstraintEliminationPass());
            // InductiveRangeCheckElimination removes bounds checks inside loops
            // with provably bounded induction variables.  After CVP + CE have
            // narrowed value ranges, IRCE can eliminate per-iteration array-index
            // guards, allowing the vectorizer to produce tighter vectorised loops
            // without fallback scalar paths.
            FPM.addPass(llvm::IRCEPass());
            // SimplifyCFG merges trivially-redundant blocks and eliminates
            // unreachable code before the vectorizer.  Cleaner CFG structure
            // helps the vectorizer's control-flow analysis and reduces the
            // number of scalar epilogue paths it must handle.
            // Use aggressive options: convert if-else chains to selects,
            // hoist/sink common instructions, and convert switches to lookup
            // tables — critical for classify()-style cascading-if patterns.
            FPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
        });
    }

    // At O2+ with -floop-optimize, register LoopInterchangePass at the end of
    // the loop optimizer to interchange nested loops for better cache locality.
    // This transforms loop-nest orders so that the innermost loop accesses
    // memory contiguously (e.g. column-major → row-major), dramatically
    // reducing cache misses for matrix-style code.
    // Lowered from O3 to O2 because matrix-style loops are common even in
    // moderate-optimization builds.
    if (optimizationLevel >= OptimizationLevel::O2 && enableLoopOptimize_) {
        PB.registerLoopOptimizerEndEPCallback(
            [](llvm::LoopPassManager& LPM, llvm::OptimizationLevel /*Level*/) { LPM.addPass(llvm::LoopInterchangePass()); });
    }

    // NOTE: AggressiveInstCombine is already part of the default O2/O3
    // pipeline.  Registering it at PeepholeEP caused it to run after EVERY
    // InstCombine invocation, which is extremely expensive with diminishing
    // returns.  Removed to improve compile time.

    // At O2+, register all extra loop optimizer passes in consolidated callbacks.
    {
        const bool isO3 = optimizationLevel >= OptimizationLevel::O3;
        const bool loopOpt = enableLoopOptimize_;

        if (optimizationLevel >= OptimizationLevel::O2) {
            PB.registerLateLoopOptimizationsEPCallback([isO3, loopOpt](llvm::LoopPassManager& LPM, llvm::OptimizationLevel /*Level*/) {
                LPM.addPass(llvm::SimpleLoopUnswitchPass(/*NonTrivial=*/true));
                // LoopVersioningLICM creates a versioned copy of the loop
                // with runtime alias checks, allowing LICM to hoist loads
                // and stores that may alias.  Lowered from O3-only to O2+
                // because OmScript's noalias semantics make the runtime
                // checks trivially optimizable, benefiting general code.
                // Enabled unconditionally at O3 for maximum performance.
                if (loopOpt || isO3)
                    LPM.addPass(llvm::LoopVersioningLICMPass());
            });
        }

        if (optimizationLevel >= OptimizationLevel::O2) {
            PB.registerLoopOptimizerEndEPCallback([isO3, loopOpt](llvm::LoopPassManager& LPM, llvm::OptimizationLevel /*Level*/) {
                // LoopIdiomRecognize detects loop patterns that implement
                // memset, memcpy, or similar operations and replaces them with
                // optimized library calls.
                LPM.addPass(llvm::LoopIdiomRecognizePass());
                // IndVarSimplify canonicalizes induction variables for
                // vectorization and unrolling.
                LPM.addPass(llvm::IndVarSimplifyPass());
                // LoopDeletion removes provably dead loops.
                LPM.addPass(llvm::LoopDeletionPass());
                LPM.addPass(llvm::LoopInstSimplifyPass());
                LPM.addPass(llvm::LoopSimplifyCFGPass());
                // CanonicalizeFreezeInLoops moves freeze instructions out of
                // loop bodies, allowing SCEV to compute precise trip counts
                // for loops that contain freeze-guarded induction variables.
                // This enables better vectorization and unrolling decisions.
                LPM.addPass(llvm::CanonicalizeFreezeInLoopsPass());
                // LoopFlatten collapses nested loops with a simple inner trip
                // count into a single loop, reducing loop overhead (branch,
                // compare, increment) and enabling better vectorization of
                // matrix-style access patterns.  Enabled at O3 unconditionally,
                // or at O2 when -floop-optimize is active.
                if (isO3 || loopOpt) {
                    LPM.addPass(llvm::LoopFlattenPass());
                    // LoopUnrollAndJam unrolls an outer loop and fuses (jams)
                    // iterations of the inner loop together, improving data
                    // reuse across outer-loop iterations.  This is especially
                    // beneficial for matrix multiplication and stencil patterns.
                    // OptLevel controls aggressiveness: 3 at O3, 2 at O2.
                    LPM.addPass(llvm::LoopUnrollAndJamPass(/*OptLevel=*/isO3 ? 3 : 2));
                }
                // LoopPredication converts bounds checks inside loops into
                // loop-invariant predicates (check once, not every iteration).
                // Safe at O2 and benefits array-heavy OmScript code.
                LPM.addPass(llvm::LoopPredicationPass());
                if (isO3) {
#if LLVM_VERSION_MAJOR >= 20
                    // LoopBoundSplit splits loops by bounds to enable better
                    // vectorization (e.g. separate aligned/unaligned iterations).
                    // Disabled on LLVM <= 19: hasProcessableCondition() null-
                    // dereferences on certain integer-comparison loop bounds
                    // (as seen in benchmark_jit_aot.om), causing a segfault
                    // during the PGO training run and the final build.
                    LPM.addPass(llvm::LoopBoundSplitPass());
#else
                    // LoopReroll recognizes manually-unrolled loop patterns and
                    // "rerolls" them into a single iteration with a larger trip
                    // count.  This enables the vectorizer to handle patterns
                    // that were over-unrolled by earlier passes or written as
                    // unrolled loops in the source code.
                    LPM.addPass(llvm::LoopRerollPass());
#endif
                }
            });
        }
    }

    // At O3, inject LoopDataPrefetchPass to insert software prefetch
    // instructions for loops with predictable stride access patterns.
    // This hides memory latency on hot loops that access large arrays by
    // issuing prefetch instructions ahead of the actual loads.  The pass
    // uses TTI (Target Transform Info) to decide prefetch distance and
    // cache line size based on the target CPU.
    // Only enabled at O3 because the cost of extra prefetch instructions
    // can hurt tight loops with good spatial locality (e.g. sequential
    // access to small arrays that fit in L1).
    // Registered at ScalarOptimizerLateEP because LoopDataPrefetchPass is a
    // FunctionPass that internally walks loop nests and analyses SCEVs.
    // At O2+, consolidate all ScalarOptimizerLateEP passes into one callback.
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool isO3 = optimizationLevel >= OptimizationLevel::O3;
        const bool loopOpt = enableLoopOptimize_;
        PB.registerScalarOptimizerLateEPCallback([isO3, loopOpt](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            // LoopDataPrefetch inserts software prefetch instructions for loops
            // with predictable stride access patterns.  Promoted from O3-only
            // to O2+ with -floop-optimize: OmScript's array-walking loops
            // benefit significantly from prefetching, and the overhead of extra
            // prefetch instructions is offset by reduced cache miss latency.
            // Without -floop-optimize, stays O3-only to avoid hurting tight
            // loops with good L1 locality.
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
            // than simple constant folding because it tracks value ranges
            // through phi nodes and conditional branches.
            FPM.addPass(llvm::SCCPPass());
            FPM.addPass(llvm::InstCombinePass());
            // ConstraintElimination uses branch conditions to narrow value
            // ranges, enabling unsigned loop vectorization and eliminating
            // redundant bounds checks.  Beneficial at O2+ (not just O3).
            FPM.addPass(llvm::ConstraintEliminationPass());
            // JumpThreading threads branches through basic blocks with
            // known conditions, reducing branch mispredictions.  Promoted
            // from O3 to O2 because OmScript generates many conditional
            // patterns (bounds checks, type checks) that benefit from
            // threading even at moderate optimization levels.
            FPM.addPass(llvm::JumpThreadingPass());
            // DivRemPairs reuses quotient from div for rem, saving an
            // expensive division instruction.  Promoted from O3 to O2
            // because OmScript's modulo-heavy patterns (hash tables,
            // index wrapping) directly benefit.
            FPM.addPass(llvm::DivRemPairsPass());
            // ConstantHoisting materializes expensive constants once and
            // shares them across basic blocks, reducing code size and
            // register pressure.  Promoted from O3 to O2.
            FPM.addPass(llvm::ConstantHoistingPass());
            // IRCE (Inductive Range Check Elimination) removes bounds checks
            // from loops with inductive ranges.  Promoted from O3 to O2
            // because OmScript's array-heavy loops with bounds checks benefit
            // significantly: the pass proves that the induction variable stays
            // within bounds for the entire loop, eliminating per-iteration checks.
            FPM.addPass(llvm::IRCEPass());
            // PartiallyInlineLibCalls inlines the fast path of math library
            // functions (sqrt, floor, ceil, etc.) and only calls the slow
            // errno-setting path when needed.  Promoted from O3 to O2 because
            // the fast path is a single instruction (e.g. sqrtsd) vs. a full
            // library call (~50 cycles), and OmScript's float operations
            // commonly use these functions.
            FPM.addPass(llvm::PartiallyInlineLibCallsPass());
            // SeparateConstOffsetFromGEP canonicalizes GEP chains by factoring
            // out constant offsets, enabling better CSE and addressing mode
            // selection.  Promoted from O3 to O2 because OmScript's array
            // layout (length header + data) creates GEP patterns with constant
            // +1 offsets that benefit from factoring.
            FPM.addPass(llvm::SeparateConstOffsetFromGEPPass());
            // GuardWidening merges multiple guard checks into a single
            // wider check, reducing branching overhead in bounds-checked
            // loops.  Promoted from O3 to O2 because OmScript generates
            // bounds checks on every array access, and widening them
            // eliminates redundant checks in multi-access loops.
            FPM.addPass(llvm::GuardWideningPass());
            if (isO3) {
                // LibCallsShrinkWrap wraps math library calls (sqrt, exp2, pow,
                // log, etc.) with fast-path domain checks.  When the argument
                // is known to be in the valid domain (e.g. non-negative for
                // sqrt), the expensive errno-setting/NaN-checking slow path is
                // bypassed entirely.  This is especially beneficial for
                // floating-point benchmarks that call math functions in loops.
                FPM.addPass(llvm::LibCallsShrinkWrapPass());
                FPM.addPass(llvm::SpeculativeExecutionPass());
                FPM.addPass(llvm::DFAJumpThreadingPass());
                FPM.addPass(llvm::SinkingPass());
                // NewGVN is a graph-based GVN that catches redundancies
                // classic GVN misses (e.g. through PHI nodes and memory).
                FPM.addPass(llvm::NewGVNPass());
            }
            // Aggressive SimplifyCFG at the end to convert if-else chains
            // into select instructions or lookup tables after all inlining,
            // jump threading, and CVP have simplified conditions.
            FPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
        });
    }

    // At O2+, register VectorCombinePass and LoopSinkPass as one of the last
    // function-level optimizations.
    //   VectorCombinePass: optimizes scalar/vector interactions using target
    //     cost models — converts redundant scalar extract/insert sequences
    //     into direct vector operations and eliminates unnecessary shuffles.
    //   LoopSinkPass: sinks loop-invariant code back into the loop body
    //     when the instruction is only used on a cold path, reducing register
    //     pressure on the hot path.  Uses block frequency heuristics and can
    //     optionally benefit from profile data when available.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            llvm::FunctionPassManager FPM;
            FPM.addPass(llvm::VectorCombinePass());
            FPM.addPass(llvm::LoopSinkPass());
            // LoadStoreVectorizer combines adjacent scalar loads/stores into
            // vector operations.  This is especially beneficial for struct
            // field access patterns and array initialization loops.
            FPM.addPass(llvm::LoadStoreVectorizerPass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
            // CalledValuePropagation propagates known function pointer values
            // through indirect calls, enabling devirtualization and inlining.
            MPM.addPass(llvm::CalledValuePropagationPass());
            // ConstantMerge deduplicates identical global constants across the
            // module (e.g. duplicate string literals, floating-point constants,
            // constant arrays).  Fewer unique constants means smaller data
            // sections, better D-cache utilization, and reduced link time.
            MPM.addPass(llvm::ConstantMergePass());
        });
    }

    // ── Loop Re-Optimization Pass ──────────────────────────────────
    // After the main pipeline (inlining, fusion, distribution, partial
    // inlining), many new optimization opportunities are exposed:
    //   - Inlined code may contain loops with invariant computations
    //   - Fused loops may have new redundancies
    //   - Constant propagation may have simplified loop bounds
    //
    // Re-running LICM + InstCombine + loop simplification catches
    // these late opportunities that the first pass couldn't see.
    // This is an architectural pattern used by production compilers (GCC's
    // -ftree-loop-optimize-2, LLVM's LTO pipeline) but not typically
    // available in language-level compilers.
    //
    // Only enabled at O3 to avoid any stability risk at lower opt levels.
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
    // loop vectorizer has transformed loops, the CFG often contains new
    // scalar-epilogue paths, predicated blocks, and vector-select patterns
    // that can be merged into larger basic blocks.  This improves the
    // backend's instruction scheduling and reduces branch overhead in
    // vectorized code.
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
    // parameters.  After inlining and constant propagation, many functions
    // have parameters that are never used in the function body.  Removing
    // these eliminates register pressure from passing dead values and
    // enables further inlining (smaller call signatures).
    //
    // IPSCCPPass is lowered to O2+ (from O3) because OmScript compiles as a
    // single translation unit — the full call graph is always available, making
    // interprocedural constant propagation cheap and highly effective.  At O2,
    // we enable function specialization (AllowFuncSpec=true by default) so that
    // functions called with constant arguments get specialized clones with the
    // constants folded in.  This is especially beneficial for OmScript programs
    // that use helper functions dispatched from a central switch/case.
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool isO3 = optimizationLevel >= OptimizationLevel::O3;
        PB.registerOptimizerLastEPCallback(
            [isO3](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            // IPSCCPPass performs inter-procedural sparse conditional constant
            // propagation — a more powerful version of SCCP that propagates
            // constants, value ranges, and struct field values across function
            // boundaries through the call graph.  This catches constant-folding
            // opportunities that function-local SCCP misses, such as functions
            // always called with a specific constant argument.
            MPM.addPass(llvm::IPSCCPPass());
            MPM.addPass(llvm::DeadArgumentEliminationPass());
            // PartialInlinerPass outlines cold regions (error handling, slow
            // paths) from otherwise-hot functions and inlines only the hot
            // entry region into callers.  This gives the performance benefit
            // of inlining (no call overhead on the fast path) without the
            // code-size cost of inlining the entire function body.
            // Promoted from O3 to O2: OmScript functions commonly have
            // bounds-check error paths that are cold; partial inlining moves
            // only the hot fast-path into callers.
            MPM.addPass(llvm::PartialInlinerPass());

            // ── Second IPO constant-propagation round (O3 only) ────────────
            // After inlining + PartialInliner, new call sites with constant
            // arguments become visible.  A second IPSCCP pass propagates these
            // newly-exposed constants inter-procedurally and folds them.  The
            // subsequent function-level SCCP + InstCombine + GVN clean up the
            // folded constants within each function, and a final DeadArgElim
            // removes any parameters that became dead after specialization.
            // This replicates the "round 2 IPO" strategy used in production
            // compilers (GCC -O3's second IPCP pass, Clang ThinLTO pipeline).
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
                // function attribute inference in both directions.  IPSCCP
                // may have specialized functions (making callees smaller /
                // simpler), enabling new readnone/readonly proofs that
                // weren't visible earlier.  Running PostOrder (bottom-up)
                // first propagates leaf-function purity upward, then
                // ReversePostOrder (top-down) propagates constraints from
                // the call sites that IPSCCP simplified.
                MPM.addPass(llvm::createModuleToPostOrderCGSCCPassAdaptor(
                    llvm::PostOrderFunctionAttrsPass()));
                MPM.addPass(llvm::ReversePostOrderFunctionAttrsPass());
                // InferFunctionAttrsPass re-annotates library functions whose
                // attributes may have been stripped by earlier passes (e.g.
                // MergedLoadStoreMotion can strip memory attributes on
                // library calls).  Re-running it is cheap and ensures we
                // don't lose malloc/free/memcpy noalias annotations.
                MPM.addPass(llvm::InferFunctionAttrsPass());
            }

            // EliminateAvailableExternallyPass removes bodies of externally-
            // available functions that are no longer needed after IPO.  After
            // inlining and GlobalOpt have processed all call sites, many
            // available_externally definitions exist only as "dead weight".
            // Removing them shrinks the module, improves link time, and gives
            // the linker a cleaner symbol table.
            MPM.addPass(llvm::EliminateAvailableExternallyPass());
        });
    }

    // At O3, add a module-level AttributorPass after the main IPSCCP/inliner
    // rounds.  AttributorCGSCCPass (already registered above) processes one
    // SCC at a time during inlining, but AttributorPass operates on the entire
    // module at once.  This module-wide pass can propagate noalias, nofree,
    // nosync, readnone, and nocapture across SCCs that are disconnected in the
    // call graph — e.g. it can mark a function as "readnone" even if it calls
    // another readnone function in a different SCC.  This is especially useful
    // for OmScript programs where math helpers call other math helpers.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            MPM.addPass(llvm::AttributorPass());
            // After Attributor propagates new attributes (e.g. marks functions
            // as readnone), re-run LICM to hoist loads and function calls out
            // of loops that Attributor proved are loop-invariant.
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
    // callback BEFORE HotColdSplitting.  The vectorizer (which runs earlier
    // in the pipeline) creates vector srem instructions from the original
    // scalar urem/srem.  At this point the loop counter PHI nodes still
    // carry non-negativity information (nsw/nuw flags, assume intrinsics).
    // After HotColdSplitting, the loop code may be outlined into a separate
    // function where the loop counters become parameters without non-negativity
    // metadata, making the srem→urem proof impossible.
    // Promoted from superopt-only to unconditional at O2+: srem→urem is
    // always safe when the operands are provably non-negative, and the
    // conversion saves ~10 instructions per modulo operation (no sign
    // correction needed).
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
    // These three concerns share one OptimizerLastEP callback so that the order
    // contract is enforced by construction: Rule 4 and Rule 5 must see the full
    // loop structure before HotColdSplitting outlines cold blocks into new
    // functions that would hide or invalidate the loop metadata.
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool isO3last = optimizationLevel >= OptimizationLevel::O3;
        PB.registerOptimizerLastEPCallback(
            [isO3last](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/ OM_MODULE_EP_EXTRA_PARAM) {
            // Rule 4: annotate over-large loop bodies for µop-cache partitioning.
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(OpcacheLoopPartitionPass()));
            // Rule 5: promote large streaming-write loops to non-temporal stores.
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(LargeArrayStreamingStorePass()));
            // HotColdSplitting outlines cold code regions (error handlers, assertion
            // failures, rarely-taken branches) into separate functions.  This improves
            // I-cache density on the hot path and enables better register allocation
            // and instruction scheduling in the remaining hot code.
            MPM.addPass(llvm::HotColdSplittingPass());
            // StripDeadPrototypes removes unused external function declarations.
            // After inlining, dead argument elimination, and DCE, many declared-
            // but-never-called prototypes remain.  Removing them reduces symbol
            // table pressure and link time.
            MPM.addPass(llvm::StripDeadPrototypesPass());
            // GlobalSplit splits internal globals with multiple fields into
            // separate globals, enabling more precise alias analysis and
            // allowing SROA/mem2reg to promote individual fields to registers.
            MPM.addPass(llvm::GlobalSplitPass());
            if (isO3last) {
                // MergeFunctions deduplicates identical function bodies, reducing
                // code size and I-cache pressure.  OmScript's monomorphization of
                // generic functions often produces identical machine code for
                // different type instantiations.
                MPM.addPass(llvm::MergeFunctionsPass());
            }
            // AlwaysInlinerPass at the very end: PartialInlinerPass can create
            // new alwaysinline wrapper stubs for hot-path copies; inlining them
            // here means the code-generation stage sees fully inlined call graphs.
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
        // passes and defers heavy IPO (inlining, IPSCCP, GlobalDCE) to the
        // linker.  This avoids double-optimizing the bitcode — once here and
        // again during the link-time optimization pass in the linker.
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
    // @optmax functions marked alwaysinline (set in optimizeOptMaxFunctions)
    // are force-inlined at their call sites before the main pipeline's inliner
    // runs.  The main inliner skips alwaysinline functions when their body is
    // larger than the cost threshold; AlwaysInlinerPass does unconditional
    // inlining regardless of size, matching the user's explicit intent.
    if (optimizationLevel >= OptimizationLevel::O1 && !optMaxFunctions.empty()) {
        MPM.addPass(llvm::AlwaysInlinerPass());
    }

    // At O2+, append GlobalOptPass after the standard pipeline to constant-fold
    // and internalize global variables, propagate initial values, and eliminate
    // globals that are only stored but never read.  This cleans up patterns the
    // default pipeline leaves behind (e.g. globals used only in main).
    // Also run in LTO mode: single-TU programs benefit from GlobalOpt even
    // when the LTO pre-link pipeline is used, since the linker's LTO pass
    // may not run GlobalOpt on every module.
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
    // loop vectorizer (which runs as part of the pipeline) sees urem instead
    // of srem.  The vectorizer's cost model treats urem-by-constant as cheaper
    // than srem-by-constant (urem avoids the signed-correction fixup), enabling
    // vectorization of inner loops like `sum += ((i^j) + k) % 37`.
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        for (auto& func : *module) {
            superopt::convertSRemToURem(func);
            superopt::convertSDivToUDiv(func);
        }
    }
    // Pre-pipeline HGOE loop annotation: set target-optimal unroll count,
    // interleave count, and vector width on loops BEFORE the LLVM pipeline
    // runs.  This ensures the unroller and vectorizer respect the target
    // CPU's resource constraints (register count, I-cache size, pipeline
    // depth) instead of using generic heuristics that over-unroll for CPUs
    // with expensive modulo/division sequences.
    //
    // NOTE: Disabled because LLVM's O3 pipeline already has excellent
    // unroll heuristics with register pressure modeling.  Our pre-pipeline
    // metadata was being overridden by LLVM's aggressive multi-pass
    // unrolling anyway, and when it wasn't overridden, it often led to
    // suboptimal results because the cost model runs on pre-lowered IR
    // where operations like srem/sdiv appear as single instructions but
    // expand to 5-6 µops during selection.  The post-pipeline HGOE
    // (softwarePipelineLoops) runs on fully-optimized IR and gives more
    // accurate annotations for loops that escape LLVM's unroller.
    MPM.run(*module, MAM);
    if (verbose_) {
        std::cout << "    LLVM pass pipeline complete" << '\n';
    }

    // Strip `cold` and `minsize` attributes from user-defined functions.
    // LLVM's HotColdSplitting pass runs at O2+ and can misidentify user
    // functions as cold when there is no PGO profile data, leading to
    // `minsize` codegen that causes register spills, disables unrolling, and
    // in the worst case produces incorrect code when combined with `nofree`
    // inference (e.g., LLVM incorrectly infers that a function with a local
    // heap-allocated array is `nofree` after inlining, then misoptimizes the
    // free() call at the end of the function, resulting in runtime crashes).
    // The `cold` and `minsize` attributes are only appropriate for OS runtime
    // helpers like exit() and abort() — not for user computation functions.
    // Stripping these attributes after the pipeline restores full-speed
    // codegen.  Functions explicitly annotated with @cold by the user are
    // preserved so that intentional cold annotations remain respected.
    if (optimizationLevel >= OptimizationLevel::O2) {
        for (auto& F : *module) {
            if (F.isDeclaration()) continue;
            if (userAnnotatedColdFunctions_.count(F.getName())) continue; // preserve @cold
            F.removeFnAttr(llvm::Attribute::Cold);
            F.removeFnAttr(llvm::Attribute::MinSize);
        }
    }

    // Strip `nofree` from any non-declaration function that directly or
    // indirectly calls `free` (or any `allockind("free")` function).
    // LLVM's attribute inference can incorrectly add `nofree` to functions
    // after inlining: when a hot function with a local heap-allocated array
    // is inlined into a cold caller, the combined function's memory-freeing
    // `invalidate` (= free()) call may be missed by `InferFunctionAttrs`,
    // leaving a stale `nofree` attribute that causes misoptimizations such
    // as load-hoisting across free() and use-after-free at runtime.
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
    // All post-main-pipeline cleanup passes share one PassBuilder and four
    // analysis managers.  This eliminates 4-5 redundant PassBuilder/TargetMachine
    // setups and allows LLVM's deferred analysis invalidation to correctly track
    // what was changed between cleanup stages, avoiding redundant recomputation.
    //
    // PTOPost enables vectorization so that TargetTransformInfo, LoopVectorize,
    // and SLP analysis infrastructure are registered in PostFAM/PostLAM.  The
    // PTO flags only affect analysis registration here (not pass insertion),
    // since we manually add passes to each FPM rather than using
    // buildPerModuleDefaultPipeline.
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
    // Returns immediately if funcs is empty.  Uses a FilteredFuncPass module
    // wrapper so that the FunctionAnalysisManagerModuleProxy is correctly set up
    // (required for per-function analysis access like TargetIRAnalysis).
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
    // by manually inlining self-recursive calls a fixed number of levels.
    // LLVM's inliner refuses to inline self-recursive calls (to prevent
    // infinite expansion), so after the standard pipeline has converted one
    // branch to a tail-call loop, we manually inline the remaining recursive
    // call.  Each level doubles the work done per actual function call,
    // reducing call overhead by ~2^depth.  6 levels gives ~64x fewer calls
    // (2^6 = 64), providing deep unrolling for naive recursive algorithms
    // like fib.
    if (optimizationLevel >= OptimizationLevel::O3) {
        static constexpr unsigned kRecursiveInlineDepth = 6;
        // Conservative size limit: the function BEFORE inlining must be
        // small enough that inlining won't create a huge function.
        // After inlining, each copy roughly doubles the size. We limit
        // the pre-inline size to 350 instructions to cap the resulting
        // function at ~350 * 2^6 = ~22,400 instructions at depth 6.
        // In practice the growth is sub-exponential because LLVM's DCE
        // eliminates dead branches after each inlining level.
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
                // exponential blowup (fib has two calls: fib(n-1)+fib(n-2),
                // but LLVM's TCO eliminates one, leaving exactly one).
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
        // NewGVN + SCCP + CVP are significantly more powerful than the old
        // legacy GVN for catching redundancies after recursive inlining:
        // - SCCP propagates constants through inlined phi nodes
        // - CVP sharpens value ranges from duplicated branch conditions
        // - NewGVN catches memory-SSA-backed redundant loads the old GVN misses
        // - DSE removes stores whose values are dead after inlining merges paths
        if (!inlinedFuncs.empty()) {
            llvm::FunctionPassManager PostInlineFPM;
            PostInlineFPM.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
            PostInlineFPM.addPass(llvm::SCCPPass());
            PostInlineFPM.addPass(llvm::CorrelatedValuePropagationPass());
            PostInlineFPM.addPass(llvm::NewGVNPass());
            PostInlineFPM.addPass(llvm::DSEPass());
            PostInlineFPM.addPass(llvm::InstCombinePass());
            PostInlineFPM.addPass(llvm::ADCEPass());
            PostInlineFPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
            runPostFPMOnFuncs(std::move(PostInlineFPM),
                              {inlinedFuncs.begin(), inlinedFuncs.end()});
        }
    }

    // ── Function specialization for constant arguments ──────────────────
    // When a non-recursive internal function is called with one or more
    // constant arguments, clone it with those constants inlined.  The
    // specialized version enables further constant folding, dead code
    // elimination, and loop optimizations within the clone.
    //
    // This is a compile-time-only transformation: no runtime dispatch.
    // We limit to small functions (≤200 instructions) and at most 4
    // specializations per function to control code-size growth.
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
        // @optmax(specialize=[...]).  When the set is non-empty for a given
        // caller, only those callee names are eligible; all other callees in
        // that caller are skipped.  Callers that have no specialize list (or
        // a non-OPTMAX caller) use the default O3-wide heuristic.
        // key = caller name, value = set of callee names to specialize
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
        // Uses NewGVN + SCCP + CVP + DSE instead of the old legacy GVN pass:
        // - InstSimplify folds constant expressions exposed when constant args
        //   were inlined (e.g. a switch on a constant argument becomes a
        //   direct branch, which SROA/mem2reg can then promote to registers)
        // - SCCP propagates the newly-constant arguments through phi nodes
        // - CVP sharpens value ranges from branch conditions on inlined constants
        // - NewGVN eliminates redundancies visible after constant propagation
        // - DSE removes stores into stack slots that were promoted to constants
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
                SpecFPM.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
                SpecFPM.addPass(llvm::SCCPPass());
                SpecFPM.addPass(llvm::CorrelatedValuePropagationPass());
                SpecFPM.addPass(llvm::NewGVNPass());
                SpecFPM.addPass(llvm::DSEPass());
                SpecFPM.addPass(llvm::InstCombinePass());
                SpecFPM.addPass(llvm::ADCEPass());
                SpecFPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
                runPostFPMOnFuncs(std::move(SpecFPM), std::move(specFuncList));
            }
        }
    }

    // Superoptimizer: run after the standard LLVM pipeline to catch patterns
    // that individual passes miss.  The superoptimizer performs:
    //   - Idiom recognition (rotate, abs, min/max, popcount)
    //   - Algebraic identity simplification on LLVM IR
    //   - Branch-to-select conversion for simple diamond CFGs
    //   - Enumerative synthesis of cheaper instruction sequences
    // Enabled at O2+ unless explicitly disabled with -fno-superopt.
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        if (verbose_) {
            std::cout << "    Running superoptimizer (level " << superoptLevel_
                      << ": idiom recognition, algebraic simplification"
                      << (superoptLevel_ >= 2 ? ", synthesis, branch-opt" : "")
                      << ")..." << '\n';
        }
        superopt::SuperoptimizerConfig superConfig;
        // Configure based on superopt level:
        //   Level 1: idiom + algebraic only (fast compilation)
        //   Level 2: all features, default synthesis (balanced)
        //   Level 3: all features, aggressive synthesis (slower compilation)
        if (superoptLevel_ <= 1) {
            superConfig.enableSynthesis = false;
            superConfig.enableBranchOpt = false;
        } else if (superoptLevel_ >= 3 || optimizationLevel >= OptimizationLevel::O3) {
            superConfig.synthesis.maxInstructions = 5;
            superConfig.synthesis.costThreshold = 0.9;
        }
        // Unified cost model: when a hardware profile is available, wire the
        // hardware-accurate latency data into the superoptimizer so that all
        // cost-driven decisions (synthesis candidate selection, idiom cost
        // comparison, replacement gating) use the same source as the HGOE.
        if (hgoe::shouldActivate(hgoe::HGOEConfig{marchCpu_, mtuneCpu_})) {
            std::string resolvedCpu = marchCpu_.empty() ? mtuneCpu_ : marchCpu_;
            if (resolvedCpu == "native")
                resolvedCpu = llvm::sys::getHostCPUName().str();
            auto profileOpt = hgoe::lookupMicroarch(resolvedCpu);
            if (profileOpt) {
                // Capture profile by value — no lifetime dependency on any graph.
                hgoe::MicroarchProfile capturedProfile = *profileOpt;
                superConfig.costFn = [capturedProfile](const llvm::Instruction* i) {
                    return hgoe::instrCostFromProfile(i, capturedProfile);
                };
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
        // Replaces the old legacy GVN pass with NewGVN + SCCP + CVP + DSE
        // to catch the full range of redundancies the superoptimizer creates:
        // - SCCP folds constants through idiom-replacement phi nodes
        // - CVP sharpens value ranges from simplified branch conditions
        // - NewGVN catches memory-SSA-backed redundancies from idiom rewrites
        // - DSE removes stores made dead by select-merging and algebra folds
        // - MemCpyOpt coalesces adjacent stores the superoptimizer introduced
        if (totalSuperOpts > 0) {
            std::vector<llvm::Function*> smallFuncs;
            for (auto& func : *module) {
                if (!func.isDeclaration() && func.getInstructionCount() <= 2000)
                    smallFuncs.push_back(&func);
            }
            if (!smallFuncs.empty()) {
                llvm::FunctionPassManager PSuperFPM;
                PSuperFPM.addPass(llvm::SCCPPass());
                PSuperFPM.addPass(llvm::CorrelatedValuePropagationPass());
                PSuperFPM.addPass(llvm::NewGVNPass());
                PSuperFPM.addPass(llvm::DSEPass());
                PSuperFPM.addPass(llvm::MemCpyOptPass());
                PSuperFPM.addPass(llvm::InstCombinePass());
                PSuperFPM.addPass(llvm::ADCEPass());
                PSuperFPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
                runPostFPMOnFuncs(std::move(PSuperFPM), std::move(smallFuncs));
            }

            // At O3, run a second superoptimizer pass after the cleanup.
            // The first pass's simplifications (idiom replacement, algebraic
            // folds, select merging) expose new patterns — e.g. a De Morgan
            // transform followed by absorption, or a strength-reduced multiply
            // that becomes a power-of-2 shift the second pass can recognize.
            // The second pass is lighter (no synthesis to avoid compile-time
            // blowup) but picks up the incremental wins from the first pass.
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
            }
        }
    } else if (verbose_ && optimizationLevel >= OptimizationLevel::O2 && !enableSuperopt_) {
        std::cout << "    Superoptimizer disabled (-fno-superopt)" << '\n';
    }

    // Post-pipeline srem→urem and sdiv→udiv conversion.  The pre-pipeline
    // conversion (above) catches srem/sdiv in the original loop body so the
    // vectorizer sees urem/udiv.  However, LLVM's loop unroller creates NEW
    // srem/sdiv instructions in unrolled copies that didn't go through the
    // pre-pipeline conversion.  For example, after 4x unrolling of
    //   sum += ((i^j) + k) % 37
    // the first iteration has urem (converted pre-pipeline) but iterations
    // 2-4 have srem (created by the unroller).  This post-pipeline pass
    // catches those residual srem/sdiv instructions.
    //
    // This is an OmScript-specific advantage: because OmScript's for-loop
    // iterators are immutable within the loop body, we can prove
    // non-negativity of iterator-derived expressions even after unrolling.
    // C compilers cannot make this guarantee because C loop variables can
    // be modified arbitrarily.
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        // Each round first infers nuw flags (exposing more conversion targets),
        // then converts srem/sdiv.  Converges in 2-3 iterations for typical
        // unrolled loops.
        const unsigned postConvCount = runSignedToUnsignedConverge(*module, 3);
        if (verbose_ && postConvCount > 0) {
            std::cout << "    Post-pipeline signed→unsigned: "
                      << postConvCount << " conversions" << '\n';
        }
    }

    // Hardware Graph Optimization Engine: run after the superoptimizer when
    // -march or -mtune is explicitly provided.  The HGOE models the target
    // CPU as a directed graph of execution resources, maps the compiled
    // program onto that graph, and applies hardware-aware transformations
    // (FMA generation, prefetch insertion, branch layout optimisation).
    // When neither flag is set, this is a complete no-op.
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
        // its own loop optimizer and forced unroll/vectorize metadata causes
        // the LTO pipeline to spend excessive time or hang.
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
    // The HGOE may introduce new arithmetic patterns (e.g. from FMA fusion
    // or loop restructuring) that contain srem/sdiv instructions.  Run one
    // more converging round to catch these residuals.
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2
        && enableHGOE_ && !marchCpu_.empty()) {
        const unsigned postHGOECount = runSignedToUnsignedConverge(*module, 2);
        if (verbose_ && postHGOECount > 0) {
            std::cout << "    Post-HGOE signed→unsigned: "
                      << postHGOECount << " conversions" << '\n';
        }
    }

    // -----------------------------------------------------------------------
    // Post-Optimization Prefetch Cleanup
    // -----------------------------------------------------------------------
    // After all major optimizations (constant folding, loop elimination,
    // inlining, superoptimizer, HGOE), remove prefetch intrinsics that are
    // no longer meaningful:
    //   1. Target is a constant or derived from a constant (inttoptr of literal).
    //   2. Target is a stack allocation (alloca) or register-promoted value.
    //   3. The prefetched value has no remaining memory-accessing users.
    //   4. The associated computation was constant-folded or eliminated.
    //   5. Pure functions with no memory side effects need no prefetch.
    // Prefetch-tagged variables go to registers (promoted by SROA/mem2reg)
    // and stay there until invalidated — no special metadata bypass needed.
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
                    // Skip if tagged with !omscript.memory_prefetch — these are
                    // intentional memory-resident prefetches for large types
                    // (structs, arrays) that cannot fit in registers.
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
        // Merges the former legacy cleanupFPM and the former separate new-PM
        // mini-pipeline into one coherent pass sequence, using the shared
        // PostMAM analysis managers.  This eliminates an entire extra pass over
        // all functions and avoids creating a second PassBuilder/TargetMachine.
        //
        // Phase 1: Constant propagation + value-range refinement.
        //   SCCP + CVP run BEFORE NewGVN so that the GVN's memory-SSA value
        //   numbering has accurate range and constant information to work with.
        //   BDCE removes dead bits that SCCP/CVP have proven always-zero, keeping
        //   the IR clean for the vectorizer cost model.
        //
        // Phase 2: Value numbering + memory cleanup.
        //   NewGVN is significantly more powerful than legacy GVN for post-custom-
        //   pass cleanup: it handles memory SSA, congruence classes through phi
        //   nodes, and loads/stores that classical GVN misses.  DSE and MemCpyOpt
        //   run after GVN to remove stores and copies that GVN proved dead.
        //
        // Phase 3: Loop normalization + invariant hoisting.
        //   IndVarSimplify re-canonicalizes IVs modified by HGOE/superoptimizer.
        //   LICM(MemSSA) hoists newly-invariant computations out of loops that
        //   SCCP/CVP/NewGVN just simplified.  IRCE removes bounds checks that
        //   IndVarSimplify proved always-true.  ConstraintElimination handles
        //   remaining relational checks via a system of linear constraints.
        //
        // Phase 4: Strength reduction + final peephole.
        //   AggressiveInstCombine catches multi-instruction patterns (multi-shift
        //   idioms, interleaved arithmetic) that standard InstCombine misses.
        //
        // Phase 5: Vectorization (second pass).
        //   Re-runs SLP after srem→urem + superoptimizer + HGOE because those
        //   transforms expose loops that were non-vectorizable before (e.g. a
        //   modulo-heavy loop where srem blocked vectorization).  This second
        //   vectorization pass is the key OmScript advantage over clang, which
        //   only vectorizes once.  VectorCombine cleans up scalar/vector extract/
        //   insert redundancies the SLP vectorizer introduces.
        //
        // This "closing pass" pattern mirrors GCC -O3's second tree-optimization
        // round and LLVM's ThinLTO pipeline.  The difference is that OmScript's
        // domain-specific custom passes (superoptimizer, HGOE, recursive inlining,
        // function specialization) create optimization opportunities that the
        // standard LLVM pipeline cannot anticipate.
        if (optimizationLevel >= OptimizationLevel::O2) {
            llvm::FunctionPassManager CloseFPM;
            // Phase 1: constant propagation + value-range refinement.
            CloseFPM.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
            CloseFPM.addPass(llvm::SCCPPass());
            CloseFPM.addPass(llvm::CorrelatedValuePropagationPass());
            CloseFPM.addPass(llvm::BDCEPass());
            // Phase 2: value numbering + memory cleanup.
            CloseFPM.addPass(llvm::NewGVNPass());
            CloseFPM.addPass(llvm::DSEPass());
            CloseFPM.addPass(llvm::MemCpyOptPass());
            // MAPR: hoist independent loads above non-aliasing preceding stores
            // to maximise memory-level parallelism in the closing pipeline.
            CloseFPM.addPass(MemoryAccessPhaseReorderPass());
            CloseFPM.addPass(llvm::InstCombinePass());
            // Phase 3: loop normalization + invariant hoisting.
            CloseFPM.addPass(llvm::LoopSimplifyPass());
            CloseFPM.addPass(llvm::LCSSAPass());
            {
                llvm::LoopPassManager LPMClose;
                LPMClose.addPass(llvm::IndVarSimplifyPass());
                LPMClose.addPass(llvm::LICMPass(llvm::LICMOptions()));
                CloseFPM.addPass(llvm::createFunctionToLoopPassAdaptor(
                    std::move(LPMClose), /*UseMemorySSA=*/true));
            }
            CloseFPM.addPass(llvm::IRCEPass());
            CloseFPM.addPass(llvm::ConstraintEliminationPass());
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
        // to multiplicative-inverse (mul/shift/sub) sequences.
        //
        // IMPORTANT: This MUST run AFTER all LLVM optimization passes (including
        // loop unrolling and InstCombine).  Running it pre-pipeline caused a
        // correctness bug: LLVM's loop unroller would create multiple copies of
        // the loop body, and InstCombine would then incorrectly "simplify" the
        // i128 magic-multiply sequence for unrolled copies.  Specifically,
        // LLVM replaced `mulhu(x, magic)` (the HIGH 64 bits of x*magic, computed
        // via i128 arithmetic) with `x * magic mod 2^64` (the LOW 64 bits),
        // producing wrong remainders for every other unrolled iteration.
        //
        // Running here ensures the magic-multiply is introduced AFTER all
        // InstCombine passes have finished, so no further instcombine pass
        // can misidentify and corrupt the i128 mulhu pattern.
        //
        // The srem→urem conversion still runs pre-pipeline (see above) so the
        // loop vectorizer's cost model sees urem-by-constant (cheaper than
        // srem-by-constant in LLVM's cost model) and vectorizes accordingly.
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
    // Replaces the former legacy FunctionPassManager with a new-PM pipeline
    // that uses NewGVN (more powerful than legacy GVN — handles memory-SSA
    // congruence classes and loads through phi nodes), DSEPass (uses MemorySSA
    // for precise dead-store detection), and ADCEPass (aggressive dead-code
    // elimination instead of simple DCE).
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
    // - nounwind: guaranteed no exceptions (enables tail call, smaller EH tables)
    // - optsize is NOT set (we want max speed, not size)
    // alwaysinline threshold: force-inline OPTMAX helpers up to 500 instructions.
    // A higher threshold ensures utility functions like classify(), poly_eval(),
    // gcd(), and Vec2 operator methods are force-inlined, eliminating call
    // overhead in tight loops and enabling the surrounding loop to be
    // vectorized end-to-end without opaque call barriers.
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
        // Higher threshold (30 instrs) ensures utility functions like
        // classify(), add_one/two/four() are force-inlined, eliminating
        // call overhead in tight loops.
        if (func.getInstructionCount() < kAlwaysInlineThreshold
                && !func.hasFnAttribute(llvm::Attribute::NoInline)) {
            func.addFnAttr(llvm::Attribute::AlwaysInline);
        }

        // aggressiveVec=true: hint to the back-end to use the widest available
        // SIMD registers and try harder to vectorize all loops.  We set the
        // "prefer-vector-width" function attribute (same mechanism as clang's
        // -mprefer-vector-width=512) and add loop-vectorize metadata later.
        const std::string nameStr = name.str();
        auto cfgIt = optMaxFunctionConfigs_.find(nameStr);
        if (cfgIt != optMaxFunctionConfigs_.end() && cfgIt->second.aggressiveVec) {
            func.addFnAttr("prefer-vector-width", "512");
            // min-legal-vector-width=0 lets the vectorizer choose width freely.
            func.addFnAttr("min-legal-vector-width", "0");
        }

        // fast_math=true: sweep all existing FP instructions and apply full
        // fast-math flags.  Instructions created during IR building already got
        // flags from the builder context, but instructions introduced by earlier
        // optimization passes (GVN, InstCombine, inliner) may not have them.
        // Applying flags now ensures the OPTMAX FPM and the new-PM vectorizer
        // both see a fully fast-math environment for FP-heavy loops.
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
        // to allow the alias analysis to treat them as non-overlapping.  This
        // enables LICM to hoist loads across calls, DSE to eliminate dead stores
        // to pointer parameters, and the vectorizer to use !noalias metadata on
        // loads/stores inside loops.
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
    //
    // The legacy FunctionPassManager previously used here had two fundamental
    // limitations:
    //   1. The loop vectorizer only exists in the new pass manager.
    //   2. Legacy passes can't use MemorySSA-backed LICM/DSE, which is more
    //      precise and enables more aggressive hoisting.
    //
    // This rewrite builds a single new-PM FunctionPassManager that covers all
    // five optimization phases in a cohesive pipeline with full analysis-manager
    // integration.  The PassBuilder is configured with LoopVectorization=true
    // and SLPVectorization=true so the built-in vectorization cost models are
    // active and target-aware.
    //
    // The FPM is run up to optMaxIterations times per function — the first pass
    // does the heavy lifting (unrolling, vectorization), subsequent passes catch
    // patterns newly exposed by those transformations.  The loop exits early if
    // a pass returns PreservedAnalyses::all() (nothing changed).
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
    // SROA/Promote lift memory to SSA registers; EarlyCSE(MemSSA) eliminates
    // redundant loads using MemorySSA; NewGVN is more powerful than classic GVN
    // because it catches equivalences through PHI nodes.  SCCP + CVP tighten
    // value ranges before loops, enabling IndVarSimplify to prove trip counts.
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

    // ── Phase 2: Loop canonicalization and invariant hoisting ─────────────
    // LoopRotate puts the back-edge condition at the bottom (do-while form),
    // which is required for LICM to hoist invariants from the loop header.
    // LICMPass with MemorySSA is more precise than the legacy LICM — it can
    // hoist loads that the legacy version would conservatively skip.
    // SimpleLoopUnswitch creates loop-invariant-condition specialisations.
    // IndVarSimplify canonicalizes induction variables for the vectorizer.
    FPMMax.addPass(llvm::LoopSimplifyPass());
    FPMMax.addPass(llvm::LCSSAPass());
    {
        llvm::LoopPassManager LPM;
        LPM.addPass(llvm::LoopRotatePass());
        LPM.addPass(llvm::LICMPass(llvm::LICMOptions()));
        LPM.addPass(llvm::SimpleLoopUnswitchPass(/*NonTrivial=*/true));
        LPM.addPass(llvm::LoopIdiomRecognizePass());
        LPM.addPass(llvm::IndVarSimplifyPass());
        LPM.addPass(llvm::LoopPredicationPass());
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
    // LoopFlatten collapses nested loops with simple inner trip counts into a
    // single-level loop, reducing loop overhead and widening the vectorizable
    // iteration space.  LoopUnrollAndJam fuses outer-loop iterations with their
    // inner loops to improve data reuse across outer iterations.  LoopUnroll
    // performs standard unrolling (OptLevel=3 = aggressive threshold/factor).
    // ForgetAllSCEVInLoopUnroll=true forces SCEV to recompute trip counts after
    // each unrolling step, giving the next step accurate information.
    FPMMax.addPass(llvm::LoopSimplifyPass());
    FPMMax.addPass(llvm::LCSSAPass());
    {
        llvm::LoopPassManager LPM;
        LPM.addPass(llvm::LoopFlattenPass());
        LPM.addPass(llvm::LoopUnrollAndJamPass(/*OptLevel=*/3));
        FPMMax.addPass(llvm::createFunctionToLoopPassAdaptor(std::move(LPM)));
    }
    // LoopUnrollPass is a function pass in LLVM 18, not a loop pass.
    // OptLevel=3 enables aggressive unrolling.
    // ForgetSCEV=true (ForgetAllSCEVInLoopUnroll) drops SCEV information
    // for the unrolled loop after each unroll step.  This causes the next
    // pass in the FPM (e.g. IndVarSimplify inside subsequent loop passes) to
    // recompute fresh trip-count and IV analysis rather than reusing stale
    // pre-unroll SCEV results — important because the unrolled body can have
    // different induction structure than the original loop.
    FPMMax.addPass(llvm::LoopUnrollPass(
        llvm::LoopUnrollOptions(/*OptLevel=*/3, /*OnlyWhenForced=*/false,
                                /*ForgetSCEV=*/true)));
    // Post-unroll cleanup: NewGVN merges values made equal by unrolled IVs
    // using MemorySSA for more precise memory-dep analysis; CVP sharpens ranges;
    // hyperblock CFG converts exposed branches to selects.
    FPMMax.addPass(llvm::NewGVNPass());
    FPMMax.addPass(llvm::InstCombinePass());
    FPMMax.addPass(llvm::CorrelatedValuePropagationPass());
    FPMMax.addPass(llvm::JumpThreadingPass());
    FPMMax.addPass(llvm::DFAJumpThreadingPass());
    FPMMax.addPass(llvm::SpeculativeExecutionPass());
    FPMMax.addPass(llvm::SimplifyCFGPass(hyperblockCFGOpts()));
    FPMMax.addPass(llvm::InstCombinePass());
    FPMMax.addPass(llvm::ADCEPass());

    // ── Phase 3: Scalar optimizations ────────────────────────────────────
    // Sinking, strength reduction, and miscellaneous scalar passes that improve
    // the quality of the loop body before vectorization.
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
    // MAPR: hoist independent loads above non-aliasing preceding stores to
    // cluster load issues and maximise memory-level parallelism before
    // vectorisation sees the loop body.
    FPMMax.addPass(MemoryAccessPhaseReorderPass());
    FPMMax.addPass(llvm::Float2IntPass());
    FPMMax.addPass(llvm::DivRemPairsPass());
    FPMMax.addPass(llvm::InferAlignmentPass());
    FPMMax.addPass(llvm::MergeICmpsPass());
    FPMMax.addPass(llvm::IRCEPass());
    FPMMax.addPass(llvm::ConstraintEliminationPass());
    FPMMax.addPass(llvm::GuardWideningPass());
    FPMMax.addPass(llvm::SCCPPass());
    FPMMax.addPass(llvm::InstCombinePass());
    // RPAR: rebalance linear associative chains into O(log n) depth balanced
    // trees, reducing the critical path through arithmetic reductions and
    // exposing ILP for the OoO scheduler.
    FPMMax.addPass(TreeHeightReductionPass());
    FPMMax.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
    // BER: convert near-50/50 diamond CFGs to select instructions, removing
    // mispredict flushes from unpredictable branches in hot OPTMAX loops.
    FPMMax.addPass(BranchEntropyReductionPass());
    FPMMax.addPass(llvm::ADCEPass());

    // ── Phase 4: Vectorization ────────────────────────────────────────────
    // This is the primary transformation that the legacy FPM could NOT perform.
    // Before calling the loop vectorizer, re-canonicalize loops with MemorySSA-
    // backed LICM and LoopVersioningLICM (creates a versioned copy with runtime
    // alias checks so LICM can hoist aliased loads) to maximise loop body purity.
    // LoopDistribute splits independent memory-access patterns into separate
    // loops, each of which the vectorizer can handle cleanly.  LoopFuse merges
    // small adjacent loops to amortise loop overhead.
    FPMMax.addPass(llvm::LoopSimplifyPass());
    FPMMax.addPass(llvm::LCSSAPass());
    FPMMax.addPass(llvm::AlignmentFromAssumptionsPass());
    {
        llvm::LoopPassManager LPM;
        LPM.addPass(llvm::IndVarSimplifyPass());
        LPM.addPass(llvm::LoopVersioningLICMPass());
        LPM.addPass(llvm::LICMPass(llvm::LICMOptions()));
        FPMMax.addPass(llvm::createFunctionToLoopPassAdaptor(
            std::move(LPM), /*UseMemorySSA=*/true));
    }
    FPMMax.addPass(llvm::LoopDistributePass());
    FPMMax.addPass(llvm::LoopFusePass());
    // Sharpen value ranges one final time before the cost model runs.
    FPMMax.addPass(llvm::CorrelatedValuePropagationPass());
    FPMMax.addPass(llvm::BDCEPass());
    FPMMax.addPass(llvm::InstCombinePass());
    FPMMax.addPass(llvm::IRCEPass());
    // Loop vectorizer: the central transformation of this phase.
    // InterleaveOnlyWhenForced=false lets the cost model decide interleaving;
    // VectorizeOnlyWhenForced=false lets the cost model decide vectorization.
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
    // After vectorization the CFG and scalar/vector interaction patterns change
    // significantly.  A final round of canonicalization, LICM, GVN, and
    // InstCombine picks up the residual opportunities.
    FPMMax.addPass(llvm::AggressiveInstCombinePass());
    FPMMax.addPass(llvm::InstCombinePass());
    FPMMax.addPass(llvm::LoopSimplifyPass());
    FPMMax.addPass(llvm::LCSSAPass());
    {
        llvm::LoopPassManager LPM;
        LPM.addPass(llvm::LICMPass(llvm::LICMOptions()));
        FPMMax.addPass(llvm::createFunctionToLoopPassAdaptor(
            std::move(LPM), /*UseMemorySSA=*/true));
    }
    FPMMax.addPass(llvm::DSEPass());
    FPMMax.addPass(llvm::MemCpyOptPass());
    FPMMax.addPass(llvm::NewGVNPass());
    FPMMax.addPass(llvm::InstSimplifyPass());
    FPMMax.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
    FPMMax.addPass(llvm::LoopSinkPass());
    FPMMax.addPass(llvm::ADCEPass());

    // ── Run the OPTMAX pipeline on each OPTMAX function ───────────────────
    // Up to optMaxIterations passes for convergence: the first iteration does
    // the heavy lifting (unrolling, vectorization, tree rebalancing); subsequent
    // iterations catch patterns newly exposed by those transforms.
    //
    // Two convergence criteria (either triggers early exit):
    //   (a) PA.areAllPreserved(): the FPM explicitly reports no IR changes.
    //       This fires when every pass in the FPM returns PreservedAnalyses::all(),
    //       which happens occasionally for already-optimal functions.
    //   (b) Instruction-count delta = 0: the function size did not change.
    //       This is a more reliable convergence check because many LLVM passes
    //       conservatively return PreservedAnalyses::none() even when no IR
    //       was actually modified (to avoid expensive analysis re-validation).
    //       Comparing instruction counts catches this case at the cost of one
    //       extra getInstructionCount() call per iteration.
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
