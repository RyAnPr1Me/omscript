#include "bytecode.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include <gtest/gtest.h>

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
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 42);
}

TEST(VMTest, PushFloatAndReturn) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(3.14);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 3.14);
}

TEST(VMTest, PushStringAndReturn) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(0);
        e.emitString("hello");
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_STREQ(vm.getLastReturn().asString(), "hello");
}

TEST(VMTest, PopOperation) {
    // POP should discard the top of stack
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(10);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(20);
        e.emit(OpCode::POP); // discards 20
        e.emit(OpCode::RETURN);
        e.emitReg(0); // returns 10
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 10);
}

TEST(VMTest, DupOperation) {
    // DUP duplicates the top of stack
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::DUP);
        e.emit(OpCode::ADD);
        e.emitReg(1);
        e.emitReg(0);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 84); // 42 + 42
}

// ===========================================================================
// Arithmetic operations
// ===========================================================================

TEST(VMTest, AddIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(3);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(4);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 7);
}

TEST(VMTest, SubIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(10);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(3);
        e.emit(OpCode::SUB);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 7);
}

TEST(VMTest, MulIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(6);
        e.emit(OpCode::MUL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 30);
}

TEST(VMTest, DivIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(10);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(3);
        e.emit(OpCode::DIV);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 3);
}

TEST(VMTest, ModIntegers) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(10);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(3);
        e.emit(OpCode::MOD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, NegInteger) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::NEG);
        e.emitReg(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
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
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(5);
        e.emit(OpCode::EQ);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, EqualFalse) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(3);
        e.emit(OpCode::EQ);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

TEST(VMTest, NotEqual) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(3);
        e.emit(OpCode::NE);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LessThan) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(3);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(5);
        e.emit(OpCode::LT);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LessOrEqual) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(5);
        e.emit(OpCode::LE);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, GreaterThan) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(3);
        e.emit(OpCode::GT);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, GreaterOrEqual) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(5);
        e.emit(OpCode::GE);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
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
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(1);
        e.emit(OpCode::AND);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LogicalAndFalse) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0);
        e.emit(OpCode::AND);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

TEST(VMTest, LogicalOr) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(1);
        e.emit(OpCode::OR);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LogicalNot) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0);
        e.emit(OpCode::NOT);
        e.emitReg(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, LogicalNotTrue) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::NOT);
        e.emitReg(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

// ===========================================================================
// Variables
// ===========================================================================

TEST(VMTest, StoreAndLoadVar) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::STORE_VAR);
        e.emitReg(0);
        e.emitString("x");
        e.emit(OpCode::LOAD_VAR);
        e.emitReg(1);
        e.emitString("x");
        e.emit(OpCode::RETURN);
        e.emitReg(1);
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
        e.emitReg(0);
        e.emitString("x");
        e.emit(OpCode::RETURN);
        e.emitReg(0);
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
        e.emitReg(0);
        e.emitString("undef");
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

// ===========================================================================
// Control flow
// ===========================================================================

TEST(VMTest, Jump) {
    // PUSH_INT r0, 1; JUMP over PUSH_INT r0, 2; RETURN r0
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::JUMP);
        size_t jumpAddr = e.currentOffset();
        e.emitShort(0); // placeholder

        // This should be skipped
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(2);

        uint16_t target = static_cast<uint16_t>(e.currentOffset());
        e.patchJump(jumpAddr, target);

        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, JumpIfFalseSkips) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0); // false
        e.emit(OpCode::JUMP_IF_FALSE);
        e.emitReg(0);
        size_t jumpAddr = e.currentOffset();
        e.emitShort(0); // placeholder

        // This should be skipped
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(99);
        e.emit(OpCode::RETURN);
        e.emitReg(0);

        uint16_t target = static_cast<uint16_t>(e.currentOffset());
        e.patchJump(jumpAddr, target);

        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 42);
}

TEST(VMTest, JumpIfFalseDoesNotSkip) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1); // true
        e.emit(OpCode::JUMP_IF_FALSE);
        e.emitReg(0);
        size_t jumpAddr = e.currentOffset();
        e.emitShort(0); // placeholder

        // This should NOT be skipped
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(99);
        e.emit(OpCode::RETURN);
        e.emitReg(0);

        uint16_t target = static_cast<uint16_t>(e.currentOffset());
        e.patchJump(jumpAddr, target);

        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
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
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::HALT);
    });
    VM vm;
    vm.execute(code);
    // HALT returns a default NONE value
    EXPECT_EQ(vm.getLastReturn().getType(), Value::Type::NONE);
}

// ===========================================================================
// Return with default register value
// ===========================================================================

TEST(VMTest, ReturnEmptyStack) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

// ===========================================================================
// Call opcode – full function dispatch
// ===========================================================================

TEST(VMTest, CallSimpleFunction) {
    // Register a function "add" that loads its two local args and adds them.
    BytecodeFunction addFunc;
    addFunc.name = "add";
    addFunc.arity = 2;
    addFunc.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(1);
        e.emitByte(1);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    // Main bytecode: push 3 and 4, call add(3, 4), return result.
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(3);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(4);
        e.emit(OpCode::CALL);
        e.emitReg(2);
        e.emitString("add");
        e.emitByte(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(addFunc);
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 7);
}

