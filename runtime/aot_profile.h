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
//                                     │ (>= 20 calls)      │
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
//                                     │  O2/O3 Optimization│
//                                     │  - Constant fold   │
//                                     │  - Dead code elim  │
//                                     │  - Inlining        │
//                                     │  - Loop unrolling  │
//                                     │  - Vectorization   │
//                                     │  - Type specialize │
//                                     │  - Range specialize│
//                                     │  - Loop unroll PGO │
//                                     │  - Inline hints    │
//                                     └────────┬───────────┘
//                                              │
//                                              ▼
//                                     ┌────────────────────┐
//                                     │ Optimized Machine  │────> Execution
//                                     │ Code (Tier-2+)     │    (hot-patched)
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
// in-process using a multi-tier JIT strategy:
//
//   Tier 1 — Baseline JIT (fast startup):
//     The module's clean IR is serialised to bitcode, then a fresh copy is
//     loaded, call-counting dispatch prologs are injected into every
//     non-main function, and the result is JIT-compiled at O2 via LLVM
//     MCJIT.  Execution begins immediately.
//
//   Runtime Profiling (continuous):
//     Each non-main function collects runtime data via atomic counters
//     and lightweight profiling callbacks:
//     - Call frequency, argument types, branch probabilities,
//       observed constants, value ranges, loop trip counts,
//       call-site frequencies.
//     Profiling continues even after Tier-2 promotion so that higher
//     tiers benefit from richer statistical data.
//
//   Tier 2 — Warm recompile (20 calls):
//     First O2+PGO recompile with early profile data.
//     Inliner threshold: 600.
//     O2 keeps recompilation fast (~2x faster than O3) while still
//     benefiting from PGO annotations (branch weights, type hints).
//
//   Tier 3 — Hot recompile (2000 calls):
//     Full O3+PGO recompile with deep profile data.
//     Inliner threshold: 1200, double O3 pass for cascading optimizations,
//     alwaysinline on hot callees (>40% of calls).
//
//   Background Compilation:
//     All recompilations (Tier-2 and Tier-3) run on a dedicated background
//     thread.  When a tier threshold is hit, the dispatch prolog's callback
//     enqueues a recompilation task and returns immediately — the function
//     continues executing baseline (Tier-1) code with zero pause.  When
//     the background thread finishes, it atomically stores the new native
//     function pointer into the dispatch slot, and subsequent calls take
//     the fast path to the optimised code.
//
//   Each tier:
//     1. Parses the clean bitcode, strips unreachable functions.
//     2. Annotates with PGO entry counts, branch weights, type hints,
//        constant specialization, range assumptions, inline hints.
//     3. Re-runs the LLVM O2 or O3 pipeline with tier-specific settings.
//     4. JIT-compiles and atomically hot-patches the dispatch slot.
//
//   User flags (-ffast-math, -fvectorize, -funroll-loops, -floop-optimize)
//   are propagated to the Tier-2+ pipeline so the JIT honours the same
//   optimisation policy the user requested.
//
//   Deoptimization:
//     Specialized code may include guard checks.  After kDeoptThreshold
//     failures, the hot-patch slot resets to nullptr — falling back to
//     Tier-1 baseline code.
// ---------------------------------------------------------------------------

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace llvm {
class ExecutionEngine;
class LLVMContext;
class Module;
} // namespace llvm

namespace omscript {

class AdaptiveJITRunner {
  public:
    // -----------------------------------------------------------------------
    // Multi-tier recompilation thresholds
    // -----------------------------------------------------------------------
    // The JIT uses three execution tiers, each triggered when a function's
    // call count first reaches the corresponding threshold:
    //
    //   Tier 1 (baseline):  O2-backend JIT of the compiler's IR — runs from
    //                       call 0 while profile data is being collected.
    //                       O2 backend provides fast startup with good code.
    //   Tier 2 (warm):      20 calls — first O2+PGO recompile with early
    //                       profile data (branch weights, argument types).
    //                       O2 keeps recompile time low while applying PGO.
    //   Tier 3 (hot):       2000 calls — full O3+PGO recompile with deep
    //                       profile, inliner threshold 1200, double O3
    //                       pass for cascading optimizations, alwaysinline
    //                       on hot callees.
    //
    // The reduced number of tiers (3 vs the previous 5) dramatically cuts
    // total recompilation overhead: each hot function is recompiled at most
    // twice instead of four times, and the Tier-2 O2 recompile is ~2x
    // faster than a full O3 pass.
    // -----------------------------------------------------------------------
    static constexpr int64_t kTier2Threshold = 20;
    static constexpr int64_t kTier3Threshold = 2000;
    /// Highest tier number (Tier-1 = baseline, Tier-2..kMaxTier = recompiled).
    static constexpr int kMaxTier = 3;

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

