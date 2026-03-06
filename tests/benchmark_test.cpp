// ---------------------------------------------------------------------------
// Benchmark tests for JIT profiler hot-path overhead
// ---------------------------------------------------------------------------
//
// These tests verify that the profiler recording functions have acceptable
// overhead and that the thread-local sampling counters provide correct
// per-thread sampling without cross-thread counter aliasing.
//
// Methodology:
//   - Call each profiler entry point many thousands of times and measure
//     wall-clock elapsed time.
//   - Assert that per-call overhead is below a generous threshold (200 ns).
//     This catches regressions like accidentally re-introducing mutex
//     acquisition or atomic operations on the hot path while remaining
//     stable on loaded CI runners.
//   - Verify that per-thread sampling counters fire independently across
//     threads so that two threads calling different functions both produce
//     samples, even when the same global call index would suppress one.
// ---------------------------------------------------------------------------

#include "jit_profiler.h"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

using namespace omscript;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Helper: measure elapsed nanoseconds for N calls of fn()
// ---------------------------------------------------------------------------
template <typename Fn>
static double nsPerCall(int n, Fn fn) {
    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i)
        fn(i);
    auto t1 = Clock::now();
    double elapsed = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return elapsed / static_cast<double>(n);
}

// ---------------------------------------------------------------------------
// recordBranch overhead
// ---------------------------------------------------------------------------
TEST(BenchmarkProfiler, RecordBranchOverhead) {
    JITProfiler::instance().reset();

    constexpr int kCalls = 100'000;
    double ns = nsPerCall(kCalls, [](int i) {
        JITProfiler::instance().recordBranch("bench_branch", 0, (i & 1) != 0);
    });

    // 200 ns per call is a generous upper bound that accommodates loaded CI
    // runners; well-tuned sampling code should be under 5 ns on modern
    // hardware.  The threshold still catches regressions that re-introduce
    // mutex acquisition (typically 500 ns+) on every call.
    EXPECT_LT(ns, 200.0) << "recordBranch overhead too high: " << ns << " ns/call";

    JITProfiler::instance().reset();
}

// ---------------------------------------------------------------------------
// recordLoopTripCount overhead
// ---------------------------------------------------------------------------
TEST(BenchmarkProfiler, RecordLoopTripCountOverhead) {
    JITProfiler::instance().reset();

    constexpr int kCalls = 100'000;
    double ns = nsPerCall(kCalls, [](int i) {
        JITProfiler::instance().recordLoopTripCount("bench_loop", 0,
                                                    static_cast<uint64_t>(i + 1));
    });

    EXPECT_LT(ns, 200.0) << "recordLoopTripCount overhead too high: " << ns << " ns/call";

    JITProfiler::instance().reset();
}

// ---------------------------------------------------------------------------
// recordCallSite overhead
// ---------------------------------------------------------------------------
TEST(BenchmarkProfiler, RecordCallSiteOverhead) {
    JITProfiler::instance().reset();

    constexpr int kCalls = 100'000;
    double ns = nsPerCall(kCalls, [](int /*i*/) {
        JITProfiler::instance().recordCallSite("bench_caller", "bench_callee");
    });

    EXPECT_LT(ns, 200.0) << "recordCallSite overhead too high: " << ns << " ns/call";

    JITProfiler::instance().reset();
}

// ---------------------------------------------------------------------------
// Thread-local counter isolation: each newly-created thread has its own
// independent sample counter starting at zero.
//
// Run two background threads sequentially (each to completion before the
// next starts) recording observations for distinct function names.  With
// thread-local counters every new thread begins at counter 0, so it fires
// its first sample on the very first eligible call and produces the
// expected number of samples regardless of any prior counter state in other
// threads.  Both functions must be sampled.
// ---------------------------------------------------------------------------
TEST(BenchmarkProfiler, ThreadLocalCounterIsolation) {
    JITProfiler::instance().reset();

    constexpr int kCallsPerThread = 160; // expect ~10 samples at 1/16 rate

    // Run each thread to completion before starting the next to avoid
    // mutex-contention races that would make the assertion non-deterministic.
    auto runWorker = [&](const char* funcName) {
        std::thread t([funcName]() {
            for (int i = 0; i < kCallsPerThread; ++i)
                JITProfiler::instance().recordBranch(funcName, 0, (i & 1) != 0);
        });
        t.join();
    };

    runWorker("tl_fn_a");
    runWorker("tl_fn_b");

    const FunctionProfile* profA = JITProfiler::instance().getProfile("tl_fn_a");
    const FunctionProfile* profB = JITProfiler::instance().getProfile("tl_fn_b");

    // Both functions must have been sampled.  A new thread's thread_local
    // counter starts at 0 so the first sampling window is always active.
    ASSERT_NE(profA, nullptr) << "tl_fn_a was not sampled at all";
    ASSERT_NE(profB, nullptr) << "tl_fn_b was not sampled at all";

    uint64_t samplesA = profA->branches.empty()
                            ? 0
                            : profA->branches[0].takenCount + profA->branches[0].notTakenCount;
    uint64_t samplesB = profB->branches.empty()
                            ? 0
                            : profB->branches[0].takenCount + profB->branches[0].notTakenCount;

    EXPECT_GE(samplesA, 1u) << "tl_fn_a: expected ≥1 sample, got " << samplesA;
    EXPECT_GE(samplesB, 1u) << "tl_fn_b: expected ≥1 sample, got " << samplesB;

    JITProfiler::instance().reset();
}

// ---------------------------------------------------------------------------
// Sampling rate accuracy: branch counter should record approximately 1/16
// of all calls from a single thread.
// ---------------------------------------------------------------------------
TEST(BenchmarkProfiler, BranchSamplingRateAccuracy) {
    JITProfiler::instance().reset();

    constexpr int kCalls = 1600; // expect ~100 samples at 1/16 rate
    for (int i = 0; i < kCalls; ++i)
        JITProfiler::instance().recordBranch("rate_check", 0, true);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("rate_check");
    ASSERT_NE(prof, nullptr);
    ASSERT_GE(prof->branches.size(), 1u);

    uint64_t samples = prof->branches[0].takenCount;
    // Allow ±50% tolerance around the expected 100 samples.
    EXPECT_GE(samples, 50u) << "fewer samples than expected: " << samples;
    EXPECT_LE(samples, 150u) << "more samples than expected: " << samples;

    JITProfiler::instance().reset();
}

// ---------------------------------------------------------------------------
// Sampling rate accuracy: loop counter should record approximately 1/8
// of all calls from a single thread.
// ---------------------------------------------------------------------------
TEST(BenchmarkProfiler, LoopSamplingRateAccuracy) {
    JITProfiler::instance().reset();

    constexpr int kCalls = 800; // expect ~100 samples at 1/8 rate
    for (int i = 0; i < kCalls; ++i)
        JITProfiler::instance().recordLoopTripCount("loop_rate_check", 0,
                                                    static_cast<uint64_t>(i + 1));

    const FunctionProfile* prof = JITProfiler::instance().getProfile("loop_rate_check");
    ASSERT_NE(prof, nullptr);
    ASSERT_GE(prof->loops.size(), 1u);

    uint64_t samples = prof->loops[0].executionCount;
    // Allow ±50% tolerance around the expected 100 samples.
    EXPECT_GE(samples, 50u) << "fewer loop samples than expected: " << samples;
    EXPECT_LE(samples, 150u) << "more loop samples than expected: " << samples;

    JITProfiler::instance().reset();
}
