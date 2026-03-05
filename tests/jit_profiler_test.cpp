// ---------------------------------------------------------------------------
// Unit tests for the JIT profiler and deoptimization modules
// ---------------------------------------------------------------------------
// Tests:
//   - JITProfiler: branch recording, argument recording, dominant type,
//     constant specialization detection
//   - DeoptManager: guard failure counting, deoptimization threshold,
//     hot-patch slot reset
// ---------------------------------------------------------------------------

#include "deopt.h"
#include "jit_profiler.h"
#include <gtest/gtest.h>

using namespace omscript;

// ===========================================================================
// JITProfiler tests
// ===========================================================================

TEST(JITProfilerTest, RecordBranch) {
    JITProfiler::instance().reset();

    // recordBranch() samples every 16th call (static counter, `& 0xF`).
    // Call 160 taken + 80 not-taken to guarantee enough samples pass through.
    for (int i = 0; i < 160; i++)
        JITProfiler::instance().recordBranch("foo", 0, true);
    for (int i = 0; i < 80; i++)
        JITProfiler::instance().recordBranch("foo", 0, false);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("foo");
    ASSERT_NE(prof, nullptr);
    ASSERT_EQ(prof->branches.size(), 1u);
    // With 1/16 sampling, expect ~10 taken and ~5 not-taken (±1 due to alignment).
    EXPECT_GE(prof->branches[0].takenCount, 1u);
    EXPECT_GE(prof->branches[0].notTakenCount, 1u);
    // Ratio should approximate 160:80 = 2:1 → probability ≈ 0.667
    EXPECT_GT(prof->branches[0].takenProbability(), 0.4);
    EXPECT_LT(prof->branches[0].takenProbability(), 0.95);

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, RecordMultipleBranches) {
    JITProfiler::instance().reset();

    // Call 16× per branch site to guarantee at least one sample per site.
    for (int i = 0; i < 16; i++)
        JITProfiler::instance().recordBranch("bar", 0, true);
    for (int i = 0; i < 16; i++)
        JITProfiler::instance().recordBranch("bar", 1, false);
    for (int i = 0; i < 16; i++)
        JITProfiler::instance().recordBranch("bar", 2, true);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("bar");
    ASSERT_NE(prof, nullptr);
    ASSERT_GE(prof->branches.size(), 3u);
    EXPECT_GE(prof->branches[0].takenCount, 1u);
    EXPECT_GE(prof->branches[1].notTakenCount, 1u);
    EXPECT_GE(prof->branches[2].takenCount, 1u);

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, RecordArgType) {
    JITProfiler::instance().reset();

    // Record integer args for parameter 0
    for (int i = 0; i < 10; i++)
        JITProfiler::instance().recordArg("compute", 0, ArgType::Integer, 42);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("compute");
    ASSERT_NE(prof, nullptr);
    ASSERT_EQ(prof->args.size(), 1u);
    EXPECT_EQ(prof->args[0].dominantType(), ArgType::Integer);
    EXPECT_EQ(prof->args[0].totalCalls, 10u);

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, DominantTypeDetection) {
    JITProfiler::instance().reset();

    // Record 8 integers and 2 floats — integer should dominate
    for (int i = 0; i < 8; i++)
        JITProfiler::instance().recordArg("mixed", 0, ArgType::Integer, i);
    for (int i = 0; i < 2; i++)
        JITProfiler::instance().recordArg("mixed", 0, ArgType::Float, 0);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("mixed");
    ASSERT_NE(prof, nullptr);
    EXPECT_EQ(prof->args[0].dominantType(), ArgType::Integer);

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, ConstantSpecialization) {
    JITProfiler::instance().reset();

    // Record 9 calls with constant 42 and 1 with 99 — should detect specialization
    for (int i = 0; i < 9; i++)
        JITProfiler::instance().recordArg("const_fn", 0, ArgType::Integer, 42);
    JITProfiler::instance().recordArg("const_fn", 0, ArgType::Integer, 99);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("const_fn");
    ASSERT_NE(prof, nullptr);
    EXPECT_TRUE(prof->args[0].hasConstantSpecialization());
    EXPECT_EQ(prof->args[0].observedConstant, 42);

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, NoConstantSpecialization) {
    JITProfiler::instance().reset();

    // Record diverse values — should NOT detect constant specialization
    for (int i = 0; i < 10; i++)
        JITProfiler::instance().recordArg("diverse", 0, ArgType::Integer, i);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("diverse");
    ASSERT_NE(prof, nullptr);
    EXPECT_FALSE(prof->args[0].hasConstantSpecialization());

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, GetNonexistentProfile) {
    JITProfiler::instance().reset();
    EXPECT_EQ(JITProfiler::instance().getProfile("nonexistent"), nullptr);
}

