// ---------------------------------------------------------------------------
// DeoptManager unit tests
// ---------------------------------------------------------------------------

#include "deopt.h"
#include <gtest/gtest.h>

using namespace omscript;

class DeoptManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        DeoptManager::instance().reset();
    }
    void TearDown() override {
        DeoptManager::instance().reset();
    }
};

TEST_F(DeoptManagerTest, InitiallyZeroFailures) {
    EXPECT_EQ(DeoptManager::instance().failureCount("foo"), 0);
    EXPECT_FALSE(DeoptManager::instance().isDeoptimized("foo"));
}

TEST_F(DeoptManagerTest, SingleGuardFailure) {
    void* slot = reinterpret_cast<void*>(0xdeadbeef);
    void** slotPtr = &slot;
    DeoptManager::instance().onGuardFailure("bar", slotPtr);

    EXPECT_EQ(DeoptManager::instance().failureCount("bar"), 1);
    EXPECT_FALSE(DeoptManager::instance().isDeoptimized("bar"));
    // Slot should not have been cleared yet (below threshold)
    EXPECT_NE(slot, nullptr);
}

TEST_F(DeoptManagerTest, DeoptAfterThreshold) {
    void* slot = reinterpret_cast<void*>(0xdeadbeef);
    void** slotPtr = &slot;

    // Fire enough failures to trigger deopt
    for (int64_t i = 0; i < DeoptManager::kDeoptThreshold; i++) {
        DeoptManager::instance().onGuardFailure("hot_func", slotPtr);
    }

    EXPECT_TRUE(DeoptManager::instance().isDeoptimized("hot_func"));
    EXPECT_GE(DeoptManager::instance().failureCount("hot_func"), DeoptManager::kDeoptThreshold);
    // Slot should have been cleared to nullptr
    EXPECT_EQ(slot, nullptr);
}

TEST_F(DeoptManagerTest, NullSlotSafe) {
    // Passing nullptr for fnPtrSlot should not crash
    for (int64_t i = 0; i <= DeoptManager::kDeoptThreshold; i++) {
        DeoptManager::instance().onGuardFailure("null_slot", nullptr);
    }
    EXPECT_TRUE(DeoptManager::instance().isDeoptimized("null_slot"));
}

TEST_F(DeoptManagerTest, ResetClearsState) {
    void* slot = reinterpret_cast<void*>(0xdeadbeef);
    void** slotPtr = &slot;

    for (int64_t i = 0; i < DeoptManager::kDeoptThreshold; i++) {
        DeoptManager::instance().onGuardFailure("reset_func", slotPtr);
    }
    EXPECT_TRUE(DeoptManager::instance().isDeoptimized("reset_func"));

    DeoptManager::instance().reset();
    EXPECT_FALSE(DeoptManager::instance().isDeoptimized("reset_func"));
    EXPECT_EQ(DeoptManager::instance().failureCount("reset_func"), 0);
}

TEST_F(DeoptManagerTest, IndependentFunctions) {
    void* slot1 = reinterpret_cast<void*>(0x1);
    void* slot2 = reinterpret_cast<void*>(0x2);

    // Only deopt func1, not func2
    for (int64_t i = 0; i < DeoptManager::kDeoptThreshold; i++) {
        DeoptManager::instance().onGuardFailure("func1", &slot1);
    }
    DeoptManager::instance().onGuardFailure("func2", &slot2);

    EXPECT_TRUE(DeoptManager::instance().isDeoptimized("func1"));
    EXPECT_FALSE(DeoptManager::instance().isDeoptimized("func2"));
    EXPECT_EQ(slot1, nullptr);
    EXPECT_NE(slot2, nullptr);
}

// ---------------------------------------------------------------------------
// C-linkage callback test
// ---------------------------------------------------------------------------

TEST_F(DeoptManagerTest, CLinkageCallback) {
    void* slot = reinterpret_cast<void*>(0xdeadbeef);
    __omsc_deopt_guard_fail("c_func", &slot);
    EXPECT_EQ(DeoptManager::instance().failureCount("c_func"), 1);
}
