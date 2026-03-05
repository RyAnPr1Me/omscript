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
//                                     │  - Range specialize│
//                                     │  - Loop unroll PGO │
//                                     │  - Inline hints    │
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
// in-process using a multi-tier JIT strategy:
//
//   Tier 1 — Baseline JIT (fast startup):
//     The module's clean IR is serialised to bitcode, then a fresh copy is
//     loaded, call-counting dispatch prologs are injected into every
//     non-main function, and the result is JIT-compiled at O3 via LLVM
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
//   Tier 2 — Warm recompile (50 calls):
//     First O3+PGO recompile with early profile data.
//     Inliner threshold: 600.
//
//   Tier 3 — Hot recompile (500 calls):
//     Second recompile with 10x more profile samples.
//     Inliner threshold: 800 — more aggressive cross-function inlining.
//
//   Tier 4 — Saturated recompile (5000 calls):
//     Deep profile, inliner threshold 1200, double O3 pipeline pass
//     to catch cascading optimizations.
//
//   Tier 5 — Mega-hot recompile (50000 calls):
//     Maximum aggression: inliner threshold 1600, alwaysinline on hot
//     callees, double O3 pass, full specialisation of the hot path.
//
//   Each tier:
//     1. Parses the clean bitcode, strips unreachable functions.
//     2. Annotates with PGO entry counts, branch weights, type hints,
//        constant specialization, range assumptions, inline hints.
//     3. Re-runs the full LLVM O3 pipeline with tier-specific settings.
//     4. JIT-compiles and hot-patches the dispatch slot.
//
//   User flags (-ffast-math, -fvectorize, -funroll-loops, -floop-optimize)
//   are propagated to the Tier-2+ O3 pipeline so the JIT honours the same
//   optimisation policy the user requested.
//
//   Deoptimization:
//     Specialized code may include guard checks.  After kDeoptThreshold
//     failures, the hot-patch slot resets to nullptr — falling back to
//     Tier-1 baseline code.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
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
    // The JIT uses five execution tiers, each triggered when a function's
    // call count first reaches the corresponding threshold:
    //
    //   Tier 1 (baseline):  O3-backend JIT of the compiler's IR — runs from
    //                       call 0 while profile data is being collected.
    //   Tier 2 (warm):      50 calls — first O3+PGO recompile with early
    //                       profile data (branch weights, argument types).
    //   Tier 3 (hot):       500 calls — richer profile, more aggressive
    //                       inlining (threshold 800) and full loop unroll.
    //   Tier 4 (saturated): 5000 calls — deep profile, inliner 1200, double
    //                       O3 pass for cascading optimizations.
    //   Tier 5 (mega-hot):  50000 calls — maximum aggression: inliner 1600,
    //                       alwaysinline on hot callees, double O3 pass.
    //
    // Each successive tier uses increasingly aggressive inliner thresholds
    // and benefits from more statistically significant profile data.
    // -----------------------------------------------------------------------
    static constexpr int64_t kTier2Threshold = 50;
    static constexpr int64_t kTier3Threshold = 500;
    static constexpr int64_t kTier4Threshold = 5000;
    static constexpr int64_t kTier5Threshold = 50000;
    /// Highest tier number (Tier-1 = baseline, Tier-2..kMaxTier = recompiled).
    static constexpr int kMaxTier = 5;

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
    /// threshold.  Recompiles @p name from the clean bitcode with O3 + PGO,
    /// using tier-appropriate aggressiveness, then writes the new native
    /// function pointer into *fnPtrSlot.
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

    /// Return the tier (2, 3, 4, or 5) for a given call count, or 0 if below
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
/// Trigger Tier-2 recompilation of @p name.
/// @p callCount is the observed call count (used as the PGO entry-count hint).
/// @p fnPtrSlot points to the @__omsc_fn_<name> global in the JIT binary;
/// the recompiler stores the new native pointer there on success.
void __omsc_adaptive_recompile(const char* name, int64_t callCount, void** fnPtrSlot);
} // extern "C"

#endif // AOT_PROFILE_H
