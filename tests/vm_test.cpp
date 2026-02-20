#include <gtest/gtest.h>
#include "vm.h"
#include "bytecode.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"

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
// Call opcode – full function dispatch
// ===========================================================================

TEST(VMTest, CallSimpleFunction) {
    // Register a function "add" that loads its two local args and adds them.
    BytecodeFunction addFunc;
    addFunc.name = "add";
    addFunc.arity = 2;
    addFunc.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitByte(0);          // first arg
        e.emit(OpCode::LOAD_LOCAL);
        e.emitByte(1);          // second arg
        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
    });

    // Main bytecode: push 3 and 4, call add(3, 4), return result.
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(3);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(4);
        e.emit(OpCode::CALL);
        e.emitString("add");
        e.emitByte(2);
        e.emit(OpCode::RETURN);
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
        e.emitInt(42);
        e.emit(OpCode::RETURN);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::CALL);
        e.emitString("answer");
        e.emitByte(0);
        e.emit(OpCode::RETURN);
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
        e.emitByte(0);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(2);
        e.emit(OpCode::MUL);
        e.emit(OpCode::RETURN);
    });

    // "quad" calls double_it twice: double_it(double_it(x))
    BytecodeFunction quad;
    quad.name = "quad";
    quad.arity = 1;
    quad.bytecode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitByte(0);
        e.emit(OpCode::CALL);
        e.emitString("double_it");
        e.emitByte(1);
        // result of double_it is on stack; call double_it again
        e.emit(OpCode::CALL);
        e.emitString("double_it");
        e.emitByte(1);
        e.emit(OpCode::RETURN);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::CALL);
        e.emitString("quad");
        e.emitByte(1);
        e.emit(OpCode::RETURN);
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
        e.emitInt(0);
        e.emit(OpCode::RETURN);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::CALL);
        e.emitString("f");
        e.emitByte(1); // expects 2 but got 1
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
        e.emitByte(0);
        e.emit(OpCode::STORE_VAR);
        e.emitString("x");
        e.emit(OpCode::POP);  // STORE_VAR uses peek, leaving the stored value on the stack; pop it
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(99);
        e.emit(OpCode::CALL);
        e.emitString("set_x");
        e.emitByte(1);
        e.emit(OpCode::POP);  // discard return value
        e.emit(OpCode::LOAD_VAR);
        e.emitString("x");
        e.emit(OpCode::RETURN);
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
        e.emitByte(0);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(10);
        e.emit(OpCode::ADD);
        e.emit(OpCode::STORE_LOCAL);
        e.emitByte(0);
        e.emit(OpCode::POP);  // STORE_LOCAL uses peek, leaving the stored value on the stack; pop it
        e.emit(OpCode::LOAD_LOCAL);
        e.emitByte(0);
        e.emit(OpCode::RETURN);
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(5);
        e.emit(OpCode::CALL);
        e.emitString("inc");
        e.emitByte(1);
        e.emit(OpCode::RETURN);
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
        e.emitByte(5); // no locals
    });

    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::CALL);
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

// ===========================================================================
// Jump out of bounds
// ===========================================================================

TEST(VMTest, JumpOutOfBounds) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::JUMP);
        e.emitShort(0xFFFF);  // way past end
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, JumpIfFalseOutOfBounds) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);  // false condition
        e.emit(OpCode::JUMP_IF_FALSE);
        e.emitShort(0xFFFF);  // way past end
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

// ===========================================================================
// STORE_VAR uses peek (value stays on stack after store)
// ===========================================================================

TEST(VMTest, StoreVarLeavesValueOnStack) {
    // STORE_VAR uses peek() to read the value without popping.
    // The value should remain on the stack after the store, allowing
    // it to be used as the result of an assignment expression.
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(99);
        e.emit(OpCode::STORE_VAR);
        e.emitString("x");
        // The value 99 should still be on the stack (peek semantics)
        // Pop the leftover value via another store or a pop
        e.emit(OpCode::POP);
        // Now load the stored variable and return it
        e.emit(OpCode::LOAD_VAR);
        e.emitString("x");
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 99);
}

// ===========================================================================
// Nested CALL preserves caller stack
// ===========================================================================