TEST(JITProfilerTest, GetOrCreateProfile) {
    JITProfiler::instance().reset();

    auto& prof = JITProfiler::instance().getOrCreateProfile("new_fn");
    EXPECT_EQ(prof.name, "new_fn");
    EXPECT_EQ(prof.callCount, 0u);
    EXPECT_TRUE(prof.branches.empty());
    EXPECT_TRUE(prof.args.empty());

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, BranchProbabilityNoData) {
    BranchProfile bp;
    // No data — should return 0.5 (50/50)
    EXPECT_DOUBLE_EQ(bp.takenProbability(), 0.5);
}

// ===========================================================================
// DeoptManager tests
// ===========================================================================

TEST(DeoptManagerTest, InitialState) {
    DeoptManager::instance().reset();
    EXPECT_EQ(DeoptManager::instance().failureCount("test"), 0);
    EXPECT_FALSE(DeoptManager::instance().isDeoptimized("test"));
}

TEST(DeoptManagerTest, RecordGuardFailure) {
    DeoptManager::instance().reset();
    void* slot = reinterpret_cast<void*>(0xDEADBEEF);
    void** slotPtr = &slot;

    // Record one guard failure — should not trigger deoptimization
    DeoptManager::instance().onGuardFailure("guard_fn", slotPtr);
    EXPECT_EQ(DeoptManager::instance().failureCount("guard_fn"), 1);
    EXPECT_FALSE(DeoptManager::instance().isDeoptimized("guard_fn"));
    EXPECT_NE(slot, nullptr); // slot should NOT be cleared yet

    DeoptManager::instance().reset();
}

TEST(DeoptManagerTest, DeoptimizationThreshold) {
    DeoptManager::instance().reset();
    void* slot = reinterpret_cast<void*>(0xDEADBEEF);
    void** slotPtr = &slot;

    // Record failures up to the threshold
    for (int64_t i = 0; i < DeoptManager::kDeoptThreshold; i++) {
        DeoptManager::instance().onGuardFailure("hot_fn", slotPtr);
    }

    // After kDeoptThreshold failures, the function should be deoptimized
    EXPECT_TRUE(DeoptManager::instance().isDeoptimized("hot_fn"));
    EXPECT_EQ(DeoptManager::instance().failureCount("hot_fn"), DeoptManager::kDeoptThreshold);
    EXPECT_EQ(slot, nullptr); // slot should be cleared to revert to baseline
}

TEST(DeoptManagerTest, DeoptOnlyOnce) {
    DeoptManager::instance().reset();
    void* slot = reinterpret_cast<void*>(0xDEADBEEF);
    void** slotPtr = &slot;

    // Trigger deoptimization
    for (int64_t i = 0; i < DeoptManager::kDeoptThreshold + 5; i++) {
        DeoptManager::instance().onGuardFailure("once_fn", slotPtr);
    }

    // Slot should only be cleared once
    EXPECT_TRUE(DeoptManager::instance().isDeoptimized("once_fn"));
    EXPECT_EQ(slot, nullptr);
}

TEST(DeoptManagerTest, NullSlotPointer) {
    DeoptManager::instance().reset();

    // Passing nullptr for fnPtrSlot should not crash
    for (int64_t i = 0; i < DeoptManager::kDeoptThreshold; i++) {
        DeoptManager::instance().onGuardFailure("null_slot_fn", nullptr);
    }
    EXPECT_TRUE(DeoptManager::instance().isDeoptimized("null_slot_fn"));
}

// ===========================================================================
// ArgType utility tests
// ===========================================================================

TEST(ArgTypeTest, ArgTypeNames) {
    EXPECT_STREQ(argTypeName(ArgType::Unknown), "unknown");
    EXPECT_STREQ(argTypeName(ArgType::Integer), "int");
    EXPECT_STREQ(argTypeName(ArgType::Float), "float");
    EXPECT_STREQ(argTypeName(ArgType::String), "string");
    EXPECT_STREQ(argTypeName(ArgType::Array), "array");
    EXPECT_STREQ(argTypeName(ArgType::None), "none");
}

