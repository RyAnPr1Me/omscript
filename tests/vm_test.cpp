#include <gtest/gtest.h>
#include "vm.h"
#include "bytecode.h"

using namespace omscript;

// Helper to build bytecode for a simple sequence of operations
static std::vector<uint8_t> buildBytecode(std::function<void(BytecodeEmitter&)> fn) {
    BytecodeEmitter emitter;
    fn(emitter);
    return emitter.getCode();
}

// ===========================================================================
// Basic push/return
// ===========================================================================

TEST(VMTest, PushIntAndReturn) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(42);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 42);
}

TEST(VMTest, PushFloatAndReturn) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitFloat(3.14);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 3.14);
}

TEST(VMTest, PushStringAndReturn) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitString("hello");
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_STREQ(vm.getLastReturn().asString(), "hello");
}

// ===========================================================================
// Arithmetic operations
// ===========================================================================

TEST(VMTest, AddIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(3);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(4);
        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 7);
}

TEST(VMTest, SubIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(10);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(3);
        e.emit(OpCode::SUB);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 7);
}

TEST(VMTest, MulIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(6);
        e.emit(OpCode::MUL);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 30);
}

TEST(VMTest, DivIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(10);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(3);
        e.emit(OpCode::DIV);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 3);
}

TEST(VMTest, ModIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(10);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(3);
        e.emit(OpCode::MOD);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, NegInteger) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::NEG);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), -5);
}

// ===========================================================================
// Comparison operations
// ===========================================================================

TEST(VMTest, EqualTrue) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::EQ);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, EqualFalse) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(3);
        e.emit(OpCode::EQ);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

TEST(VMTest, NotEqual) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(3);
        e.emit(OpCode::NE);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LessThan) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(3);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::LT);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LessOrEqual) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::LE);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, GreaterThan) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(3);
        e.emit(OpCode::GT);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, GreaterOrEqual) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::GE);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

// ===========================================================================
// Logical operations
// ===========================================================================

TEST(VMTest, LogicalAnd) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::AND);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LogicalAndFalse) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::AND);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

TEST(VMTest, LogicalOr) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::OR);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LogicalNot) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::NOT);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LogicalNotTrue) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::NOT);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

// ===========================================================================
// Stack operations
// ===========================================================================

TEST(VMTest, Pop) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(99);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(42);
        e.emit(OpCode::POP); // pop 42
        e.emit(OpCode::RETURN); // return 99
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 99);
}

// ===========================================================================
// Variables
// ===========================================================================

TEST(VMTest, StoreAndLoadVar) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(42);
        e.emit(OpCode::STORE_VAR);
        e.emitString("x");
        e.emit(OpCode::POP); // STORE_VAR pushes the value back
        e.emit(OpCode::LOAD_VAR);
        e.emitString("x");
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 42);
}

TEST(VMTest, SetGlobalBeforeExecute) {
    VM vm;
    vm.setGlobal("x", Value(int64_t(100)));
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_VAR);
        e.emitString("x");
        e.emit(OpCode::RETURN);
    });
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 100);
}

TEST(VMTest, GetGlobal) {
    VM vm;
    vm.setGlobal("x", Value(int64_t(55)));
    EXPECT_EQ(vm.getGlobal("x").asInt(), 55);
}

TEST(VMTest, GetUndefinedGlobal) {
    VM vm;
    EXPECT_THROW(vm.getGlobal("nonexistent"), std::runtime_error);
}

TEST(VMTest, LoadUndefinedVar) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_VAR);
        e.emitString("undef");
        e.emit(OpCode::RETURN);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

// ===========================================================================
// Control flow
// ===========================================================================

