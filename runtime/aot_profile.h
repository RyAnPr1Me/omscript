#ifndef AOT_PROFILE_H
#define AOT_PROFILE_H

// ---------------------------------------------------------------------------
// Hybrid AOT + Tiered Optimizing JIT — Execution Pipeline
// ---------------------------------------------------------------------------
//
// PIPELINE DIAGRAM:
//
//   ┌────────────┐    ┌──────────┐    ┌──────────────────┐    ┌───────────────┐
//   │ Source Code │───>│ Compiler │───>│ Optimized IR     │───>│ Baseline Code │
//   │  (.om)     │    │ Frontend │    │ (AOT, SSA/LLVM)  │    │ (Tier-1, O2)  │
//   └────────────┘    └──────────┘    └──────────────────┘    └───────┬───────┘
//                                              │                      │
//                                              │ (preserved bitcode)  │ execution
//                                              ▼                      ▼
//                                     ┌────────────────┐     ┌────────────────┐
//                                     │ Clean IR Cache │     │    Runtime     │
//                                     │ (bitcode blob) │     │   Profiling   │
//                                     └───────┬────────┘     └───────┬────────┘
//                                             │                      │
//                                             │  ┌───────────────────┘
//                                             │  │ call counts, branch probs,
//                                             │  │ arg types, constants
//                                             ▼  ▼
//                                     ┌────────────────────┐
//                                     │  Hot Function      │
//                                     │  Detected          │
//                                     │ (>= 100 calls)     │
//                                     └────────┬───────────┘
//                                              │
//                                              ▼
//                                     ┌────────────────────┐
//                                     │  Specialized IR    │
//                                     │  - PGO entry count │
//                                     │  - Branch weights  │
//                                     │  - Type hints      │
//                                     └────────┬───────────┘
//                                              │
//                                              ▼
//                                     ┌────────────────────┐
//                                     │  O3 Optimization   │
//                                     │  - Constant fold   │
//                                     │  - Dead code elim  │
//                                     │  - Inlining        │
//                                     │  - Loop unrolling  │
//                                     │  - Vectorization   │
//                                     │  - Type specialize │
//                                     └────────┬───────────┘
//                                              │
//                                              ▼
//                                     ┌────────────────────┐
//                                     │ Optimized Machine  │────> Execution
//                                     │ Code (Tier-2, O3)  │    (hot-patched)
//                                     └────────────────────┘
//                                              │
//                                              │ guard failure?
//                                              ▼
//                                     ┌────────────────────┐
//                                     │  Deoptimization    │───> Baseline Code
//                                     │  (revert to Tier-1)│
//                                     └────────────────────┘
//
// ---------------------------------------------------------------------------
// Adaptive JIT Runner — the dynamic execution model for `omsc run`
// ---------------------------------------------------------------------------
// When the user runs `omsc run file.om`, instead of compiling to a binary
// and spawning a subprocess, the AdaptiveJITRunner executes the program
// in-process using a two-tier JIT strategy:
//
//   Tier 1 — Initial JIT (fast):
//     The module's clean IR is serialised to bitcode, then a fresh copy is
//     loaded, call-counting dispatch prologs are injected into every
//     non-main function, and the result is JIT-compiled at O2 via LLVM
//     MCJIT.  Execution begins immediately.
//
//   Runtime Profiling (during Tier 1):
//     Each non-main function collects runtime data:
//     - Call frequency: atomic counter incremented at each invocation.
//     - Argument types: each parameter's type tag is recorded via
//       __omsc_profile_arg() — feeds into type specialization.
//     - Branch probabilities: conditional branch taken/not-taken counts
//       recorded via __omsc_profile_branch() (reserved for future use).
//     - Observed constants: frequently-passed constant values tracked
//       for constant folding during recompilation.
//     Overhead is minimal: one atomic increment + one callback per arg
//     per call during the warm-up phase only.
//
//   Tier 2 — Adaptive recompile (hot):
//     Every function counts its own invocations via an atomicrmw at entry.
//     When the count first reaches kRecompileThreshold the function calls
//     __omsc_adaptive_recompile(), which:
//       1. Parses the clean (counter-free) bitcode into a fresh context.
//       2. Annotates the target function with setEntryCount(callCount) —
//          this is the PGO hint that shifts the LLVM optimizer from static
//          heuristics to real runtime data.
//       3. Applies collected branch weights from the JITProfiler to
//          conditional branches as LLVM metadata.
//       4. Re-runs the full LLVM O3 pipeline with those annotations:
//          the inliner, branch-layout pass, loop vectorizer, and unroller
//          all see the function as hot and optimize accordingly.
//       5. JIT-compiles the result via MCJIT.
//       6. Writes the new native function pointer into the function's
//          @__omsc_fn_<name> slot.
//
//   Deoptimization:
//     Specialized code may include guard checks (e.g. type assertions).
//     If a guard fails at runtime, __omsc_deopt_guard_fail() increments
//     a failure counter.  After kDeoptThreshold failures, the hot-patch
//     slot is reset to nullptr — reverting to Tier-1 baseline code.
//
//   Future calls take the fast path at the top of the dispatch prolog:
//     %fp = load volatile ptr, @__omsc_fn_<name>
//     if (%fp != null)  tail call %fp(args)   // already recompiled
//   — one volatile load + a well-predicted branch, then a direct call to
//   the O3-PGO-optimised native code with zero counter overhead.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace llvm {
class ExecutionEngine;
class LLVMContext;
class Module;
} // namespace llvm

