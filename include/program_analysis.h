#pragma once
#ifndef PROGRAM_ANALYSIS_H
#define PROGRAM_ANALYSIS_H

/// @file program_analysis.h
/// @brief Centralized program-wide analysis module for the OmScript optimizer.
///
/// This module implements the "Program Facts Snapshot" — a unified, queryable
/// view of the program that is recomputed once after each optimization wave and
/// consumed by subsequent waves.
///
/// Design principles
/// =================
///
///   1. Pure analysis — no optimization logic.  This module only reads LLVM IR
///      and LLVM analysis results; it never modifies the module.
///
///   2. Centralized — consolidates facts that were previously inferred
///      redundantly in codegen_opt.cpp, ipof_pass.cpp, and hardware_graph.cpp:
///      function purity, side-effect summaries, pointer nullability, const-arg
///      call-site patterns, reachable call-graph structure, and loop structure
///      metadata.
///
///   3. Reuse existing analyses — facts are derived by querying LLVM analyses
///      (LoopInfo, CallGraph) that are already registered in the caller's
///      ModuleAnalysisManager; no extra PassBuilder or analysis manager is
///      constructed here.
///
///   4. Wave-stamped — each snapshot records which optimization wave produced
///      it so callers can detect stale snapshots.
///
/// Usage
/// =====
///
///   // After running an optimization wave:
///   auto snapshot = omscript::computeProgramFacts(*module, PostMAM, waveIndex);
///
///   // Query a specific function:
///   if (auto* fs = snapshot.get("myFunc")) {
///       if (fs->isPure && fs->instructionCount < 50)
///           // cheap pure function — safe to skip expensive passes
///   }
///
/// Integration points in runOptimizationPasses():
///   - After MPM.run (main pipeline wave)
///   - After post-pipeline specialization + ConstArgProp wave
///   - After superoptimizer wave
///   - After HGOE wave
///   - After SDR wave
///   - After IPOF wave (snapshot feeds the closing pipeline)
///
/// Integration points in optimizeOptMaxFunctions():
///   - After PreIPO (feeds per-function OPTMAX pipeline)
///   - After per-function OPTMAX pipeline (feeds PostIPO)

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// LLVM analysis manager — included directly because ModuleAnalysisManager is
// a type alias (not a class), so it cannot be forward-declared as 'class'.
#include <llvm/IR/PassManager.h>

// Forward declaration for Module — reduces include fan-out.
namespace llvm {
class Module;
} // namespace llvm

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// FunctionSnapshot — per-function facts distilled from LLVM IR attributes
// and lightweight analysis queries.
// ─────────────────────────────────────────────────────────────────────────────

struct FunctionSnapshot {
    std::string name;

    // ── Basic metrics ─────────────────────────────────────────────────────
    unsigned instructionCount = 0;  ///< Total instructions (non-declaration functions)
    unsigned basicBlockCount  = 0;  ///< Number of basic blocks
    bool     isDeclaration    = false; ///< True when there is no function body

    // ── Memory/side-effect attributes ────────────────────────────────────
    /// Does not access memory at all (readnone).  Safe for speculative execution
    /// and CSE across arbitrary program points.
    bool doesNotAccessMemory = false;

    /// Only reads memory (readonly).  Safe for hoisting above writes.
    bool onlyReadsMemory     = false;

    /// Does not free heap memory (nofree).  Loads past calls to this function
    /// can be freely reordered by the alias analyzer.
    bool doesNotFreeMemory   = false;

    /// Always returns to its caller (willreturn).  No infinite loops or
    /// non-local exits; enables call speculation.
    bool willReturn          = false;

    /// Does not throw (nounwind).  Eliminates unwind-path overhead.
    bool doesNotThrow        = false;

    /// Has no synchronization (nosync).  Safe for reordering around atomics.
    bool noSync              = false;

    // ── Inlining / specialization potential ──────────────────────────────
    bool isAlwaysInline      = false; ///< Marked alwaysinline
    bool isNoInline          = false; ///< Marked noinline

    // ── Derived purity flag ───────────────────────────────────────────────
    /// Convenience: true iff doesNotAccessMemory && willReturn && doesNotThrow.
    /// Equivalent to LLVM "readnone" + "willreturn" + "nounwind" — the minimum
    /// set of attributes needed to treat a call as a pure value.
    bool isPure = false;

    // ── Call-graph metrics ────────────────────────────────────────────────
    unsigned directCallerCount = 0; ///< Number of distinct call sites that call this function
    unsigned directCalleeCount = 0; ///< Number of distinct functions this function calls
    bool     hasSelfRecursion  = false; ///< Has at least one self-recursive call site

