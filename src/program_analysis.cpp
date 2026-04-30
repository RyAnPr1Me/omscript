/// @file program_analysis.cpp
/// @brief Implementation of the centralized Program Facts Snapshot module.
///
/// See program_analysis.h for the design overview.
///
/// Implementation strategy
/// =======================
///
///   Pass 1 — attribute scan (O(F), cheap):
///     Walk all functions and read their LLVM IR attributes directly.
///     No analysis manager queries required for this pass.
///     Populates: basic metrics, memory/effect flags, inlining flags, isPure.
///
///   Pass 2 — call-graph scan (O(I), where I = total instructions):
///     Walk all call-site instructions to build:
///       - per-function caller/callee degree
///       - self-recursion flag
///       - const-arg call-site counts (as both caller and callee)
///       - reachability set (via BFS from entry points)
///
///   Pass 3 — loop structure (O(F) queries into FAM, lazy):
///     For each defined non-declaration function, query LoopInfo from the
///     FunctionAnalysisManagerModuleProxy.  Uses the pre-existing analysis
///     managers in MAM so no new PassBuilder is needed.
///
/// The three passes are consolidated into a single forward walk over the
/// module to keep instruction-cache pressure low.

#include "program_analysis.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace omscript {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helper: derive the maximum loop depth by recursively walking the loop tree.
// ─────────────────────────────────────────────────────────────────────────────

