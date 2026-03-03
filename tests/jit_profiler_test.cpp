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

    // Record 10 taken + 5 not-taken for branch 0 of function "foo"
    for (int i = 0; i < 10; i++)
        JITProfiler::instance().recordBranch("foo", 0, true);
    for (int i = 0; i < 5; i++)
        JITProfiler::instance().recordBranch("foo", 0, false);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("foo");
    ASSERT_NE(prof, nullptr);
    ASSERT_EQ(prof->branches.size(), 1u);
    EXPECT_EQ(prof->branches[0].takenCount, 10u);
    EXPECT_EQ(prof->branches[0].notTakenCount, 5u);
    EXPECT_NEAR(prof->branches[0].takenProbability(), 10.0 / 15.0, 0.01);

    JITProfiler::instance().reset();
}

TEST(JITProfilerTest, RecordMultipleBranches) {
    JITProfiler::instance().reset();

    JITProfiler::instance().recordBranch("bar", 0, true);
    JITProfiler::instance().recordBranch("bar", 1, false);
    JITProfiler::instance().recordBranch("bar", 2, true);

    const FunctionProfile* prof = JITProfiler::instance().getProfile("bar");
    ASSERT_NE(prof, nullptr);
    ASSERT_EQ(prof->branches.size(), 3u);
    EXPECT_EQ(prof->branches[0].takenCount, 1u);
    EXPECT_EQ(prof->branches[1].notTakenCount, 1u);
    EXPECT_EQ(prof->branches[2].takenCount, 1u);

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