TEST(VMTest, CallFunctionNoArgs) {
    // Register a zero-argument function that returns 42.
    BytecodeFunction constFunc;
    constFunc.name = "answer";
    constFunc.arity = 0;
    constFunc.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::CALL);
        e.emitReg(0);
        e.emitString("answer");
        e.emitByte(0);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });

    VM vm;
    vm.registerFunction(constFunc);
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 42);
}

TEST(VMTest, CallNestedFunctions) {
    // "double_it" multiplies its arg by 2
    BytecodeFunction doubleIt;
    doubleIt.name = "double_it";
    doubleIt.arity = 1;
    doubleIt.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(2);
        e.emit(OpCode::MUL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    // "quad" calls double_it twice: double_it(double_it(x))
    BytecodeFunction quad;
    quad.name = "quad";
    quad.arity = 1;
    quad.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::CALL);
        e.emitReg(1);
        e.emitString("double_it");
        e.emitByte(1);
        e.emitReg(0);
        e.emit(OpCode::CALL);
        e.emitReg(2);
        e.emitString("double_it");
        e.emitByte(1);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::CALL);
        e.emitReg(1);
        e.emitString("quad");
        e.emitByte(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });

    VM vm;
    vm.registerFunction(doubleIt);
    vm.registerFunction(quad);
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 20); // 5 * 2 * 2 = 20
}

TEST(VMTest, CallUndefinedFunction) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::CALL);
        e.emitReg(0);
        e.emitString("nonexistent");
        e.emitByte(0);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, CallArityMismatch) {
    BytecodeFunction func;
    func.name = "f";
    func.arity = 2;
    func.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::CALL);
        e.emitReg(1);
        e.emitString("f");
        e.emitByte(1);
        e.emitReg(0);
    });

    VM vm;
    vm.registerFunction(func);
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, CallPreservesGlobals) {
    // A function that sets a global and returns.
    BytecodeFunction setter;
    setter.name = "set_x";
    setter.arity = 1;
    setter.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::STORE_VAR);
        e.emitReg(0);
        e.emitString("x");
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(99);
        e.emit(OpCode::CALL);
        e.emitReg(1);
        e.emitString("set_x");
        e.emitByte(1);
        e.emitReg(0);
        e.emit(OpCode::LOAD_VAR);
        e.emitReg(2);
        e.emitString("x");
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(setter);
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 99);
}

TEST(VMTest, LoadLocalAndStoreLocal) {
    // Register a function that modifies a local and returns it.
    BytecodeFunction func;
    func.name = "inc";
    func.arity = 1;
    func.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(10);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::STORE_LOCAL);
        e.emitByte(0);
        e.emitReg(2);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(3);
        e.emitByte(0);
        e.emit(OpCode::RETURN);
        e.emitReg(3);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::CALL);
        e.emitReg(1);
        e.emitString("inc");
        e.emitByte(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });

    VM vm;
    vm.registerFunction(func);
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 15); // 5 + 10
}

TEST(VMTest, LoadLocalOutOfRange) {
    BytecodeFunction func;
    func.name = "bad";
    func.arity = 0;
    func.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(5); // no locals
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::CALL);
        e.emitReg(0);
        e.emitString("bad");
        e.emitByte(0);
    });

    VM vm;
    vm.registerFunction(func);
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

// ===========================================================================
// Error cases
// ===========================================================================

TEST(VMTest, StackUnderflow) {
    // In register-based VM, adding two uninitialized (NONE) registers throws.
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::ADD);
        e.emitReg(0);
        e.emitReg(0);
        e.emitReg(0);
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
    // PUSH_INT requires rd(1B) + value(8B) but we only provide the opcode
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
        e.emitReg(0);
        e.emitFloat(1.5);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(1);
        e.emitFloat(2.5);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 4.0);
}

TEST(VMTest, SubFloats) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(5.0);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(1);
        e.emitFloat(2.5);
        e.emit(OpCode::SUB);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 2.5);
}

TEST(VMTest, NegFloat) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(3.14);
        e.emit(OpCode::NEG);
        e.emitReg(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
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
        e.emitReg(0);
        e.emitString("hello ");
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(1);
        e.emitString("world");
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
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
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0);
        e.emit(OpCode::DIV);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, ModByZero) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0);
        e.emit(OpCode::MOD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
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
        e.emitReg(0);
        e.emitInt(10);
        e.emit(OpCode::STORE_VAR);
        e.emitReg(0);
        e.emitString("x");
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(20);
        e.emit(OpCode::STORE_VAR);
        e.emitReg(0);
        e.emitString("x");
        e.emit(OpCode::LOAD_VAR);
        e.emitReg(1);
        e.emitString("x");
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 20);
}

// ===========================================================================
// Jump out of bounds
// ===========================================================================