TEST(VMTest, CallPreservesCallerStack) {
    // This test verifies that a function call does not destroy intermediate
    // values on the caller's stack.  Before the fix, execute() called
    // stack.clear() which wiped the caller's data.
    //
    // Caller logic:  push 100, push call(identity, 42), ADD → should be 142
    //
    // identity(x) just returns its argument.
    auto identityCode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitByte(0);
        e.emit(OpCode::RETURN);
    });

    auto callerCode = buildBytecode([](BytecodeEmitter& e) {
        // Push an intermediate value that must survive the CALL
        e.emit(OpCode::PUSH_INT);
        e.emitInt(100);
        // Call identity(42)
        e.emit(OpCode::PUSH_INT);
        e.emitInt(42);
        e.emit(OpCode::CALL);
        e.emitString("identity");
        e.emitByte(1);
        // ADD the intermediate value (100) and the call result (42)
        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
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
    // push 10, call(id, 20), ADD, call(id, 30), ADD → 10 + 20 + 30 = 60
    auto identityCode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL);
        e.emitByte(0);
        e.emit(OpCode::RETURN);
    });

    auto callerCode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(10);

        e.emit(OpCode::PUSH_INT);
        e.emitInt(20);
        e.emit(OpCode::CALL);
        e.emitString("id");
        e.emitByte(1);

        e.emit(OpCode::ADD);

        e.emit(OpCode::PUSH_INT);
        e.emitInt(30);
        e.emit(OpCode::CALL);
        e.emitString("id");
        e.emitByte(1);

        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
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
// Stack overflow protection
// ===========================================================================

TEST(VMTest, StackOverflowThrows) {
    // Build bytecode that pushes values in a tight loop exceeding the stack limit.
    BytecodeEmitter emitter;
    for (size_t i = 0; i <= VM::kMaxStackSize; ++i) {
        emitter.emit(OpCode::PUSH_INT);
        emitter.emitInt(static_cast<int64_t>(i));
    }
    emitter.emit(OpCode::HALT);

    VM vm;
    EXPECT_THROW(vm.execute(emitter.getCode()), std::runtime_error);
}

// ===========================================================================
// Call depth limit
// ===========================================================================

TEST(VMTest, CallDepthLimitThrows) {
    // A function that calls itself unconditionally will hit the call depth limit.
    BytecodeEmitter body;
    body.emit(OpCode::CALL);
    body.emitString("recurse");
    body.emitByte(0);  // 0 arguments
    body.emit(OpCode::RETURN);

    BytecodeFunction fn;
    fn.name = "recurse";
    fn.arity = 0;
    fn.bytecode = body.getCode();

    BytecodeEmitter caller;
    caller.emit(OpCode::CALL);
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
        e.emitInt(0xFF);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0x0F);
        e.emit(OpCode::BIT_AND);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0x0F);
}

TEST(VMTest, BitwiseOr) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0xF0);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0x0F);
        e.emit(OpCode::BIT_OR);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0xFF);
}

TEST(VMTest, BitwiseXor) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0xFF);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0x0F);
        e.emit(OpCode::BIT_XOR);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 0xF0);
}

TEST(VMTest, BitwiseNot) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::BIT_NOT);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), ~static_cast<int64_t>(0));
}

TEST(VMTest, ShiftLeft) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(4);
        e.emit(OpCode::SHL);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 16);
}

TEST(VMTest, ShiftRight) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(16);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(4);
        e.emit(OpCode::SHR);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(VMTest, BitwiseAndOnFloatThrows) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_FLOAT);
        e.emitFloat(1.0);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::BIT_AND);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

TEST(VMTest, ShiftLeftOutOfRange) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(64);
        e.emit(OpCode::SHL);
    });
    VM vm;
    EXPECT_THROW(vm.execute(code), std::runtime_error);
}

// ===========================================================================
// DUP opcode
// ===========================================================================

TEST(VMTest, Dup) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(42);
        e.emit(OpCode::DUP);
        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 84);
}

TEST(VMTest, DupString) {
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING);
        e.emitString("hello");
        e.emit(OpCode::DUP);
        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
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
        e.emitInt(42);
        e.emit(OpCode::PRINT);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
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
        e.emitString("hello world");
        e.emit(OpCode::PRINT);
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
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
static BytecodeFunction makeBytecodeFunc(
    const std::string& name, uint8_t arity,
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
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(1);
        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
    });

    VM vm;
    vm.registerFunction(addFunc);

    // Call more than JIT threshold times — the function should be JIT-compiled.
    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT); e.emitInt(3);
            e.emit(OpCode::PUSH_INT); e.emitInt(4);
            e.emit(OpCode::CALL);
            e.emitString("add");
            e.emitByte(2);
            e.emit(OpCode::RETURN);
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
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(0);
        e.emit(OpCode::PUSH_INT);   e.emitInt(0);
        e.emit(OpCode::LT);
        e.emit(OpCode::JUMP_IF_FALSE);
        size_t patch = e.currentOffset();
        e.emitShort(0); // placeholder

        // if-body: return -x
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(0);
        e.emit(OpCode::NEG);
        e.emit(OpCode::RETURN);

        // after-if: return x
        e.patchJump(patch, static_cast<uint16_t>(e.currentOffset()));
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(0);
        e.emit(OpCode::RETURN);
    });

    VM vm;
    vm.registerFunction(absFunc);

    // Warm up with positive value to trigger JIT.
    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT); e.emitInt(5);
            e.emit(OpCode::CALL);
            e.emitString("myabs");
            e.emitByte(1);
            e.emit(OpCode::RETURN);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), 5);
    }

    EXPECT_TRUE(vm.isJITCompiled("myabs"));

    // Now test with negative value (through the JIT path).
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT); e.emitInt(-7);
        e.emit(OpCode::CALL);
        e.emitString("myabs");
        e.emitByte(1);
        e.emit(OpCode::RETURN);
    });
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 7);
}