static unsigned maxDepth(const llvm::Loop* L, unsigned currentDepth) noexcept {
    unsigned best = currentDepth;
    for (const llvm::Loop* sub : L->getSubLoops())
        best = std::max(best, maxDepth(sub, currentDepth + 1));
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: check whether every argument of a call is an LLVM Constant.
// ─────────────────────────────────────────────────────────────────────────────

static bool allArgsConst(const llvm::CallBase* CB) noexcept {
    if (CB->arg_empty()) return false;
    for (unsigned i = 0; i < CB->arg_size(); ++i)
        if (!llvm::isa<llvm::Constant>(CB->getArgOperand(i))) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: collect all module-level entry points.
// An "entry point" is any externally-visible non-declaration function or any
// function whose name is "main".
// ─────────────────────────────────────────────────────────────────────────────

static std::unordered_set<const llvm::Function*>
collectEntryPoints(const llvm::Module& M) {
    std::unordered_set<const llvm::Function*> entries;
    for (const llvm::Function& F : M) {
        if (F.isDeclaration()) continue;
        if (!F.hasLocalLinkage() || F.getName() == "main")
            entries.insert(&F);
    }
    return entries;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: BFS reachability from entry points over the direct call graph.
// Returns the set of reachable functions.
// ─────────────────────────────────────────────────────────────────────────────

static std::unordered_set<const llvm::Function*>
computeReachable(const llvm::Module& M) {
    const auto entries = collectEntryPoints(M);
    std::unordered_set<const llvm::Function*> visited;
    std::deque<const llvm::Function*> worklist(entries.begin(), entries.end());
    visited.insert(entries.begin(), entries.end());

    while (!worklist.empty()) {
        const llvm::Function* cur = worklist.front();
        worklist.pop_front();

        for (const llvm::BasicBlock& BB : *cur) {
            for (const llvm::Instruction& I : BB) {
                const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                if (!CB) continue;
                const llvm::Function* callee = CB->getCalledFunction();
                if (!callee || callee->isDeclaration()) continue;
                if (visited.insert(callee).second)
                    worklist.push_back(callee);
            }
        }
    }
    return visited;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// computeProgramFacts — public API
// ─────────────────────────────────────────────────────────────────────────────

ProgramFactsSnapshot computeProgramFacts(llvm::Module& M,
                                         llvm::ModuleAnalysisManager& MAM,
                                         unsigned wave)
{
    ProgramFactsSnapshot snapshot;
    snapshot.wave = wave;

    // ── Pass 0: BFS reachability (once per snapshot) ──────────────────────
    const auto reachable = computeReachable(M);

    // ── Pass 1+2+3: single forward walk over all functions ────────────────
    //
    // We collect call-graph information on the fly in the same pass as the
    // attribute scan so the instruction stream is visited only once.

    // Temporary per-function callee sets (for cardinality, not full sets).
    std::unordered_map<const llvm::Function*, std::unordered_set<const llvm::Function*>> calleeMap;
    std::unordered_map<const llvm::Function*, std::unordered_set<const llvm::Function*>> callerMap;

    // Obtain the FunctionAnalysisManager proxy so we can query LoopInfo.
    // The proxy is registered by PassBuilder::crossRegisterProxies; querying
    // it for the first time does not re-run any analysis.
    auto* fampProxy = MAM.getCachedResult<llvm::FunctionAnalysisManagerModuleProxy>(M);

    for (llvm::Function& F : M) {
        FunctionSnapshot fs;
        fs.name          = F.getName().str();
        fs.isDeclaration = F.isDeclaration();

        snapshot.totalFunctions++;

        if (!F.isDeclaration()) {
            // ── Basic metrics ─────────────────────────────────────────────
            for (const llvm::BasicBlock& BB : F) {
                fs.basicBlockCount++;
                fs.instructionCount += static_cast<unsigned>(BB.size());
            }

            snapshot.definedFunctions++;
            snapshot.totalInstructions += fs.instructionCount;
        }

        // ── Memory/effect attributes (read-only, no analysis needed) ─────
        fs.doesNotAccessMemory = F.doesNotAccessMemory();
        fs.onlyReadsMemory     = F.onlyReadsMemory();
        fs.doesNotFreeMemory   = F.hasFnAttribute(llvm::Attribute::NoFree);
        fs.willReturn          = F.hasFnAttribute(llvm::Attribute::WillReturn);
        fs.doesNotThrow        = F.doesNotThrow();
        fs.noSync              = F.hasFnAttribute(llvm::Attribute::NoSync);
        fs.isAlwaysInline      = F.hasFnAttribute(llvm::Attribute::AlwaysInline);
        fs.isNoInline          = F.hasFnAttribute(llvm::Attribute::NoInline);

        // Derived purity: readnone + willreturn + nounwind
        fs.isPure = fs.doesNotAccessMemory && fs.willReturn && fs.doesNotThrow;

        // ── Reachability ──────────────────────────────────────────────────
        fs.isReachable = (reachable.count(&F) != 0);

        if (!F.isDeclaration()) {
            // ── Call-graph + const-arg scan (Pass 2) ──────────────────────
            for (const llvm::BasicBlock& BB : F) {
                for (const llvm::Instruction& I : BB) {
                    const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!CB) continue;
                    llvm::Function* callee = CB->getCalledFunction();
                    if (!callee) continue;

                    // Self-recursion
                    if (callee == &F)
                        fs.hasSelfRecursion = true;

                    // Track unique callees for this caller
                    calleeMap[&F].insert(callee);
                    // Track callers for the callee
                    if (!callee->isDeclaration())
                        callerMap[callee].insert(&F);

                    // Const-arg counting (as caller perspective)
                    if (allArgsConst(CB))
                        fs.constArgCallSitesAsCallerCount++;
                }
            }

            // ── Loop structure (Pass 3 — lazy FAM query) ──────────────────
            // Query LoopInfo only when the FAM proxy is available.
            if (fampProxy) {
                auto& FAM = fampProxy->getManager();
                // getCachedResult returns nullptr if LoopInfo hasn't been
                // computed yet — we avoid triggering fresh analysis here to
                // keep this function non-mutating with respect to the MAM
                // state; any existing cached results are sufficient.
                const auto* LI = FAM.getCachedResult<llvm::LoopAnalysis>(F);
                if (LI) {
                    for (const llvm::Loop* L : *LI) {
                        fs.topLevelLoopCount++;
                        fs.maxLoopDepth = std::max(fs.maxLoopDepth,
                                                   maxDepth(L, 1u));
                    }
                }
            }
        }

        snapshot.functions.emplace(fs.name, std::move(fs));
    }

    // ── Post-walk: fill in cross-function counts ──────────────────────────
    for (auto& [fn, fs] : snapshot.functions) {
        const llvm::Function* Fptr = M.getFunction(fn);
        if (!Fptr) continue;

        // Callee count (distinct callees from this function)
        if (auto it = calleeMap.find(Fptr); it != calleeMap.end())
            fs.directCalleeCount = static_cast<unsigned>(it->second.size());

        // Caller count (distinct callers of this function)
        if (auto it = callerMap.find(Fptr); it != callerMap.end())
            fs.directCallerCount = static_cast<unsigned>(it->second.size());

        // Const-arg as callee: count how many callers pass all-const args
        // We walk the users of the function to get the callee-perspective count.
        if (!Fptr->isDeclaration()) {
            unsigned calleeConstCount = 0;
            for (const llvm::User* U : Fptr->users()) {
                const auto* CB = llvm::dyn_cast<llvm::CallBase>(U);
                if (CB && CB->getCalledFunction() == Fptr && allArgsConst(CB))
                    calleeConstCount++;
            }
            fs.constArgCallSitesAsCalleeCount = calleeConstCount;
        }
    }

    // ── Aggregate module-level metrics and classified sets ────────────────
    for (const auto& [name, fs] : snapshot.functions) {
        if (fs.isReachable && !fs.isDeclaration)
            snapshot.reachableFunctions++;

        if (!fs.isDeclaration && !fs.isReachable)
            snapshot.unreachableFunctions.insert(name);

        if (fs.isPure && !fs.isDeclaration)
            snapshot.pureFunctions.insert(name);

        snapshot.totalConstArgCallSites += fs.constArgCallSitesAsCalleeCount;

        // Specialization candidates: ≥2 callers AND ≥1 all-const call site
        if (!fs.isDeclaration && fs.directCallerCount >= 2 &&
            fs.constArgCallSitesAsCalleeCount >= 1)
            snapshot.specializationCandidates.insert(name);
    }

    return snapshot;
}

} // namespace omscript