    // ── Loop structure ────────────────────────────────────────────────────
    unsigned topLevelLoopCount = 0; ///< Number of outermost loops in this function
    unsigned maxLoopDepth      = 0; ///< Deepest loop nesting level observed

    // ── Constant-argument call-site patterns ─────────────────────────────
    /// Number of call sites INSIDE this function (as caller) where all
    /// arguments passed to the callee are LLVM constants.  A non-zero count
    /// means ConstArgPropagation / function specialization has more targets
    /// to exploit in this function.
    unsigned constArgCallSitesAsCallerCount = 0;

    /// Number of call sites TARGETING this function (as callee) where ALL
    /// arguments are LLVM constants.  Non-zero means specializing this
    /// function for those constants would create a fully-constant body.
    unsigned constArgCallSitesAsCalleeCount = 0;

    // ── Reachability ──────────────────────────────────────────────────────
    /// True for functions reachable from a module entry point.  False for
    /// internal functions with no remaining callers (candidates for GlobalDCE).
    bool isReachable = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// ProgramFactsSnapshot — module-wide analysis aggregate produced after each
// optimization wave.
// ─────────────────────────────────────────────────────────────────────────────

struct ProgramFactsSnapshot {
    unsigned wave = 0; ///< Which optimization wave produced this snapshot (1-based)

    // ── Per-function map ──────────────────────────────────────────────────
    std::unordered_map<std::string, FunctionSnapshot> functions;

    // ── Module-level metrics ──────────────────────────────────────────────
    unsigned totalFunctions     = 0; ///< All functions (including declarations)
    unsigned definedFunctions   = 0; ///< Non-declaration functions
    unsigned reachableFunctions = 0; ///< Reachable from an entry point
    unsigned totalInstructions  = 0; ///< Sum of instruction counts across all defined functions

    // ── Interprocedural opportunities ────────────────────────────────────
    /// Total const-arg call sites across the module (as callee perspective).
    /// Reflects the total budget available for ConstArgPropagation /
    /// function specialization in the next wave.
    unsigned totalConstArgCallSites = 0;

    // ── Classified function sets ──────────────────────────────────────────
    /// Functions that are pure (readnone + willreturn + nounwind).  The closing
    /// pipeline can skip DSE/MemCpyOpt for these functions.
    std::unordered_set<std::string> pureFunctions;

    /// Functions with ≥2 callers and ≥1 const-arg call site — the highest-value
    /// targets for specialization in the next wave.
    std::unordered_set<std::string> specializationCandidates;

    /// Functions that are unreachable (no callers and not an entry point).
    /// GlobalDCE will remove these; they can be skipped by expensive passes.
    std::unordered_set<std::string> unreachableFunctions;

    // ── Convenience accessor ──────────────────────────────────────────────

    /// Return the snapshot for @p name, or nullptr if unknown.
    const FunctionSnapshot* get(const std::string& name) const noexcept {
        const auto it = functions.find(name);
        return it != functions.end() ? &it->second : nullptr;
    }

    /// Return true iff @p name is known to be a pure function in this snapshot.
    bool isPure(const std::string& name) const noexcept {
        return pureFunctions.count(name) != 0;
    }

    /// Return true iff @p name is a dead (unreachable) function.
    bool isUnreachable(const std::string& name) const noexcept {
        return unreachableFunctions.count(name) != 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// computeProgramFacts — compute a fresh ProgramFactsSnapshot
// ─────────────────────────────────────────────────────────────────────────────
///
/// Queries @p MAM for analyses already registered (LoopInfo via the
/// FunctionAnalysisManagerModuleProxy) and reads function attributes directly.
/// This function never modifies @p M.
///
/// @param M     The module to analyze.
/// @param MAM   A ModuleAnalysisManager with crossRegisteredProxies so that
///              FunctionAnalysisManagerModuleProxy is available.
/// @param wave  The 1-based wave index to stamp into the snapshot.
///
/// @note All LoopInfo queries are lazy: the FAM will recompute them from scratch
///       only for the functions that are actually queried, avoiding the full cost
///       of a module-wide loop analysis rebuild.
ProgramFactsSnapshot computeProgramFacts(llvm::Module& M,
                                         llvm::ModuleAnalysisManager& MAM,
                                         unsigned wave);

} // namespace omscript

#endif // PROGRAM_ANALYSIS_H