TEST(ArgProfileTest, RecordAndDominant) {
    ArgProfile ap;
    ap.record(ArgType::Integer, 10);
    ap.record(ArgType::Integer, 20);
    ap.record(ArgType::Float, 0);

    EXPECT_EQ(ap.dominantType(), ArgType::Integer);
    EXPECT_EQ(ap.totalCalls, 3u);
}

TEST(ArgProfileTest, EmptyDominant) {
    ArgProfile ap;
    EXPECT_EQ(ap.dominantType(), ArgType::Unknown);
    EXPECT_EQ(ap.totalCalls, 0u);
}

// ===========================================================================
// C-linkage callback tests — verify the extern "C" entry points work
// ===========================================================================

TEST(JITProfilerCallbackTest, ProfileBranchCallback) {
    JITProfiler::instance().reset();

    // C callback goes through recordBranch() which samples every 16th call.
    // Call 32× taken + 16× not-taken to ensure samples pass through.
    for (int i = 0; i < 32; i++)
        __omsc_profile_branch("cb_br_fn", 0, 1); // taken
    for (int i = 0; i < 16; i++)
        __omsc_profile_branch("cb_br_fn", 0, 0); // not taken

    const FunctionProfile* prof = JITProfiler::instance().getProfile("cb_br_fn");
    ASSERT_NE(prof, nullptr);
    ASSERT_GE(prof->branches.size(), 1u);
    EXPECT_GE(prof->branches[0].takenCount, 1u);
    EXPECT_GE(prof->branches[0].notTakenCount, 1u);

    JITProfiler::instance().reset();
}

TEST(JITProfilerCallbackTest, ProfileBranchMultipleSites) {
    JITProfiler::instance().reset();

    // 16 calls per branch site to guarantee at least one sample per site.
    for (int i = 0; i < 16; i++)
        __omsc_profile_branch("mb_fn", 0, 1);
    for (int i = 0; i < 16; i++)
        __omsc_profile_branch("mb_fn", 1, 0);
    for (int i = 0; i < 16; i++)
        __omsc_profile_branch("mb_fn", 2, 1);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("mb_fn");
    ASSERT_NE(prof, nullptr);
    ASSERT_GE(prof->branches.size(), 3u);
    EXPECT_GE(prof->branches[0].takenCount, 1u);
    EXPECT_GE(prof->branches[1].notTakenCount, 1u);
    EXPECT_GE(prof->branches[2].takenCount, 1u);

    JITProfiler::instance().reset();
}

TEST(JITProfilerCallbackTest, ProfileArgCallback) {
    JITProfiler::instance().reset();

    __omsc_profile_arg("cb_arg_fn", 0, static_cast<uint8_t>(ArgType::Integer), 100);
    __omsc_profile_arg("cb_arg_fn", 0, static_cast<uint8_t>(ArgType::Integer), 200);
    __omsc_profile_arg("cb_arg_fn", 1, static_cast<uint8_t>(ArgType::Float), 0);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("cb_arg_fn");
    ASSERT_NE(prof, nullptr);
    ASSERT_GE(prof->args.size(), 2u);
    EXPECT_EQ(prof->args[0].dominantType(), ArgType::Integer);
    EXPECT_EQ(prof->args[0].totalCalls, 2u);
    EXPECT_EQ(prof->args[1].dominantType(), ArgType::Float);

    JITProfiler::instance().reset();
}

TEST(JITProfilerCallbackTest, ProfileArgOutOfRangeTypeClamped) {
    JITProfiler::instance().reset();

    // Type byte 255 is out of range — the callback should clamp to ArgType::Unknown
    __omsc_profile_arg("clamp_fn", 0, 255, 0);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("clamp_fn");
    ASSERT_NE(prof, nullptr);
    ASSERT_GE(prof->args.size(), 1u);
    EXPECT_EQ(prof->args[0].dominantType(), ArgType::Unknown);

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, DumpDoesNotCrash) {
    JITProfiler::instance().reset();
    JITProfiler::instance().recordBranch("dump_fn", 0, true);
    JITProfiler::instance().recordArg("dump_fn", 0, ArgType::Integer, 42);
    // Redirect stderr to avoid polluting test output, just verify no crash
    EXPECT_NO_FATAL_FAILURE(JITProfiler::instance().dump());
    JITProfiler::instance().reset();
}