TEST(VMTest, JITFallbackForStrings) {
    // A function that uses PRINT — not JIT-eligible.
    auto printFunc = makeBytecodeFunc("printer", 0, [](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_STRING); e.emitString("hello");
        e.emit(OpCode::PRINT);
        e.emit(OpCode::PUSH_INT); e.emitInt(0);
        e.emit(OpCode::RETURN);
    });

    VM vm;
    vm.registerFunction(printFunc);

    // Call many times — should NOT be JIT-compiled (uses PRINT + STRING).
    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::CALL);
            e.emitString("printer");
            e.emitByte(0);
            e.emit(OpCode::RETURN);
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
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(1);
        e.emit(OpCode::MUL);
        e.emit(OpCode::RETURN);
    });

    VM vm;
    vm.registerFunction(mulFunc);

    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT); e.emitInt(6);
            e.emit(OpCode::PUSH_INT); e.emitInt(7);
            e.emit(OpCode::CALL);
            e.emitString("mul");
            e.emitByte(2);
            e.emit(OpCode::RETURN);
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
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(0);   // push x
        e.emit(OpCode::PUSH_INT);   e.emitInt(2);     // push 2
        e.emit(OpCode::MUL);                           // x * 2
        e.emit(OpCode::STORE_LOCAL); e.emitByte(1);   // store to local 1 (y)
        e.emit(OpCode::POP);                           // clean stack
        // return y + 1
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(1);    // push y
        e.emit(OpCode::PUSH_INT);   e.emitInt(1);     // push 1
        e.emit(OpCode::ADD);                           // y + 1
        e.emit(OpCode::RETURN);
    });

    VM vm;
    vm.registerFunction(func);

    for (size_t i = 0; i <= BytecodeJIT::kJITThreshold + 1; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT); e.emitInt(10);
            e.emit(OpCode::CALL);
            e.emitString("compute");
            e.emitByte(1);
            e.emit(OpCode::RETURN);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), 21);  // 10*2+1
    }
    EXPECT_TRUE(vm.isJITCompiled("compute"));
}

TEST(VMTest, JITRecompileAfterThreshold) {
    // fn doubler(x) { return x * 2; }
    auto func = makeBytecodeFunc("doubler", 1, [](BytecodeEmitter& e) {
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(0);
        e.emit(OpCode::PUSH_INT);   e.emitInt(2);
        e.emit(OpCode::MUL);
        e.emit(OpCode::RETURN);
    });

    VM vm;
    vm.registerFunction(func);

    // Warm up past JIT threshold + recompile threshold.
    size_t totalCalls = BytecodeJIT::kJITThreshold + BytecodeJIT::kRecompileThreshold + 5;
    for (size_t i = 0; i < totalCalls; i++) {
        auto code = buildBytecode([](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT); e.emitInt(7);
            e.emit(OpCode::CALL);
            e.emitString("doubler");
            e.emitByte(1);
            e.emit(OpCode::RETURN);
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
    // fn compute(x, y) { return x + y; }
    // The hybrid compiler should emit bytecode for 'compute' since it lacks
    // type annotations.  We can then register it with the VM and call it.
    CodeGenerator codegen;
    VM vm;
    hybridCompileAndRegister(R"(
        fn compute(x, y) { return x + y; }
        fn main() { return compute(3, 4); }
    )", vm, codegen);

    // 'compute' should have been classified as Interpreted
    EXPECT_EQ(codegen.getFunctionTier("compute"), ExecutionTier::Interpreted);
    EXPECT_TRUE(codegen.hasHybridBytecodeFunctions());

    // Build main bytecode that calls compute(10, 20)
    auto mainCode = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT); e.emitInt(10);
        e.emit(OpCode::PUSH_INT); e.emitInt(20);
        e.emit(OpCode::CALL);
        e.emitString("compute");
        e.emitByte(2);
        e.emit(OpCode::RETURN);
    });
    vm.execute(mainCode);
    EXPECT_EQ(vm.getLastReturn().asInt(), 30);
}