TEST(VMTest, JumpOutOfBounds) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::JUMP);
        e.emitShort(0xFFFF); // way past end
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, JumpIfFalseOutOfBounds) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0);
        e.emit(OpCode::JUMP_IF_FALSE);
        e.emitReg(0);
        e.emitShort(0xFFFF); // way past end
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

// ===========================================================================
// STORE_VAR preserves register value
// ===========================================================================

TEST(VMTest, StoreVarLeavesValueOnStack) {
    // In register-based VM, STORE_VAR reads from a register without modifying it.
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(99);
        e.emit(OpCode::STORE_VAR);
        e.emitReg(0);
        e.emitString("x");
        e.emit(OpCode::LOAD_VAR);
        e.emitReg(1);
        e.emitString("x");
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 99);
}

// ===========================================================================
// Nested CALL preserves caller registers
// ===========================================================================

TEST(VMTest, CallPreservesCallerStack) {
    // identity(x) just returns its argument.
    auto identityCode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });

    // Caller: r0=100, call identity(42)->r2, ADD r0+r2->r3, return r3
    auto callerCode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(100);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(42);
        e.emit(OpCode::CALL);
        e.emitReg(2);
        e.emitString("identity");
        e.emitByte(1);
        e.emitReg(1);
        e.emit(OpCode::ADD);
        e.emitReg(3);
        e.emitReg(0);
        e.emitReg(2);
        e.emit(OpCode::RETURN);
        e.emitReg(3);
    });

    VM vm;
    BytecodeFunction identityFn;
    identityFn.name = "identity";
    identityFn.arity = 1;
    identityFn.bytecode = identityCode;
    vm.registerFunction(identityFn);

    vm.execute(callerCode);
    EXPECT_EQ(vm.getLastReturn().asInt(), 142);
}

// ===========================================================================
// Multiple nested calls preserve all intermediate values
// ===========================================================================

TEST(VMTest, MultipleCallsPreserveStack) {
    // 10 + id(20) + id(30) = 60
    auto identityCode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });

    auto callerCode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(10);

        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(20);
        e.emit(OpCode::CALL);
        e.emitReg(2);
        e.emitString("id");
        e.emitByte(1);
        e.emitReg(1);

        e.emit(OpCode::ADD);
        e.emitReg(3);
        e.emitReg(0);
        e.emitReg(2);

        e.emit(OpCode::PUSH_INT);
        e.emitReg(4);
        e.emitInt(30);
        e.emit(OpCode::CALL);
        e.emitReg(5);
        e.emitString("id");
        e.emitByte(1);
        e.emitReg(4);

        e.emit(OpCode::ADD);
        e.emitReg(6);
        e.emitReg(3);
        e.emitReg(5);
        e.emit(OpCode::RETURN);
        e.emitReg(6);
    });

    VM vm;
    BytecodeFunction idFn;
    idFn.name = "id";
    idFn.arity = 1;
    idFn.bytecode = identityCode;
    vm.registerFunction(idFn);

    vm.execute(callerCode);
    EXPECT_EQ(vm.getLastReturn().asInt(), 60);
}

// ===========================================================================
// Call depth limit
// ===========================================================================

TEST(VMTest, CallDepthLimitThrows) {
    // A function that calls itself unconditionally will hit the call depth limit.
    BytecodeEmitter body;
    body.emit(OpCode::CALL);
    body.emitReg(0);
    body.emitString("recurse");
    body.emitByte(0);
    body.emit(OpCode::RETURN);
    body.emitReg(0);

    BytecodeFunction fn;
    fn.name = "recurse";
    fn.arity = 0;
    fn.bytecode = body.getCode();

    BytecodeEmitter caller;
    caller.emit(OpCode::CALL);
    caller.emitReg(0);
    caller.emitString("recurse");
    caller.emitByte(0);
    caller.emit(OpCode::HALT);

    VM vm;
    vm.registerFunction(fn);
    EXPECT_THROW(vm.execute(caller.getCode()), std::runtime_error);
}

// ===========================================================================
// Bitwise operations in VM
// ===========================================================================

TEST(VMTest, BitwiseAnd) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0xFF);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0x0F);
        e.emit(OpCode::BIT_AND);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0x0F);
}

TEST(VMTest, BitwiseOr) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0xF0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0x0F);
        e.emit(OpCode::BIT_OR);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0xFF);
}

TEST(VMTest, BitwiseXor) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0xFF);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0x0F);
        e.emit(OpCode::BIT_XOR);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0xF0);
}

TEST(VMTest, BitwiseNot) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0);
        e.emit(OpCode::BIT_NOT);
        e.emitReg(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), ~static_cast<int64_t>(0));
}

TEST(VMTest, ShiftLeft) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(4);
        e.emit(OpCode::SHL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 16);
}

TEST(VMTest, ShiftRight) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(16);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(4);
        e.emit(OpCode::SHR);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, BitwiseAndOnFloatThrows) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(1.0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(1);
        e.emit(OpCode::BIT_AND);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, ShiftLeftOutOfRange) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(64);
        e.emit(OpCode::SHL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

// ===========================================================================
// MOV opcode (replaces DUP for register-based VM)
// ===========================================================================

TEST(VMTest, Dup) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::MOV);
        e.emitReg(1);
        e.emitReg(0);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 84);
}