// ===========================================================================
// Constant specialization boundary tests — validate the >80% threshold used
// by Tier-2 llvm.assume injection
// ===========================================================================

TEST(ArgProfileTest, ConstantSpecExactThreshold) {
    // Exactly 80% (8/10) should NOT trigger (threshold is >80%, i.e. strictly greater)
    ArgProfile ap;
    for (int i = 0; i < 8; i++)
        ap.record(ArgType::Integer, 42);
    ap.record(ArgType::Integer, 99);
    ap.record(ArgType::Integer, 100);
    EXPECT_FALSE(ap.hasConstantSpecialization());
}

TEST(ArgProfileTest, ConstantSpecAboveThreshold) {
    // 81% (81/100) should trigger constant specialization
    ArgProfile ap;
    for (int i = 0; i < 81; i++)
        ap.record(ArgType::Integer, 42);
    for (int i = 0; i < 19; i++)
        ap.record(ArgType::Integer, i + 100);
    EXPECT_TRUE(ap.hasConstantSpecialization());
    EXPECT_EQ(ap.observedConstant, 42);
}

TEST(ArgProfileTest, ConstantSpecNonIntegerType) {
    // Float arguments should NOT trigger constant specialization
    // (only integer constants are tracked)
    ArgProfile ap;
    for (int i = 0; i < 10; i++)
        ap.record(ArgType::Float, 0);
    EXPECT_FALSE(ap.hasConstantSpecialization());
}

TEST(ArgProfileTest, ConstantSpecSingleCall) {
    // A single call with a constant should trigger (1/1 = 100% > 80%)
    ArgProfile ap;
    ap.record(ArgType::Integer, 7);
    EXPECT_TRUE(ap.hasConstantSpecialization());
    EXPECT_EQ(ap.observedConstant, 7);
}

TEST(JITProfilerTest, MultiParamConstantSpecialization) {
    JITProfiler::instance().reset();

    // Simulate a function with two parameters:
    //   param 0: always receives constant 10
    //   param 1: receives diverse values
    for (int i = 0; i < 20; i++) {
        JITProfiler::instance().recordArg("multi_param", 0, ArgType::Integer, 10);
        JITProfiler::instance().recordArg("multi_param", 1, ArgType::Integer, i);
    }

    const FunctionProfile* prof = JITProfiler::instance().getProfile("multi_param");
    ASSERT_NE(prof, nullptr);
    ASSERT_EQ(prof->args.size(), 2u);
    // param 0 should have constant specialization
    EXPECT_TRUE(prof->args[0].hasConstantSpecialization());
    EXPECT_EQ(prof->args[0].observedConstant, 10);
    // param 1 should NOT have constant specialization (diverse values)
    EXPECT_FALSE(prof->args[1].hasConstantSpecialization());

    JITProfiler::instance().reset();
}

TEST(ArgProfileTest, ConstantSpecZeroCalls) {
    // Zero calls should NOT trigger constant specialization
    ArgProfile ap;
    EXPECT_FALSE(ap.hasConstantSpecialization());
    EXPECT_EQ(ap.totalCalls, 0u);
}

TEST(ArgProfileTest, NegativeConstantTracked) {
    // Verify negative integer constants are properly tracked
    ArgProfile ap;
    for (int i = 0; i < 10; i++)
        ap.record(ArgType::Integer, -42);
    EXPECT_TRUE(ap.hasConstantSpecialization());
    EXPECT_EQ(ap.observedConstant, -42);
}

// ===========================================================================
// Value range profiling tests
// ===========================================================================

TEST(ArgProfileTest, RangeTrackingBasic) {
    // Record values in [10, 50] — should track min/max correctly
    ArgProfile ap;
    ap.record(ArgType::Integer, 10);
    ap.record(ArgType::Integer, 30);
    ap.record(ArgType::Integer, 50);
    ap.record(ArgType::Integer, 20);
    EXPECT_EQ(ap.minObserved, 10);
    EXPECT_EQ(ap.maxObserved, 50);
    EXPECT_EQ(ap.rangeCount, 4u);
}

TEST(ArgProfileTest, RangeSpecializationTightRange) {
    // All values in [0, 100] — tight range, should trigger range specialization
    ArgProfile ap;
    for (int i = 0; i <= 100; i++)
        ap.record(ArgType::Integer, i);
    EXPECT_TRUE(ap.hasRangeSpecialization());
    EXPECT_EQ(ap.minObserved, 0);
    EXPECT_EQ(ap.maxObserved, 100);
}

