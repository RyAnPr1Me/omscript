// ─────────────────────────────────────────────────────────────────────────────
// ipof_pass.cpp — Implicit Phase Ordering Fixer (IPOF)
// ─────────────────────────────────────────────────────────────────────────────
//
// See ipof_pass.h for the full algorithm description.

#include "ipof_pass.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/OptimizationRemarkEmitter.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>
#include <llvm/Transforms/Scalar/MemCpyOptimizer.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Vectorize/SLPVectorizer.h>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace omscript::ipof {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

/// FNV-1a 64-bit hash over the opcodes + type IDs of all instructions in @p F.
/// Cheap enough to run before/after every local re-run as a cycle detector.
static uint64_t hashFunction(const llvm::Function& F) noexcept {
    constexpr uint64_t kFNVBasis = 14695981039346656037ULL;
    constexpr uint64_t kFNVPrime = 1099511628211ULL;
    uint64_t h = kFNVBasis;
    for (const auto& BB : F) {
        for (const auto& I : BB) {
            // Fold in opcode.
            h ^= static_cast<uint64_t>(I.getOpcode());
            h *= kFNVPrime;
            // Fold in type ID as a proxy for operand types.
            h ^= static_cast<uint64_t>(I.getType()->getTypeID());
            h *= kFNVPrime;
            // Fold in operand count to distinguish structurally similar insts.
            h ^= static_cast<uint64_t>(I.getNumOperands());
            h *= kFNVPrime;
        }
    }
    return h;
}

