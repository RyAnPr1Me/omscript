#include <gtest/gtest.h>
#include "bytecode.h"
#include <cstring>
#include <cstdint>

using namespace omscript;

// ===========================================================================
// Basic emit / read
// ===========================================================================

TEST(BytecodeTest, EmitOpCode) {
    BytecodeEmitter emitter;
    emitter.emit(OpCode::PUSH_INT);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 1u);
    EXPECT_EQ(static_cast<OpCode>(code[0]), OpCode::PUSH_INT);
}

TEST(BytecodeTest, EmitByte) {
    BytecodeEmitter emitter;
    emitter.emitByte(0xAB);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 1u);
    EXPECT_EQ(code[0], 0xAB);
}

TEST(BytecodeTest, EmitIntRoundTrip) {
    BytecodeEmitter emitter;
    int64_t value = 0x123456789ABCDEF0LL;
    emitter.emitInt(value);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 8u);

    // Manually decode little-endian
    uint64_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw |= (static_cast<uint64_t>(code[i]) << (i * 8));
    }
    int64_t decoded = 0;
    std::memcpy(&decoded, &raw, sizeof(decoded));
    EXPECT_EQ(decoded, value);
}

TEST(BytecodeTest, EmitIntNegative) {
    BytecodeEmitter emitter;
    emitter.emitInt(-42);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 8u);

    uint64_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw |= (static_cast<uint64_t>(code[i]) << (i * 8));
    }
    int64_t decoded = 0;
    std::memcpy(&decoded, &raw, sizeof(decoded));
    EXPECT_EQ(decoded, -42);
}

TEST(BytecodeTest, EmitFloatRoundTrip) {
    BytecodeEmitter emitter;
    double value = 3.14159;
    emitter.emitFloat(value);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 8u);

    uint64_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw |= (static_cast<uint64_t>(code[i]) << (i * 8));
    }
    double decoded = 0.0;
    std::memcpy(&decoded, &raw, sizeof(decoded));
    EXPECT_DOUBLE_EQ(decoded, value);
}

TEST(BytecodeTest, EmitShortRoundTrip) {
    BytecodeEmitter emitter;
    emitter.emitShort(0x1234);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 2u);
    uint16_t decoded = static_cast<uint16_t>(code[0]) | (static_cast<uint16_t>(code[1]) << 8);
    EXPECT_EQ(decoded, 0x1234);
}

// ===========================================================================
// String encoding
// ===========================================================================

TEST(BytecodeTest, EmitString) {
    BytecodeEmitter emitter;
    emitter.emitString("hello");
    auto& code = emitter.getCode();
    // 2 bytes for length + 5 bytes for characters
    ASSERT_EQ(code.size(), 7u);

    // Decode length
    uint16_t len = static_cast<uint16_t>(code[0]) | (static_cast<uint16_t>(code[1]) << 8);
    EXPECT_EQ(len, 5u);

    // Decode string
    std::string decoded(code.begin() + 2, code.begin() + 2 + len);
    EXPECT_EQ(decoded, "hello");
}

TEST(BytecodeTest, EmitEmptyString) {
    BytecodeEmitter emitter;
    emitter.emitString("");
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 2u);
    uint16_t len = static_cast<uint16_t>(code[0]) | (static_cast<uint16_t>(code[1]) << 8);
    EXPECT_EQ(len, 0u);
}

// ===========================================================================
// CurrentOffset
// ===========================================================================

TEST(BytecodeTest, CurrentOffset) {
    BytecodeEmitter emitter;
    EXPECT_EQ(emitter.currentOffset(), 0u);
    emitter.emit(OpCode::PUSH_INT);
    EXPECT_EQ(emitter.currentOffset(), 1u);
    emitter.emitInt(42);
    EXPECT_EQ(emitter.currentOffset(), 9u);
}

// ===========================================================================
// PatchJump
// ===========================================================================

TEST(BytecodeTest, PatchJump) {
    BytecodeEmitter emitter;
    emitter.emit(OpCode::JUMP);
    size_t jumpOffset = emitter.currentOffset();
    emitter.emitShort(0); // placeholder

    // Emit some more code
    emitter.emit(OpCode::HALT);

    // Patch the jump to point to the HALT instruction
    uint16_t target = static_cast<uint16_t>(emitter.currentOffset() - 1);
    emitter.patchJump(jumpOffset, target);

    auto& code = emitter.getCode();
    uint16_t patched = static_cast<uint16_t>(code[jumpOffset]) |
                       (static_cast<uint16_t>(code[jumpOffset + 1]) << 8);
    EXPECT_EQ(patched, target);
}