TEST(VMTest, DupString) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(0);
        e.emitString("hello");
        e.emit(OpCode::MOV);
        e.emitReg(1);
        e.emitReg(0);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_STREQ(vm.getLastReturn().asString(), "hellohello");
}

// ===========================================================================
// PRINT opcode
// ===========================================================================

TEST(VMTest, PrintInt) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::PRINT);
        e.emitReg(0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    VM vm;
    // Redirect stdout to check output
    testing::internal::CaptureStdout();
    vm.execute(code);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "42\n");
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

TEST(VMTest, PrintString) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(0);
        e.emitString("hello world");
        e.emit(OpCode::PRINT);
        e.emitReg(0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    VM vm;
    testing::internal::CaptureStdout();
    vm.execute(code);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "hello world\n");
}

// ===========================================================================
// JIT compilation
// ===========================================================================

#include "jit.h"

// Helper: build a BytecodeFunction from a lambda.
static BytecodeFunction makeBytecodeFunc(const std::string& name, uint8_t arity,
                                         std::function<void(BytecodeEmitter&)> fn) {
    BytecodeEmitter emitter;
    fn(emitter);
    BytecodeFunction f;
    f.name = name;
    f.arity = arity;
    f.bytecode = emitter.getCode();
    return f;
}

TEST(VMTest, JITSimpleAdd) {
    // fn add(a, b) { return a + b; }
    auto addFunc = makeBytecodeFunc("add", 2, [](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(1);
        e.emitByte(1);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(addFunc);

    // Call more than JIT threshold times — the function should be JIT-compiled.
    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT);
            e.emitReg(0);
            e.emitInt(3);
            e.emit(OpCode::PUSH_INT);
            e.emitReg(1);
            e.emitInt(4);
            e.emit(OpCode::CALL);
            e.emitReg(2);
            e.emitString("add");
            e.emitByte(2);
            e.emitReg(0);
            e.emitReg(1);
            e.emit(OpCode::RETURN);
            e.emitReg(2);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), 7);
    }

    // After enough calls, the function should be JIT-compiled.
    EXPECT_TRUE(vm.isJITCompiled("add"));
}

TEST(VMTest, JITWithControlFlow) {
    // fn abs(x) { if (x < 0) return -x; return x; }
    auto absFunc = makeBytecodeFunc("myabs", 1, [](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0);
        e.emit(OpCode::LT);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::JUMP_IF_FALSE);
        e.emitReg(2);
        size_t patch = e.currentOffset();
        e.emitShort(0); // placeholder

        // if-body: return -x
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(3);
        e.emitByte(0);
        e.emit(OpCode::NEG);
        e.emitReg(4);
        e.emitReg(3);
        e.emit(OpCode::RETURN);
        e.emitReg(4);

        // after-if: return x
        e.patchJump(patch, static_cast<uint16_t>(e.currentOffset()));
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(5);
        e.emitByte(0);
        e.emit(OpCode::RETURN);
        e.emitReg(5);
    });

    VM vm;
    vm.registerFunction(absFunc);

    // Warm up with positive value to trigger JIT.
    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT);
            e.emitReg(0);
            e.emitInt(5);
            e.emit(OpCode::CALL);
            e.emitReg(1);
            e.emitString("myabs");
            e.emitByte(1);
            e.emitReg(0);
            e.emit(OpCode::RETURN);
            e.emitReg(1);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), 5);
    }

    EXPECT_TRUE(vm.isJITCompiled("myabs"));

    // Now test with negative value (through the JIT path).
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(-7);
        e.emit(OpCode::CALL);
        e.emitReg(1);
        e.emitString("myabs");
        e.emitByte(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 7);
}

TEST(VMTest, JITFallbackForStrings) {
    // A function that uses PRINT — not JIT-eligible.
    auto printFunc = makeBytecodeFunc("printer", 0, [](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(0);
        e.emitString("hello");
        e.emit(OpCode::PRINT);
        e.emitReg(0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });

    VM vm;
    vm.registerFunction(printFunc);

    // Call many times — should NOT be JIT-compiled (uses PRINT + STRING).
    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::CALL);
            e.emitReg(0);
            e.emitString("printer");
            e.emitByte(0);
            e.emit(OpCode::RETURN);
            e.emitReg(0);
        });
        testing::internal::CaptureStdout();
        vm.execute(code);
        testing::internal::GetCapturedStdout();
        EXPECT_EQ(vm.getLastReturn().asInt(), 0);
    }

    // Should NOT be JIT-compiled.
    EXPECT_FALSE(vm.isJITCompiled("printer"));
}