/// Count all non-declaration instructions in @p F.
static unsigned instrCount(const llvm::Function& F) noexcept {
    unsigned n = 0;
    for (const auto& BB : F) n += static_cast<unsigned>(BB.size());
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 1 — Missed opportunity detection
// ─────────────────────────────────────────────────────────────────────────────

struct MissedOp {
    MissedOpKind     kind;
    llvm::Function*  fn;          ///< Owning function
    llvm::Instruction* site;      ///< Representative instruction
};

/// Check whether @p I is a binary/unary op on IR constants that wasn't folded.
static bool isUnfoldedConstantExpr(const llvm::Instruction* I) noexcept {
    if (!llvm::isa<llvm::BinaryOperator>(I) && !llvm::isa<llvm::UnaryOperator>(I))
        return false;
    for (unsigned i = 0; i < I->getNumOperands(); ++i)
        if (!llvm::isa<llvm::Constant>(I->getOperand(i))) return false;
    return true;
}

/// Check whether @p I is a load whose pointer was already loaded/stored
/// earlier in the same basic block without an intervening store.
static bool isRedundantLoad(const llvm::LoadInst* LD) noexcept {
    const llvm::Value* ptr = LD->getPointerOperand();
    const llvm::BasicBlock* BB = LD->getParent();
    // Walk backwards from LD within its BB.
    for (auto it = BB->begin(), end = BB->end(); it != end; ++it) {
        const llvm::Instruction* cur = &*it;
        if (cur == LD) break;
        if (auto* prevLD = llvm::dyn_cast<llvm::LoadInst>(cur))
            if (prevLD->getPointerOperand() == ptr) return true;
        if (auto* prevST = llvm::dyn_cast<llvm::StoreInst>(cur))
            if (prevST->getPointerOperand() == ptr) return false;
    }
    return false;
}

/// True when @p call has all-constant arguments and is a direct call to a
/// function with a body (inlinable).
static bool isConstArgCall(const llvm::CallInst* call) noexcept {
    const llvm::Function* callee = call->getCalledFunction();
    if (!callee || callee->isDeclaration()) return false;
    if (call->arg_empty()) return false; // Zero-arg calls are already handled by CFCTRE.
    for (const llvm::Value* arg : call->args())
        if (!llvm::isa<llvm::Constant>(arg)) return false;
    return true;
}

static std::vector<MissedOp> step1Detect(llvm::Function& F, const IpofConfig& cfg) {
    std::vector<MissedOp> ops;

    // Skip oversized functions to bound compile time.
    if (instrCount(F) > cfg.maxFunctionSize) return ops;

    // CSE detection: map (opcode, type, operands) → first occurrence.
    using ExprKey = std::pair<unsigned, llvm::Value*>; // (opcode, op0) as cheap key
    std::unordered_map<size_t, llvm::Instruction*> exprSeen;

    for (auto& BB : F) {
        for (auto& I : BB) {
            // ── ConstantFolding ─────────────────────────────────────────
            if (isUnfoldedConstantExpr(&I)) {
                ops.push_back({MissedOpKind::ConstantFolding, &F, &I});
                continue;
            }

            // ── DeadCode ────────────────────────────────────────────────
            if (!I.isTerminator() && !I.mayHaveSideEffects() && I.use_empty()) {
                ops.push_back({MissedOpKind::DeadCode, &F, &I});
                continue;
            }

            // ── RedundantLoad ───────────────────────────────────────────
            if (auto* LD = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                if (isRedundantLoad(LD))
                    ops.push_back({MissedOpKind::RedundantLoad, &F, &I});
            }

            // ── CallWithConst ───────────────────────────────────────────
            if (cfg.enableCallWithConst) {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                    if (isConstArgCall(call))
                        ops.push_back({MissedOpKind::CallWithConst, &F, &I});
                }
            }

            // ── CommonSubexpr (level ≥ 2) ────────────────────────────────
            // Cheap structural hash: combine opcode + num-operands + op0 pointer.
            if (cfg.aggressionLevel >= 2 && !I.isTerminator() &&
                !llvm::isa<llvm::PHINode>(I) && !I.mayHaveSideEffects()) {
                size_t key = static_cast<size_t>(I.getOpcode());
                key ^= (I.getNumOperands() * 2654435761ULL);
                if (I.getNumOperands() > 0)
                    key ^= reinterpret_cast<size_t>(I.getOperand(0)) * 40503ULL;
                auto [it, inserted] = exprSeen.try_emplace(key, &I);
                if (!inserted && it->second != &I) {
                    // Same structural key seen before — candidate CSE.
                    ops.push_back({MissedOpKind::CommonSubexpr, &F, &I});
                }
            }
        }
    }

    return ops;
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 2 — Dependency-hint graph
// ─────────────────────────────────────────────────────────────────────────────
//
// For each group of missed ops in a function, compute the ordered set of
// sub-passes needed to fix them (the "pass sequence").

enum class PassSeq {
    FoldThenDCE,       // InstSimplify + ADCE
    CSEFoldDCE,        // EarlyCSE + InstSimplify + ADCE
    LoadElim,          // MemCpyOpt + InstSimplify
    InlineFoldDCE,     // AlwaysInliner + InstSimplify + ADCE + SimplifyCFG
};

/// Determine the pass sequence for a group of missed ops in one function.
/// Multiple kinds may be present; the sequence covers all of them.
static PassSeq selectPassSeq(const std::vector<MissedOp>& ops) {
    bool hasConst  = false, hasCSE   = false;
    bool hasDead   = false, hasLoad  = false;
    bool hasInline = false;

    for (const auto& op : ops) {
        switch (op.kind) {
            case MissedOpKind::ConstantFolding:  hasConst  = true; break;
            case MissedOpKind::CommonSubexpr:    hasCSE    = true; break;
            case MissedOpKind::DeadCode:         hasDead   = true; break;
            case MissedOpKind::RedundantLoad:    hasLoad   = true; break;
            case MissedOpKind::CallWithConst:    hasInline = true; break;
            default: break;
        }
    }

    if (hasInline) return PassSeq::InlineFoldDCE;
    if (hasLoad)   return PassSeq::LoadElim;
    if (hasCSE)    return PassSeq::CSEFoldDCE;
    return PassSeq::FoldThenDCE; // covers ConstantFolding + DeadCode
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 3 — Local re-run of the selected pass sequence
// ─────────────────────────────────────────────────────────────────────────────

/// Apply @p seq to @p F (function-level) or to the whole @p M (module-level
/// when seq == InlineFoldDCE).  Returns true if any transformation fired.
static bool applyPassSeq(PassSeq seq,
                          llvm::Function& F,
                          llvm::Module& M,
                          llvm::TargetTransformInfo& /*TTI*/) {
    llvm::PassBuilder PB;
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    // The sub-passes used here (InstSimplify, ADCE, EarlyCSE, MemCpyOpt,
    // SimplifyCFG, AlwaysInliner) do not require a hardware-accurate TTI;
    // a default TargetIRAnalysis is sufficient and avoids the move-only
    // TTI copy problem.
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    if (seq == PassSeq::InlineFoldDCE) {
        // Module-level: inliner must run at module scope.
        llvm::ModulePassManager MPM;
        MPM.addPass(llvm::AlwaysInlinerPass());
        llvm::FunctionPassManager FPM;
        FPM.addPass(llvm::InstSimplifyPass());
        FPM.addPass(llvm::ADCEPass());
        FPM.addPass(llvm::SimplifyCFGPass());
        MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
        MPM.run(M, MAM);
        return true; // Conservative: assume change.
    }

    // Function-level sequences.
    llvm::FunctionPassManager FPM;
    switch (seq) {
        case PassSeq::FoldThenDCE:
            FPM.addPass(llvm::InstSimplifyPass());
            FPM.addPass(llvm::ADCEPass());
            break;
        case PassSeq::CSEFoldDCE:
            FPM.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/false));
            FPM.addPass(llvm::InstSimplifyPass());
            FPM.addPass(llvm::ADCEPass());
            break;
        case PassSeq::LoadElim:
            FPM.addPass(llvm::MemCpyOptPass());
            FPM.addPass(llvm::InstSimplifyPass());
            break;
        default:
            break;
    }
    FPM.run(F, FAM);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-function driver
// ─────────────────────────────────────────────────────────────────────────────

static IpofStats runOnFunction(llvm::Function& F,
                                llvm::Module& M,
                                llvm::TargetTransformInfo& TTI,
                                const IpofConfig& cfg) {
    IpofStats stats;
    if (F.isDeclaration()) return stats;

    // Step 1: detect.
    auto ops = step1Detect(F, cfg);
    if (ops.empty()) return stats;
    stats.opportunitiesFound = static_cast<unsigned>(ops.size());
    ++stats.opportunitiesActed;

    // Step 2: select pass sequence.
    const PassSeq seq = selectPassSeq(ops);

    // Step 3+4: iterate, gated by cost and cycle detection.
    std::unordered_set<uint64_t> seenHashes;
    const unsigned cap = std::min(cfg.maxIterations, 4u);

    for (unsigned iter = 0; iter < cap; ++iter) {
        const unsigned before = instrCount(F);
        const uint64_t hashBefore = hashFunction(F);

        if (seenHashes.count(hashBefore)) {
            ++stats.rejectedOscillation;
            break;
        }
        seenHashes.insert(hashBefore);

        // Apply.
        applyPassSeq(seq, F, M, TTI);
        ++stats.rerunsApplied;

        const unsigned after = instrCount(F);
        const int64_t delta  = static_cast<int64_t>(before) - static_cast<int64_t>(after);

        if (delta >= static_cast<int64_t>(cfg.minInstrReduction)) {
            ++stats.acceptedImprovements;
            stats.netInstrReduction += delta;

            // Credit the right per-kind counter.
            switch (seq) {
                case PassSeq::FoldThenDCE:
                    for (const auto& op : ops) {
                        if (op.kind == MissedOpKind::ConstantFolding) ++stats.foldedConstants;
                        if (op.kind == MissedOpKind::DeadCode)         ++stats.eliminatedDead;
                    }
                    break;
                case PassSeq::CSEFoldDCE:
                    ++stats.eliminatedCSE;
                    break;
                case PassSeq::LoadElim:
                    ++stats.eliminatedLoads;
                    break;
                case PassSeq::InlineFoldDCE:
                    ++stats.inlinedAndFolded;
                    break;
            }
        } else {
            ++stats.rejectedNoGain;
            break; // No gain → stop iterating this function.
        }

        // Re-scan for remaining opportunities; break early if none left.
        auto remaining = step1Detect(F, cfg);
        if (remaining.empty()) break;
    }

    return stats;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// runIPOF — public entry point
// ─────────────────────────────────────────────────────────────────────────────

IpofStats runIPOF(llvm::Module& module,
                  const std::function<llvm::TargetTransformInfo(llvm::Function&)>& getTTI,
                  const IpofConfig& cfg) {
    // Apply aggression-level overrides to the effective config.
    IpofConfig eff = cfg;
    eff.aggressionLevel     = std::min(cfg.aggressionLevel, 3u);
    eff.enableCallWithConst = cfg.enableCallWithConst || (cfg.aggressionLevel >= 2);
    eff.enableNearVectorizable =
        cfg.enableNearVectorizable && (cfg.aggressionLevel >= 3);
    eff.maxIterations = std::min(
        cfg.maxIterations,
        cfg.aggressionLevel == 1 ? 1u :
        cfg.aggressionLevel == 2 ? 2u : 3u);

    IpofStats total;

    for (llvm::Function& F : module) {
        if (F.isDeclaration()) continue;
        llvm::TargetTransformInfo TTI = getTTI(F);
        IpofStats fs = runOnFunction(F, module, TTI, eff);
        total.opportunitiesFound   += fs.opportunitiesFound;
        total.opportunitiesActed   += fs.opportunitiesActed;
        total.rerunsApplied        += fs.rerunsApplied;
        total.acceptedImprovements += fs.acceptedImprovements;
        total.rejectedNoGain       += fs.rejectedNoGain;
        total.rejectedOscillation  += fs.rejectedOscillation;
        total.foldedConstants      += fs.foldedConstants;
        total.eliminatedCSE        += fs.eliminatedCSE;
        total.eliminatedDead       += fs.eliminatedDead;
        total.eliminatedLoads      += fs.eliminatedLoads;
        total.inlinedAndFolded     += fs.inlinedAndFolded;
        total.netInstrReduction    += fs.netInstrReduction;
    }

    return total;
}

} // namespace omscript::ipof
