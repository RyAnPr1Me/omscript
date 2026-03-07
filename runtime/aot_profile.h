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
//   │  (.om)     │    │ Frontend │    │ (AOT, SSA/LLVM)  │    │ (Tier-1, O1)  │
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
//                                     │ (>= 10 calls)      │
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
//     non-main function, and the result is JIT-compiled at O1 via LLVM
//     ORC LLJIT.  Execution begins immediately.  O1 codegen provides
//     good baseline performance; the code is replaced after just 2 calls
//     by PGO-guided Tier-2.
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
//   Tier 2 — Warm recompile (2 calls):
//     Full O3+PGO recompile with double O3 pass for cascading
//     optimizations.  Inliner threshold: 10000 (whole-module) / 5000
//     (per-function).  Vectorize width 16, interleave 16, aggressive
//     loop unrolling up to 64 iterations.  Constant-arg specialization
//     threshold lowered to 60% for more aggressive folding.
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
class LLVMContext;
class Module;
class TargetMachine;
namespace orc {
class LLJIT;
} // namespace orc
} // namespace llvm

namespace omscript {

class AdaptiveJITRunner {
  public:
    // -----------------------------------------------------------------------
    // Multi-tier recompilation thresholds
    // -----------------------------------------------------------------------
    // The JIT uses two execution tiers, each triggered when a function's
    // call count first reaches the corresponding threshold:
    //
    //   Tier 1 (baseline):  O1-backend JIT of the compiler's IR — runs from
    //                       call 0 while profile data is being collected.
    //                       O1 backend provides good baseline code quality;
    //                       the code is replaced by Tier-2 after just 2 calls.
    //   Tier 2 (warm):      2 calls — full O3+PGO recompile with double O3
    //                       pass, aggressive inlining (threshold 10000/5000),
    //                       constant-arg specialization (60% threshold),
    //                       vectorize width 16, loop unrolling up to 64.
    //
    // The single recompile tier minimises total recompilation overhead:
    // each hot function is recompiled at most once, using the most
    // aggressive optimisation available.  4 background threads ensure
    // compilation completes before the function is called a third time.
    // -----------------------------------------------------------------------
    static constexpr int64_t kTier2Threshold = 2;
    static constexpr int64_t kTier3Threshold = 1000000000LL; // effectively disabled
    /// Highest tier number (Tier-1 = baseline, Tier-2..kMaxTier = recompiled).
    static constexpr int kMaxTier = 2;

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

    /// Keeps ORC LLJIT instances (and their contexts) alive so compiled
    /// native code pointers remain valid for the process lifetime.
    struct JitModule {
        std::unique_ptr<llvm::orc::LLJIT> lljit;
    };
    std::vector<JitModule> modules_;

    /// Tracks the compilation state per function.  Values:
    ///   0  — never seen (default)
    ///   1  — eagerly queued at startup but not yet compiled; onHotFunction()
    ///         may promote to 2 with the actual runtime callCount so that the
    ///         hot entry preempts the low-priority eager entry in the queue
    ///   2+ — compiled (or being compiled) at that tier
    std::unordered_map<std::string, int> functionTier_;
    std::mutex recompiledMtx_; ///< Guards functionTier_ and modules_ across threads.
    bool verbose_ = false;     ///< Print JIT recompile events when true.

    /// Maps every user function name to its __omsc_fn_* slot address in the
    /// Tier-1 LLJIT.  Populated during eager startup before background threads
    /// start; read-only thereafter (no mutex needed for reads).
    std::unordered_map<std::string, void**> functionSlots_;
    std::mutex slotsMtx_; ///< Guards functionSlots_ writes during startup.

    /// Set to true once a whole-module background compilation has been
    /// enqueued.  onHotFunction() checks this to skip redundant per-function
    /// enqueues.
    std::atomic<bool> wholeModuleQueued_{false};

    // --- User-provided optimisation flags (propagated to Tier-2+ O3 pipeline) ---
    bool fastMath_ = false;
    bool vectorize_ = true;
    bool unrollLoops_ = true;
    bool loopOptimize_ = true;

    // -------------------------------------------------------------------
    // Cached TargetMachine factory for recompilation
    // -------------------------------------------------------------------
    // Creating an LLVM TargetMachine involves string parsing, feature
    // detection, and target registry lookup (~1-3ms).  With parallel
    // background threads, each thread needs its own TargetMachine (TM
    // is not thread-safe).  We cache the host CPU/features/triple strings
    // once and reuse them.
    // -------------------------------------------------------------------
    std::string cachedTriple_;
    std::string cachedCPU_;
    std::string cachedFeatures_;
    std::once_flag tmInitFlag_;

    /// Build a new TargetMachine for the host CPU.
    /// Thread-safe: can be called from multiple background threads.
    std::unique_ptr<llvm::TargetMachine> createTargetMachine();

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
    /// Sorted by callCount descending in the priority queue: the function that
    /// has been called the most times is compiled first, ensuring the hottest
    /// code gets optimized before lukewarm or eagerly-queued code.
    struct RecompileTask {
        std::string funcName;
        int64_t callCount;
        void** fnPtrSlot; ///< Pointer to the dispatch slot (cast to std::atomic<void*>* at point of update).
        /// Max-heap ordering: higher callCount = higher priority = compiled first.
        bool operator<(const RecompileTask& o) const { return callCount < o.callCount; }
    };

    std::vector<std::thread> bgThreads_; ///< Background compilation workers.
    /// Priority queue of pending recompilation requests (hottest functions first).
    /// std::priority_queue is a max-heap, so the task with the largest callCount
    /// is always at the top and gets processed next.  Threshold-triggered tasks
    /// (callCount = actual runtime value, >= kTier2Threshold) naturally preempt
    /// eagerly-queued tasks (callCount = kTier2Threshold), so a function called
    /// 10 000 times is always compiled before one called only 5 times.
    std::priority_queue<RecompileTask> taskQueue_;
    std::mutex queueMtx_;                    ///< Guards taskQueue_.
    std::condition_variable queueCV_;        ///< Signals the background threads.
    std::atomic<bool> shutdownRequested_{false}; ///< Signals the workers to exit.

    /// Number of background compilation threads.
    /// Using 4 threads: maximises parallel compilation bandwidth so the
    /// whole-module O3 compile and per-function fallback recompiles can
    /// proceed simultaneously.
    static constexpr int kNumBgThreads = 4;

    /// Background worker loop: waits for tasks, executes them sequentially.
    void backgroundWorker();

    /// Compile the entire program module at O3+PGO in a single LLJIT instance.
    /// Marks all non-recursive user functions as AlwaysInline to enable
    /// aggressive constant folding (e.g. fib_iter(150), sum_squares_mod(300,…)).
    /// After successful compilation, atomically patches every function slot
    /// collected in functionSlots_.  Called from a background thread.
    void doRecompileWholeModule();

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