TEST(VMTest, JITMultiply) {
    // fn mul(a, b) { return a * b; }
    auto mulFunc = makeBytecodeFunc("mul", 2, [](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(1);
        e.emitByte(1);
        e.emit(OpCode::MUL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(mulFunc);

    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT);
            e.emitReg(0);
            e.emitInt(6);
            e.emit(OpCode::PUSH_INT);
            e.emitReg(1);
            e.emitInt(7);
            e.emit(OpCode::CALL);
            e.emitReg(2);
            e.emitString("mul");
            e.emitByte(2);
            e.emitReg(0);
            e.emitReg(1);
            e.emit(OpCode::RETURN);
            e.emitReg(2);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), 42);
    }
    EXPECT_TRUE(vm.isJITCompiled("mul"));
}

TEST(VMTest, JITWithLocals) {
    // fn compute(x) { var y = x * 2; return y + 1; }
    auto func = makeBytecodeFunc("compute", 1, [](BytecodeEmitter& e) {
        // y = x * 2
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0); // r0 = x
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(2); // r1 = 2
        e.emit(OpCode::MUL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1); // r2 = x * 2
        e.emit(OpCode::STORE_LOCAL);
        e.emitByte(1);
        e.emitReg(2); // local[1] = y
        // return y + 1
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(3);
        e.emitByte(1); // r3 = y
        e.emit(OpCode::PUSH_INT);
        e.emitReg(4);
        e.emitInt(1); // r4 = 1
        e.emit(OpCode::ADD);
        e.emitReg(5);
        e.emitReg(3);
        e.emitReg(4); // r5 = y + 1
        e.emit(OpCode::RETURN);
        e.emitReg(5);
    });

    VM vm;
    vm.registerFunction(func);

    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT);
            e.emitReg(0);
            e.emitInt(10);
            e.emit(OpCode::CALL);
            e.emitReg(1);
            e.emitString("compute");
            e.emitByte(1);
            e.emitReg(0);
            e.emit(OpCode::RETURN);
            e.emitReg(1);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), 21); // 10*2+1
    }
    EXPECT_TRUE(vm.isJITCompiled("compute"));
}

TEST(VMTest, JITRecompileAfterThreshold) {
    // fn doubler(x) { return x * 2; }
    auto func = makeBytecodeFunc("doubler", 1, [](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(2);
        e.emit(OpCode::MUL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(func);

    // Warm up past JIT threshold + recompile threshold.
    size_t totalCalls = BytecodeJIT::kJITThreshold + BytecodeJIT::kRecompileThreshold + 5;
    for (size_t i = 0; i < totalCalls; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT);
            e.emitReg(0);
            e.emitInt(7);
            e.emit(OpCode::CALL);
            e.emitReg(1);
            e.emitString("doubler");
            e.emitByte(1);
            e.emitReg(0);
            e.emit(OpCode::RETURN);
            e.emitReg(1);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), 14);
    }
    EXPECT_TRUE(vm.isJITCompiled("doubler"));
}

TEST(VMTest, JITGetCallCount) {
    BytecodeJIT jit;
    EXPECT_EQ(jit.getCallCount("foo"), 0u);
    jit.recordCall("foo");
    EXPECT_EQ(jit.getCallCount("foo"), 1u);
    jit.recordCall("foo");
    EXPECT_EQ(jit.getCallCount("foo"), 2u);
}

// ===========================================================================
// Hybrid compilation — execute bytecode from generateHybrid()
// ===========================================================================

// Helper: run hybrid codegen and register resulting bytecode functions with a VM.
static void hybridCompileAndRegister(const std::string& source, VM& vm, CodeGenerator& codegen) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateHybrid(program.get());

    for (auto& bcFunc : codegen.getBytecodeFunctions()) {
        BytecodeFunction f;
        f.name = bcFunc.name;
        f.arity = bcFunc.arity;
        f.bytecode = bcFunc.bytecode;
        vm.registerFunction(f);
    }
}

TEST(VMTest, HybridBytecodeExecutesCorrectly) {
    CodeGenerator codegen;
    VM vm;
    hybridCompileAndRegister(R"(
        fn compute(x, y) { return x + y; }
        fn main() { return compute(3, 4); }
    )",
                             vm, codegen);

    EXPECT_EQ(codegen.getFunctionTier("compute"), ExecutionTier::Interpreted);
    EXPECT_TRUE(codegen.hasHybridBytecodeFunctions());

    // Build main bytecode that calls compute(10, 20)
    auto mainCode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(10);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(20);
        e.emit(OpCode::CALL);
        e.emitReg(2);
        e.emitString("compute");
        e.emitByte(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    vm.execute(mainCode);
    EXPECT_EQ(vm.getLastReturn().asInt(), 30);
}