TEST(BytecodeTest, PatchJumpOutOfBounds) {
    BytecodeEmitter emitter;
    emitter.emitByte(0);
    EXPECT_THROW(emitter.patchJump(1, 0), std::runtime_error);
}

// ===========================================================================
// Multiple emissions
// ===========================================================================

TEST(BytecodeTest, FullPushIntSequence) {
    BytecodeEmitter emitter;
    emitter.emit(OpCode::PUSH_INT);
    emitter.emitInt(42);
    emitter.emit(OpCode::PUSH_INT);
    emitter.emitInt(10);
    emitter.emit(OpCode::ADD);

    auto& code = emitter.getCode();
    // 1 + 8 + 1 + 8 + 1 = 19
    EXPECT_EQ(code.size(), 19u);
    EXPECT_EQ(static_cast<OpCode>(code[0]), OpCode::PUSH_INT);
    EXPECT_EQ(static_cast<OpCode>(code[9]), OpCode::PUSH_INT);
    EXPECT_EQ(static_cast<OpCode>(code[18]), OpCode::ADD);
}

// ===========================================================================
// All OpCode values
// ===========================================================================

TEST(BytecodeTest, AllOpCodes) {
    // Verify all opcodes can be emitted without errors
    BytecodeEmitter emitter;
    emitter.emit(OpCode::PUSH_INT);
    emitter.emit(OpCode::PUSH_FLOAT);
    emitter.emit(OpCode::PUSH_STRING);
    emitter.emit(OpCode::POP);
    emitter.emit(OpCode::ADD);
    emitter.emit(OpCode::SUB);
    emitter.emit(OpCode::MUL);
    emitter.emit(OpCode::DIV);
    emitter.emit(OpCode::MOD);
    emitter.emit(OpCode::NEG);
    emitter.emit(OpCode::EQ);
    emitter.emit(OpCode::NE);
    emitter.emit(OpCode::LT);
    emitter.emit(OpCode::LE);
    emitter.emit(OpCode::GT);
    emitter.emit(OpCode::GE);
    emitter.emit(OpCode::AND);
    emitter.emit(OpCode::OR);
    emitter.emit(OpCode::NOT);
    emitter.emit(OpCode::LOAD_VAR);
    emitter.emit(OpCode::STORE_VAR);
    emitter.emit(OpCode::JUMP);
    emitter.emit(OpCode::JUMP_IF_FALSE);
    emitter.emit(OpCode::CALL);
    emitter.emit(OpCode::RETURN);
    emitter.emit(OpCode::HALT);
    EXPECT_EQ(emitter.currentOffset(), 26u);
}

TEST(BytecodeTest, EmitIntZero) {
    BytecodeEmitter emitter;
    emitter.emitInt(0);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 8u);
    for (int i = 0; i < 8; i++) {
        EXPECT_EQ(code[i], 0);
    }
}

TEST(BytecodeTest, EmitFloatZero) {
    BytecodeEmitter emitter;
    emitter.emitFloat(0.0);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 8u);
}

TEST(BytecodeTest, EmitShortZero) {
    BytecodeEmitter emitter;
    emitter.emitShort(0);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 2u);
    EXPECT_EQ(code[0], 0);
    EXPECT_EQ(code[1], 0);
}

TEST(BytecodeTest, EmitShortMax) {
    BytecodeEmitter emitter;
    emitter.emitShort(0xFFFF);
    auto& code = emitter.getCode();
    ASSERT_EQ(code.size(), 2u);
    uint16_t decoded = static_cast<uint16_t>(code[0]) | (static_cast<uint16_t>(code[1]) << 8);
    EXPECT_EQ(decoded, 0xFFFF);
}

// ===========================================================================
// String too long
// ===========================================================================

TEST(BytecodeTest, EmitStringTooLong) {
    BytecodeEmitter emitter;
    std::string longStr(70000, 'a');
    EXPECT_THROW(emitter.emitString(longStr), std::runtime_error);
}
