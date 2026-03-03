#ifndef AOT_PROFILE_H
#define AOT_PROFILE_H

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
//   Tier 2 — Adaptive recompile (hot):
//     Every function counts its own invocations via an atomicrmw at entry.
//     When the count first reaches kRecompileThreshold the function calls
//     __omsc_adaptive_recompile(), which:
//       1. Parses the clean (counter-free) bitcode into a fresh context.
//       2. Annotates the target function with setEntryCount(callCount) —
//          this is the PGO hint that shifts the LLVM optimizer from static
//          heuristics to real runtime data.
//       3. Re-runs the full LLVM O3 pipeline with that annotation in place:
//          the inliner, branch-layout pass, loop vectorizer, and unroller
//          all see the function as hot and optimize accordingly.
//       4. JIT-compiles the result via MCJIT.
//       5. Writes the new native function pointer into the function's
//          @__omsc_fn_<name> slot.
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
    /// 500 is chosen as a balance between:
    ///  - Fast enough to capture hot functions early in short-lived programs.
    ///  - High enough to accumulate a statistically stable type and branch
    ///    profile before spending O3 compilation time.
    /// For long-running server workloads a higher value (e.g. 5000) would give
    /// richer data; for micro-benchmarks a lower value (e.g. 100) converges
    /// faster.  500 is a reasonable general-purpose default.
    static constexpr int64_t kRecompileThreshold = 500;

    AdaptiveJITRunner();
    ~AdaptiveJITRunner();

    AdaptiveJITRunner(const AdaptiveJITRunner&) = delete;
    AdaptiveJITRunner& operator=(const AdaptiveJITRunner&) = delete;

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

    bool llvmInitialized_ = false;

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