TEST(VMTest, HybridMultipleFunctionsExecute) {
    CodeGenerator codegen;
    VM vm;
    hybridCompileAndRegister(R"(
        fn doubler(x) { return x * 2; }
        fn adder(a, b) { return a + b; }
        fn main() { return adder(doubler(3), 4); }
    )",
                             vm, codegen);

    EXPECT_EQ(codegen.getBytecodeFunctions().size(), 2u);

    // Call doubler(5) => 10
    auto code1 = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::CALL);
        e.emitReg(1);
        e.emitString("doubler");
        e.emitByte(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    vm.execute(code1);
    EXPECT_EQ(vm.getLastReturn().asInt(), 10);

    // Call adder(3, 7) => 10
    auto code2 = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(3);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(7);
        e.emit(OpCode::CALL);
        e.emitReg(2);
        e.emitString("adder");
        e.emitByte(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    vm.execute(code2);
    EXPECT_EQ(vm.getLastReturn().asInt(), 10);
}

TEST(VMTest, HybridAOTFunctionNotInBytecode) {
    // Fully typed functions should NOT produce bytecode
    CodeGenerator codegen;
    VM vm;
    hybridCompileAndRegister(R"(
        fn typed_add(a: int, b: int) { return a + b; }
        fn main() { return typed_add(1, 2); }
    )",
                             vm, codegen);

    EXPECT_EQ(codegen.getFunctionTier("typed_add"), ExecutionTier::AOT);
    EXPECT_FALSE(codegen.hasHybridBytecodeFunctions());
}

// ===========================================================================
// JUMP_IF_FALSE integer fast path
// ===========================================================================

TEST(VMTest, JumpIfFalseIntegerFastPath) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0); // false
        e.emit(OpCode::JUMP_IF_FALSE);
        e.emitReg(0);
        size_t patch = e.currentOffset();
        e.emitShort(0); // placeholder

        // true branch (should be skipped)
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(99);
        e.emit(OpCode::RETURN);
        e.emitReg(0);

        // false branch
        e.patchJump(patch, static_cast<uint16_t>(e.currentOffset()));
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 42);
}

TEST(VMTest, JumpIfFalseIntegerTruthy) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1); // true
        e.emit(OpCode::JUMP_IF_FALSE);
        e.emitReg(0);
        size_t patch = e.currentOffset();
        e.emitShort(0);

        // true branch
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(99);
        e.emit(OpCode::RETURN);
        e.emitReg(0);

        // false branch (should not reach)
        e.patchJump(patch, static_cast<uint16_t>(e.currentOffset()));
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 99);
}

// ===========================================================================
// JIT minimum bytecode size heuristic
// ===========================================================================

TEST(VMTest, JITSkipsTinyFunctions) {
    // A function with just RETURN (1 byte) should be too small for JIT.
    auto tinyFunc = makeBytecodeFunc("tiny", 0, [](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });

    BytecodeJIT jit;
    // Tiny functions (< kMinBytecodeSize opcodes) should fail to compile.
    bool compiled = jit.compile(tinyFunc);
    // The function bytecode is 10 bytes (1 opcode + 8 data + 1 RETURN),
    // which is >= kMinBytecodeSize (4), so it should compile successfully.
    if (tinyFunc.bytecode.size() >= BytecodeJIT::kMinBytecodeSize) {
        EXPECT_TRUE(compiled);
    } else {
        EXPECT_FALSE(compiled);
    }
}

// ===========================================================================
// JIT cached pointer reuse
// ===========================================================================

TEST(VMTest, JITCachedPointerReuse) {
    // Verify that after JIT compilation, subsequent calls use the cached path.
    auto addFunc = makeBytecodeFunc("cached_add", 2, [](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(1);
        e.emitByte(1);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(addFunc);

    // Call enough times to trigger JIT, then call more to exercise cached path.
    for (size_t i = 0; i < BytecodeJIT::kJITThreshold + 10; i++) {
        auto code = buildBytecode([&](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT);
            e.emitReg(0);
            e.emitInt(static_cast<int64_t>(i));
            e.emit(OpCode::PUSH_INT);
            e.emitReg(1);
            e.emitInt(1);
            e.emit(OpCode::CALL);
            e.emitReg(2);
            e.emitString("cached_add");
            e.emitByte(2);
            e.emitReg(0);
            e.emitReg(1);
            e.emit(OpCode::RETURN);
            e.emitReg(2);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), static_cast<int64_t>(i) + 1);
    }
    EXPECT_TRUE(vm.isJITCompiled("cached_add"));
}

// ===========================================================================
// Float JIT compilation
// ===========================================================================

TEST(VMTest, FloatJITSimpleAdd) {
    // Register a function that adds two floats.
    BytecodeFunction addFunc;
    addFunc.name = "fadd";
    addFunc.arity = 2;
    addFunc.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(1);
        e.emitByte(1);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(addFunc);

    // Call with float args enough times to trigger float-specialized JIT.
    for (size_t i = 0; i < BytecodeJIT::kJITThreshold + 3; i++) {
        auto code = buildBytecode([&](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_FLOAT);
            e.emitReg(0);
            e.emitFloat(1.5);
            e.emit(OpCode::PUSH_FLOAT);
            e.emitReg(1);
            e.emitFloat(2.5);
            e.emit(OpCode::CALL);
            e.emitReg(2);
            e.emitString("fadd");
            e.emitByte(2);
            e.emitReg(0);
            e.emitReg(1);
            e.emit(OpCode::RETURN);
            e.emitReg(2);
        });
        vm.execute(code);
        EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 4.0);
    }
    EXPECT_TRUE(vm.isJITCompiled("fadd"));
}