namespace omscript {

class AdaptiveJITRunner {
  public:
    /// Invocations before which a function is recompiled at O3 with PGO guidance.
    ///
    /// 100 strikes the right balance for typical short-to-medium OmScript programs:
    ///  - Low enough to capture hot functions early and spend most execution time
    ///    in Tier-2 O3+PGO native code rather than Tier-1 O2 baseline code.
    ///  - High enough to accumulate a statistically useful type and branch profile
    ///    before paying the O3 compilation cost.
    /// For long-running server workloads a higher value (e.g. 1000) gives richer
    /// profile data; for micro-benchmarks this is already optimal.
    static constexpr int64_t kRecompileThreshold = 100;

    AdaptiveJITRunner();
    ~AdaptiveJITRunner();

    AdaptiveJITRunner(const AdaptiveJITRunner&) = delete;
    AdaptiveJITRunner& operator=(const AdaptiveJITRunner&) = delete;

    /// Enable or disable verbose JIT progress messages (default: false).
    void setVerbose(bool v) {
        verbose_ = v;
    }
    bool isVerbose() const {
        return verbose_;
    }

    /// Execute the program defined by @p module in-process.
    /// The module is NOT modified — a working copy is created internally.
    /// Returns the integer value returned by main() (clamped to [0, 255]).
    int run(llvm::Module* module);

    /// Called from __omsc_adaptive_recompile when a function goes hot.
    /// Recompiles @p name from the clean bitcode with O3 + PGO entry-count,
    /// then writes the new function pointer into *fnPtrSlot.
    void onHotFunction(const char* name, int64_t callCount, void** fnPtrSlot);

  private:
    /// Serialised clean IR — preserved from the initial module before any
    /// counter instrumentation is added.  Used as the source for all
    /// Tier-2 recompilations.
    std::vector<char> cleanBitcode_;

    /// Keeps MCJIT ExecutionEngines (and their contexts) alive so compiled
    /// native code pointers remain valid for the process lifetime.
    struct JitModule {
        std::unique_ptr<llvm::LLVMContext> ctx;
        std::unique_ptr<llvm::ExecutionEngine> engine;
    };
    std::vector<JitModule> modules_;

    /// Functions that have already been recompiled (at most once each).
    std::unordered_set<std::string> recompiled_;
    std::mutex recompiledMtx_; ///< Guards recompiled_ and modules_ across threads.
    bool verbose_ = false;     ///< Print JIT recompile events when true.

    void ensureInitialized();

    /// Inject call-counting dispatch prologs into every non-main function
    /// in @p mod.  Adds @__omsc_calls_<name> and @__omsc_fn_<name> globals
    /// and prepends a dispatch basic block to each function.
    void injectCounters(llvm::Module& mod);
};

} // namespace omscript

// ---------------------------------------------------------------------------
// C-linkage callback — invoked directly from compiler-injected LLVM IR
// ---------------------------------------------------------------------------
extern "C" {
/// Trigger Tier-2 recompilation of @p name.
/// @p callCount is the observed call count (used as the PGO entry-count hint).
/// @p fnPtrSlot points to the @__omsc_fn_<name> global in the JIT binary;
/// the recompiler stores the new native pointer there on success.
void __omsc_adaptive_recompile(const char* name, int64_t callCount, void** fnPtrSlot);
} // extern "C"

#endif // AOT_PROFILE_H