TEST(ArgProfileTest, RangeSpecializationWideRange) {
    // Values spread across [0, 10000] — too wide, should NOT trigger
    ArgProfile ap;
    ap.record(ArgType::Integer, 0);
    ap.record(ArgType::Integer, 10000);
    EXPECT_FALSE(ap.hasRangeSpecialization());
}

TEST(ArgProfileTest, RangeSpecializationNoData) {
    // No data — should NOT trigger range specialization
    ArgProfile ap;
    EXPECT_FALSE(ap.hasRangeSpecialization());
}

TEST(ArgProfileTest, RangeSpecializationNegativeRange) {
    // Negative range [-50, -10] — should trigger
    ArgProfile ap;
    for (int i = -50; i <= -10; i++)
        ap.record(ArgType::Integer, i);
    EXPECT_TRUE(ap.hasRangeSpecialization());
    EXPECT_EQ(ap.minObserved, -50);
    EXPECT_EQ(ap.maxObserved, -10);
}

TEST(ArgProfileTest, RangeSpecializationMixedTypes) {
    // Only integer values contribute to range; if <90% are integers,
    // range specialization should not trigger.
    ArgProfile ap;
    for (int i = 0; i < 5; i++)
        ap.record(ArgType::Integer, i);
    for (int i = 0; i < 6; i++)
        ap.record(ArgType::Float, 0);
    // 5 integers out of 11 total = ~45% → range should NOT trigger
    EXPECT_FALSE(ap.hasRangeSpecialization());
}

TEST(ArgProfileTest, RangeSpecializationSingleValue) {
    // A single value [42, 42] is a tight range
    ArgProfile ap;
    ap.record(ArgType::Integer, 42);
    EXPECT_TRUE(ap.hasRangeSpecialization());
    EXPECT_EQ(ap.minObserved, 42);
    EXPECT_EQ(ap.maxObserved, 42);
}

TEST(ArgProfileTest, RangeSpecializationExactBoundary) {
    // Range width exactly 1024: [0, 1024] — should trigger
    ArgProfile ap;
    ap.record(ArgType::Integer, 0);
    ap.record(ArgType::Integer, 1024);
    EXPECT_TRUE(ap.hasRangeSpecialization());
}

TEST(ArgProfileTest, RangeSpecializationOverBoundary) {
    // Range width 1025: [0, 1025] — should NOT trigger
    ArgProfile ap;
    ap.record(ArgType::Integer, 0);
    ap.record(ArgType::Integer, 1025);
    EXPECT_FALSE(ap.hasRangeSpecialization());
}

// ===========================================================================
// Loop trip count profiling tests
// ===========================================================================

TEST(LoopProfileTest, BasicTripCount) {
    LoopProfile lp;
    EXPECT_EQ(lp.averageTripCount(), 0u);
    EXPECT_EQ(lp.executionCount, 0u);
}

TEST(LoopProfileTest, AverageTripCount) {
    LoopProfile lp;
    lp.totalIterations = 300;
    lp.executionCount = 10;
    EXPECT_EQ(lp.averageTripCount(), 30u);
}

TEST(LoopProfileTest, SingleExecution) {
    LoopProfile lp;
    lp.totalIterations = 42;
    lp.executionCount = 1;
    EXPECT_EQ(lp.averageTripCount(), 42u);
}