TEST(VMTest, Jump) {
    // PUSH 1, JUMP over PUSH 2, RETURN
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::JUMP);
        size_t jumpAddr = e.currentOffset();
        e.emitShort(0); // placeholder

        // This should be skipped
        e.emit(OpCode::PUSH_INT);
        e.emitInt(2);

        uint16_t target = static_cast<uint16_t>(e.currentOffset());
        e.patchJump(jumpAddr, target);

        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, JumpIfFalseSkips) {
    // PUSH 0, JUMP_IF_FALSE over PUSH 99, PUSH 42, RETURN
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0); // false condition
        e.emit(OpCode::JUMP_IF_FALSE);
        size_t jumpAddr = e.currentOffset();
        e.emitShort(0); // placeholder

        // This should be skipped
        e.emit(OpCode::PUSH_INT);
        e.emitInt(99);
        e.emit(OpCode::RETURN);

        uint16_t target = static_cast<uint16_t>(e.currentOffset());
        e.patchJump(jumpAddr, target);

        e.emit(OpCode::PUSH_INT);
        e.emitInt(42);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 42);
}

TEST(VMTest, JumpIfFalseDoesNotSkip) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1); // true condition
        e.emit(OpCode::JUMP_IF_FALSE);
        size_t jumpAddr = e.currentOffset();
        e.emitShort(0); // placeholder

        // This should NOT be skipped
        e.emit(OpCode::PUSH_INT);
        e.emitInt(99);
        e.emit(OpCode::RETURN);

        uint16_t target = static_cast<uint16_t>(e.currentOffset());
        e.patchJump(jumpAddr, target);

        e.emit(OpCode::PUSH_INT);
        e.emitInt(42);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 99);
}

// ===========================================================================
// Halt
// ===========================================================================

TEST(VMTest, Halt) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(42);
        e.emit(OpCode::HALT);
    });
    VM vm;
    vm.execute(code);
    // HALT returns a default NONE value
    EXPECT_EQ(vm.getLastReturn().getType(), Value::Type::NONE);
}

// ===========================================================================
// Return with empty stack
// ===========================================================================

TEST(VMTest, ReturnEmptyStack) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

// ===========================================================================
// Call opcode
// ===========================================================================

TEST(VMTest, CallPlaceholder) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(2);
        e.emit(OpCode::CALL);
        e.emitString("add");
        e.emitByte(2); // 2 arguments
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    // CALL pops args and pushes 0 as placeholder
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

// ===========================================================================
// Error cases
// ===========================================================================

TEST(VMTest, StackUnderflow) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::ADD); // needs 2 operands but stack is empty
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, UnknownOpcode) {
    std::vector<uint8_t> code = {0xFF}; // Invalid opcode
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, BytecodeReadOutOfBounds) {
    // PUSH_INT requires 8 bytes but we only provide the opcode
    std::vector<uint8_t> code = {static_cast<uint8_t>(OpCode::PUSH_INT)};
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

// ===========================================================================
// Float arithmetic in VM
// ===========================================================================

TEST(VMTest, AddFloats) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitFloat(1.5);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitFloat(2.5);
        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 4.0);
}

TEST(VMTest, SubFloats) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitFloat(5.0);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitFloat(2.5);
        e.emit(OpCode::SUB);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 2.5);
}

TEST(VMTest, NegFloat) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitFloat(3.14);
        e.emit(OpCode::NEG);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), -3.14);
}

// ===========================================================================
// String operations in VM
// ===========================================================================

TEST(VMTest, ConcatStrings) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitString("hello ");
        e.emit(OpCode::PUSH_STRING);
        e.emitString("world");
        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_STREQ(vm.getLastReturn().asString(), "hello world");
}

// ===========================================================================
// Division by zero in VM
// ===========================================================================

TEST(VMTest, DivByZero) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::DIV);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, ModByZero) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::MOD);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

// ===========================================================================
// Variable reassignment
// ===========================================================================

TEST(VMTest, VarReassignment) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(10);
        e.emit(OpCode::STORE_VAR);
        e.emitString("x");
        e.emit(OpCode::POP);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(20);
        e.emit(OpCode::STORE_VAR);
        e.emitString("x");
        e.emit(OpCode::POP);
        e.emit(OpCode::LOAD_VAR);
        e.emitString("x");
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 20);
}