TEST(VMTest, FloatJITMulSub) {
    // Register: (a * b) - a
    BytecodeFunction func;
    func.name = "fmulsub";
    func.arity = 2;
    func.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(1);
        e.emitByte(1);
        e.emit(OpCode::MUL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(3);
        e.emitByte(0);
        e.emit(OpCode::SUB);
        e.emitReg(4);
        e.emitReg(2);
        e.emitReg(3);
        e.emit(OpCode::RETURN);
        e.emitReg(4);
    });

    VM vm;
    vm.registerFunction(func);

    for (size_t i = 0; i < BytecodeJIT::kJITThreshold + 3; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_FLOAT);
            e.emitReg(0);
            e.emitFloat(3.0);
            e.emit(OpCode::PUSH_FLOAT);
            e.emitReg(1);
            e.emitFloat(4.0);
            e.emit(OpCode::CALL);
            e.emitReg(2);
            e.emitString("fmulsub");
            e.emitByte(2);
            e.emitReg(0);
            e.emitReg(1);
            e.emit(OpCode::RETURN);
            e.emitReg(2);
        });
        vm.execute(code);
        // (3.0 * 4.0) - 3.0 = 9.0
        EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 9.0);
    }
    EXPECT_TRUE(vm.isJITCompiled("fmulsub"));
}

TEST(VMTest, TypeProfileRecordsIntCalls) {
    // Register a simple add function.
    BytecodeFunction addFunc;
    addFunc.name = "typed_add";
    addFunc.arity = 2;
    addFunc.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(1);
        e.emitByte(1);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(addFunc);

    // Call with int args — should record int type profile.
    for (size_t i = 0; i < BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT);
            e.emitReg(0);
            e.emitInt(10);
            e.emit(OpCode::PUSH_INT);
            e.emitReg(1);
            e.emitInt(20);
            e.emit(OpCode::CALL);
            e.emitReg(2);
            e.emitString("typed_add");
            e.emitByte(2);
            e.emitReg(0);
            e.emitReg(1);
            e.emit(OpCode::RETURN);
            e.emitReg(2);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), 30);
    }
    // After enough calls, should be JIT-compiled with int specialization.
    EXPECT_TRUE(vm.isJITCompiled("typed_add"));
}

TEST(VMTest, TypeProfileRecordsFloatCalls) {
    // Same function, called with floats.
    BytecodeFunction addFunc;
    addFunc.name = "typed_fadd";
    addFunc.arity = 2;
    addFunc.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(1);
        e.emitByte(1);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(addFunc);

    for (size_t i = 0; i < BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_FLOAT);
            e.emitReg(0);
            e.emitFloat(1.1);
            e.emit(OpCode::PUSH_FLOAT);
            e.emitReg(1);
            e.emitFloat(2.2);
            e.emit(OpCode::CALL);
            e.emitReg(2);
            e.emitString("typed_fadd");
            e.emitByte(2);
            e.emitReg(0);
            e.emitReg(1);
            e.emit(OpCode::RETURN);
            e.emitReg(2);
        });
        vm.execute(code);
        EXPECT_NEAR(vm.getLastReturn().asFloat(), 3.3, 0.001);
    }
    EXPECT_TRUE(vm.isJITCompiled("typed_fadd"));
}

TEST(VMTest, FloatJITWithPushFloat) {
    // A function that uses PUSH_FLOAT constants inside the body.
    BytecodeFunction func;
    func.name = "fconst";
    func.arity = 1;
    func.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(1);
        e.emitFloat(10.0);
        e.emit(OpCode::MUL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });

    VM vm;
    vm.registerFunction(func);

    for (size_t i = 0; i < BytecodeJIT::kJITThreshold + 3; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_FLOAT);
            e.emitReg(0);
            e.emitFloat(5.0);
            e.emit(OpCode::CALL);
            e.emitReg(1);
            e.emitString("fconst");
            e.emitByte(1);
            e.emitReg(0);
            e.emit(OpCode::RETURN);
            e.emitReg(1);
        });
        vm.execute(code);
        EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 50.0);
    }
    EXPECT_TRUE(vm.isJITCompiled("fconst"));
}

// ===========================================================================
// Hybrid compiler integration
// ===========================================================================

TEST(VMTest, HybridCompilerProducesBytecode) {
    // Verify that generateHybrid produces bytecode for untyped functions.
    std::string source = R"(
        fn add(a, b) {
            return a + b;
        }
        fn main() {
            return add(1, 2);
        }
    )";
    omscript::Lexer lexer(source);
    auto tokens = lexer.tokenize();
    omscript::Parser parser(tokens);
    auto program = parser.parse();

    omscript::CodeGenerator codegen(omscript::OptimizationLevel::O0);
    codegen.generateHybrid(program.get());

    // 'add' has no type annotations → should be Interpreted tier.
    EXPECT_EQ(codegen.getFunctionTier("add"), omscript::ExecutionTier::Interpreted);
    // 'main' is always AOT.
    EXPECT_EQ(codegen.getFunctionTier("main"), omscript::ExecutionTier::AOT);
    // Hybrid mode should have produced bytecode for 'add'.
    EXPECT_TRUE(codegen.hasHybridBytecodeFunctions());
}