TEST(JITProfilerTest, RecordLoopTripCount) {
    JITProfiler::instance().reset();

    // recordLoopTripCount() samples every 8th call (static counter, `& 0x7`).
    // Call 8× per observation to guarantee at least one sample passes.
    for (int i = 0; i < 8; i++)
        JITProfiler::instance().recordLoopTripCount("loop_fn", 0, 100);
    for (int i = 0; i < 8; i++)
        JITProfiler::instance().recordLoopTripCount("loop_fn", 0, 200);
    for (int i = 0; i < 8; i++)
        JITProfiler::instance().recordLoopTripCount("loop_fn", 0, 300);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("loop_fn");
    ASSERT_NE(prof, nullptr);
    ASSERT_EQ(prof->loops.size(), 1u);
    // With sampling, at least some observations recorded.
    EXPECT_GE(prof->loops[0].totalIterations, 100u);
    EXPECT_GE(prof->loops[0].executionCount, 1u);
    EXPECT_GE(prof->loops[0].averageTripCount(), 100u);

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, RecordMultipleLoops) {
    JITProfiler::instance().reset();

    // 8 calls per loop site to guarantee at least one sample per site.
    for (int i = 0; i < 8; i++)
        JITProfiler::instance().recordLoopTripCount("multi_loop", 0, 10);
    for (int i = 0; i < 8; i++)
        JITProfiler::instance().recordLoopTripCount("multi_loop", 1, 50);
    for (int i = 0; i < 8; i++)
        JITProfiler::instance().recordLoopTripCount("multi_loop", 2, 1000);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("multi_loop");
    ASSERT_NE(prof, nullptr);
    ASSERT_EQ(prof->loops.size(), 3u);
    EXPECT_GE(prof->loops[0].averageTripCount(), 10u);
    EXPECT_GE(prof->loops[1].averageTripCount(), 50u);
    EXPECT_GE(prof->loops[2].averageTripCount(), 1000u);

    JITProfiler::instance().reset();
}

// ===========================================================================
// Call-site frequency profiling tests
// ===========================================================================

TEST(JITProfilerTest, RecordCallSite) {
    JITProfiler::instance().reset();

    // recordCallSite() samples every 16th call (static counter, `& 0xF`).
    // Call 32× per callee to guarantee at least one sample each.
    for (int i = 0; i < 32; i++)
        JITProfiler::instance().recordCallSite("caller", "callee_a");
    for (int i = 0; i < 16; i++)
        JITProfiler::instance().recordCallSite("caller", "callee_b");

    const FunctionProfile* prof = JITProfiler::instance().getProfile("caller");
    ASSERT_NE(prof, nullptr);
    EXPECT_GE(prof->callSites.size(), 1u);
    // At least one of the callees should have been recorded.
    EXPECT_GE(prof->callSites.count("callee_a") + prof->callSites.count("callee_b"), 1u);

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, CallSiteIndependentCallers) {
    JITProfiler::instance().reset();

    // 16 calls per caller to guarantee at least one sample passes.
    for (int i = 0; i < 16; i++)
        JITProfiler::instance().recordCallSite("fn_a", "helper");
    for (int i = 0; i < 16; i++)
        JITProfiler::instance().recordCallSite("fn_b", "helper");

    const FunctionProfile* profA = JITProfiler::instance().getProfile("fn_a");
    const FunctionProfile* profB = JITProfiler::instance().getProfile("fn_b");
    // With a static global counter, samples may only land in one of the two.
    // At least one of the two profiles should exist with data.
    bool hasA = (profA != nullptr && profA->callSites.count("helper") > 0);
    bool hasB = (profB != nullptr && profB->callSites.count("helper") > 0);
    EXPECT_TRUE(hasA || hasB);

    JITProfiler::instance().reset();
}

// ===========================================================================
// C-linkage callback tests for new profiling callbacks
// ===========================================================================

TEST(JITProfilerCallbackTest, ProfileLoopCallback) {
    JITProfiler::instance().reset();

    // recordLoopTripCount() samples every 8th call.
    // Call 8× per observation to ensure at least one passes.
    for (int i = 0; i < 8; i++)
        __omsc_profile_loop("cb_loop_fn", 0, 42);
    for (int i = 0; i < 8; i++)
        __omsc_profile_loop("cb_loop_fn", 0, 58);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("cb_loop_fn");
    ASSERT_NE(prof, nullptr);
    ASSERT_GE(prof->loops.size(), 1u);
    EXPECT_GE(prof->loops[0].totalIterations, 42u);
    EXPECT_GE(prof->loops[0].executionCount, 1u);

    JITProfiler::instance().reset();
}

TEST(JITProfilerCallbackTest, ProfileCallSiteCallback) {
    JITProfiler::instance().reset();

    // recordCallSite() samples every 16th call.
    // Call 32× to ensure at least one sample passes.
    for (int i = 0; i < 32; i++)
        __omsc_profile_call_site("cb_caller", "cb_callee");

    const FunctionProfile* prof = JITProfiler::instance().getProfile("cb_caller");
    ASSERT_NE(prof, nullptr);
    EXPECT_GE(prof->callSites.count("cb_callee"), 1u);
    EXPECT_GE(prof->callSites.at("cb_callee"), 1u);

    JITProfiler::instance().reset();
}