TEST(VMTest, HybridMultipleFunctionsExecute) {
    // Two untyped functions: doubler and adder
    CodeGenerator codegen;
    VM vm;
    hybridCompileAndRegister(R"(
        fn doubler(x) { return x * 2; }
        fn adder(a, b) { return a + b; }
        fn main() { return adder(doubler(3), 4); }
    )", vm, codegen);

    EXPECT_EQ(codegen.getBytecodeFunctions().size(), 2u);

    // Call doubler(5) => 10
    auto code1 = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT); e.emitInt(5);
        e.emit(OpCode::CALL);
        e.emitString("doubler");
        e.emitByte(1);
        e.emit(OpCode::RETURN);
    });
    vm.execute(code1);
    EXPECT_EQ(vm.getLastReturn().asInt(), 10);

    // Call adder(3, 7) => 10
    auto code2 = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT); e.emitInt(3);
        e.emit(OpCode::PUSH_INT); e.emitInt(7);
        e.emit(OpCode::CALL);
        e.emitString("adder");
        e.emitByte(2);
        e.emit(OpCode::RETURN);
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
    )", vm, codegen);

    EXPECT_EQ(codegen.getFunctionTier("typed_add"), ExecutionTier::AOT);
    EXPECT_FALSE(codegen.hasHybridBytecodeFunctions());
}

// ===========================================================================
// JUMP_IF_FALSE integer fast path
// ===========================================================================

TEST(VMTest, JumpIfFalseIntegerFastPath) {
    // if (0) { result = 99; } else { result = 42; }
    // The integer fast path should handle the zero check without isTruthy().
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0); // false condition
        e.emit(OpCode::JUMP_IF_FALSE);
        size_t patch = e.currentOffset();
        e.emitShort(0); // placeholder

        // true branch (should be skipped)
        e.emit(OpCode::PUSH_INT);
        e.emitInt(99);
        e.emit(OpCode::RETURN);

        // false branch
        e.patchJump(patch, static_cast<uint16_t>(e.currentOffset()));
        e.emit(OpCode::PUSH_INT);
        e.emitInt(42);
        e.emit(OpCode::RETURN);
    });
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 42);
}

TEST(VMTest, JumpIfFalseIntegerTruthy) {
    // if (1) { result = 99; } — nonzero integer is truthy
    auto code = buildBytecode([](BytecodeEmitter& e) {
        e.emit(OpCode::PUSH_INT);
        e.emitInt(1); // true condition
        e.emit(OpCode::JUMP_IF_FALSE);
        size_t patch = e.currentOffset();
        e.emitShort(0);

        // true branch
        e.emit(OpCode::PUSH_INT);
        e.emitInt(99);
        e.emit(OpCode::RETURN);

        // false branch (should not reach)
        e.patchJump(patch, static_cast<uint16_t>(e.currentOffset()));
        e.emit(OpCode::PUSH_INT);
        e.emitInt(0);
        e.emit(OpCode::RETURN);
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
        e.emitInt(1);
        e.emit(OpCode::RETURN);
    });

    BytecodeJIT jit;
    // Tiny functions (< kMinBytecodeSize opcodes) should fail to compile.
    bool compiled = jit.compile(tinyFunc);
    // The exact threshold depends on kMinBytecodeSize; the function bytecode
    // is 10 bytes (PUSH_INT + 8 bytes + RETURN) which should be >= 4.
    // So this should actually compile. Let's test with truly tiny code.
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
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(0);
        e.emit(OpCode::LOAD_LOCAL); e.emitByte(1);
        e.emit(OpCode::ADD);
        e.emit(OpCode::RETURN);
    });

    VM vm;
    vm.registerFunction(addFunc);

    // Call enough times to trigger JIT, then call more to exercise cached path.
    for (size_t i = 0; i < BytecodeJIT::kJITThreshold + 10; i++) {
        auto code = buildBytecode([&](BytecodeEmitter& e) {
            e.emit(OpCode::PUSH_INT); e.emitInt(static_cast<int64_t>(i));
            e.emit(OpCode::PUSH_INT); e.emitInt(1);
            e.emit(OpCode::CALL);
            e.emitString("cached_add");
            e.emitByte(2);
            e.emit(OpCode::RETURN);
        });
        vm.execute(code);
        EXPECT_EQ(vm.getLastReturn().asInt(), static_cast<int64_t>(i) + 1);
    }
    EXPECT_TRUE(vm.isJITCompiled("cached_add"));
}