    // -------------------------------------------------------------------
    // Propagated optimisation flags — set by the driver before run().
    // These mirror the user's command-line flags (-ffast-math, -fvectorize,
    // etc.) so that Tier-2+ recompilation applies the SAME optimisation
    // policy the user requested for the AOT pipeline.
    // -------------------------------------------------------------------
    void setFastMath(bool v) {
        fastMath_ = v;
    }
    void setVectorize(bool v) {
        vectorize_ = v;
    }
    void setUnrollLoops(bool v) {
        unrollLoops_ = v;
    }
    void setLoopOptimize(bool v) {
        loopOptimize_ = v;
    }

    /// Execute the program defined by @p module in-process.
    /// The module is NOT modified — a working copy is created internally.
    /// Returns the integer value returned by main() (clamped to [0, 255]).
    int run(llvm::Module* module);

    /// Called from __omsc_adaptive_recompile when a function hits a tier
    /// threshold.  Enqueues an asynchronous recompilation on the background
    /// thread and returns immediately so the calling function continues
    /// executing baseline code with virtually zero pause.  The background
    /// thread atomically updates *fnPtrSlot when compilation finishes.
    void onHotFunction(const char* name, int64_t callCount, void** fnPtrSlot);

  private:
    /// Serialised clean IR — preserved from the initial module before any
    /// counter instrumentation is added.  Used as the source for all
    /// tiered recompilations.
    std::vector<char> cleanBitcode_;

    /// Keeps MCJIT ExecutionEngines (and their contexts) alive so compiled
    /// native code pointers remain valid for the process lifetime.
    struct JitModule {
        std::unique_ptr<llvm::LLVMContext> ctx;
        std::unique_ptr<llvm::ExecutionEngine> engine;
    };
    std::vector<JitModule> modules_;

    /// Tracks the highest completed tier per function (0 = not yet recompiled).
    std::unordered_map<std::string, int> functionTier_;
    std::mutex recompiledMtx_; ///< Guards functionTier_ and modules_ across threads.
    bool verbose_ = false;     ///< Print JIT recompile events when true.

    // --- User-provided optimisation flags (propagated to Tier-2+ O3 pipeline) ---
    bool fastMath_ = false;
    bool vectorize_ = true;
    bool unrollLoops_ = true;
    bool loopOptimize_ = true;

    // -------------------------------------------------------------------
    // Background compilation thread
    // -------------------------------------------------------------------
    // Recompilation is offloaded to a dedicated background thread so that
    // the main execution thread never stalls.  The dispatch prolog's
    // __omsc_adaptive_recompile callback enqueues a task and returns
    // immediately; the background thread picks it up, runs the O2/O3+PGO
    // pipeline, and atomically stores the new function pointer when done.
    //
    // The function pointer slot is updated with an atomic store (release
    // ordering) so the next volatile load in the dispatch prolog sees it
    // without any additional fencing on the hot path.
    // -------------------------------------------------------------------

    /// A pending recompilation request.
    struct RecompileTask {
        std::string funcName;
        int64_t callCount;
        void** fnPtrSlot; ///< Atomic-compatible pointer to the dispatch slot.
    };

    std::thread bgThread_;                   ///< Background compilation worker.
    std::queue<RecompileTask> taskQueue_;     ///< Pending recompilation requests.
    std::mutex queueMtx_;                    ///< Guards taskQueue_.
    std::condition_variable queueCV_;        ///< Signals the background thread.
    std::atomic<bool> shutdownRequested_{false}; ///< Signals the worker to exit.

    /// Background worker loop: waits for tasks, executes them sequentially.
    void backgroundWorker();

    /// The actual heavy recompilation work — runs on the background thread.
    /// Parses clean bitcode, applies PGO annotations, runs O2/O3 pipeline,
    /// JIT-compiles, and atomically stores the new function pointer.
    void doRecompile(const std::string& funcName, int64_t callCount, void** fnPtrSlot);

    /// Drain the task queue and join the background thread.
    /// Called from run() after main() returns to ensure all pending
    /// compilations complete before JIT modules are destroyed.
    void drainBackgroundThread();

    /// Return the tier (2 or 3) for a given call count, or 0 if below
    /// all thresholds.
    static int tierForCallCount(int64_t count);

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
/// Enqueue asynchronous recompilation of @p name on the background thread.
/// @p callCount is the observed call count (used as the PGO entry-count hint).
/// @p fnPtrSlot points to the @__omsc_fn_<name> global in the JIT binary;
/// the background thread atomically stores the new native pointer there on
/// success.  Returns immediately so the calling function continues executing
/// baseline code with no pause.
void __omsc_adaptive_recompile(const char* name, int64_t callCount, void** fnPtrSlot);
} // extern "C"

#endif // AOT_PROFILE_H