// ===========================================================================
// Edge cases - register operations
// ===========================================================================

TEST(VMTest, RegisterPreservationAcrossCalls) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        // Set reg 0 to 10
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(10);
        // Call function that modifies registers
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(99);
        // Return original reg 0 value
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 10);
}

TEST(VMTest, ManyLocals) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        // Store values in many local slots
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(1);
        e.emit(OpCode::STORE_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(2);
        e.emit(OpCode::STORE_LOCAL);
        e.emitReg(1);
        e.emitByte(1);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(2);
        e.emitInt(3);
        e.emit(OpCode::STORE_LOCAL);
        e.emitReg(2);
        e.emitByte(2);
        // Load them back and sum
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(3);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(4);
        e.emitByte(1);
        e.emit(OpCode::ADD);
        e.emitReg(5);
        e.emitReg(3);
        e.emitReg(4);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(6);
        e.emitByte(2);
        e.emit(OpCode::ADD);
        e.emitReg(7);
        e.emitReg(5);
        e.emitReg(6);
        e.emit(OpCode::RETURN);
        e.emitReg(7);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 6);
}

TEST(VMTest, DuplicateLoadLocal) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(42);
        e.emit(OpCode::STORE_LOCAL);
        e.emitReg(0);
        e.emitByte(0);
        // Load same local twice into different registers
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(1);
        e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(2);
        e.emitByte(0);
        e.emit(OpCode::ADD);
        e.emitReg(3);
        e.emitReg(1);
        e.emitReg(2);
        e.emit(OpCode::RETURN);
        e.emitReg(3);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 84);
}

TEST(VMTest, DuplicateStoreLocal) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        // Store 10 to local 0
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(10);
        e.emit(OpCode::STORE_LOCAL);
        e.emitByte(0);
        e.emitReg(0);
        // Store 20 to same local 0
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(20);
        e.emit(OpCode::STORE_LOCAL);
        e.emitByte(0);
        e.emitReg(1);
        // Load should get 20
        e.emit(OpCode::LOAD_LOCAL);
        e.emitReg(2);
        e.emitByte(0);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 20);
}

// ===========================================================================
// Edge cases - string operations
// ===========================================================================

TEST(VMTest, StringInRegister) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(0);
        e.emitString("hello");
        e.emit(OpCode::RETURN);
        e.emitReg(0);
    });
    VM vm;
    vm.execute(code);
    EXPECT_STREQ(vm.getLastReturn().asString(), "hello");
}

TEST(VMTest, StringConcatenationViaOp) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(0);
        e.emitString("hello");
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(1);
        e.emitString("world");
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_STREQ(vm.getLastReturn().asString(), "helloworld");
}

TEST(VMTest, StringEquality) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(0);
        e.emitString("test");
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(1);
        e.emitString("test");
        e.emit(OpCode::EQ);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_NE(vm.getLastReturn().asInt(), 0); // true
}

TEST(VMTest, StringInequality) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(0);
        e.emitString("a");
        e.emit(OpCode::PUSH_STRING);
        e.emitReg(1);
        e.emitString("b");
        e.emit(OpCode::EQ);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0); // false
}

// ===========================================================================
// Edge cases - mixed types
// ===========================================================================

TEST(VMTest, IntPlusFloat) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitReg(0);
        e.emitInt(5);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(1);
        e.emitFloat(3.5);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 8.5);
}

TEST(VMTest, FloatPlusInt) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(2.5);
        e.emit(OpCode::PUSH_INT);
        e.emitReg(1);
        e.emitInt(3);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 5.5);
}

// ===========================================================================
// Float fast paths for arithmetic operations
// ===========================================================================

TEST(VMTest, FloatFastPathAdd) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(1.5);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(1);
        e.emitFloat(2.5);
        e.emit(OpCode::ADD);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 4.0);
}

TEST(VMTest, FloatFastPathSub) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(5.5);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(1);
        e.emitFloat(2.0);
        e.emit(OpCode::SUB);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 3.5);
}

TEST(VMTest, FloatFastPathMul) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(3.0);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(1);
        e.emitFloat(4.0);
        e.emit(OpCode::MUL);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 12.0);
}

TEST(VMTest, FloatFastPathDiv) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(10.0);
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(1);
        e.emitFloat(4.0);
        e.emit(OpCode::DIV);
        e.emitReg(2);
        e.emitReg(0);
        e.emitReg(1);
        e.emit(OpCode::RETURN);
        e.emitReg(2);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), 2.5);
}

TEST(VMTest, FloatFastPathNeg) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitReg(0);
        e.emitFloat(3.14);
        e.emit(OpCode::NEG);
        e.emitReg(1);
        e.emitReg(0);
        e.emit(OpCode::RETURN);
        e.emitReg(1);
    });
    VM vm;
    vm.execute(code);
    EXPECT_DOUBLE_EQ(vm.getLastReturn().asFloat(), -3.14);
}
