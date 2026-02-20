#include "vm.h"
#include "jit.h"
#include "../include/bytecode.h"
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <string>

namespace omscript {

using BytecodeIteratorDiff = std::vector<uint8_t>::difference_type;

// Maximum valid shift amount for 64-bit integers (0–63).
static constexpr int64_t kInt64BitWidth = 64;

VM::VM() : lastReturn(), jit_(std::make_unique<BytecodeJIT>()) {
    stack.reserve(256);
    locals.reserve(16);
}

VM::~VM() = default;

void VM::push(const Value& value) {
    if (stack.size() >= kMaxStackSize) {
        throw std::runtime_error("Stack overflow: exceeded maximum stack size of " +
                                 std::to_string(kMaxStackSize));
    }
    stack.push_back(value);
}

void VM::push(Value&& value) {
    if (stack.size() >= kMaxStackSize) {
        throw std::runtime_error("Stack overflow: exceeded maximum stack size of " +
                                 std::to_string(kMaxStackSize));
    }
    stack.push_back(std::move(value));
}

Value VM::pop() {
    if (stack.empty()) {
        throw std::runtime_error("Stack underflow");
    }
    Value value = std::move(stack.back());
    stack.pop_back();
    return value;
}

const Value& VM::peek(int offset) const {
    if (offset < 0) {
        throw std::runtime_error("Invalid stack offset");
    }
    if (stack.size() <= static_cast<size_t>(offset)) {
        throw std::runtime_error("Stack underflow");
    }
    return stack[stack.size() - 1 - offset];
}

void VM::ensureReadable(const std::vector<uint8_t>& code, size_t ip, size_t count) {
    if (ip + count > code.size()) {
        throw std::runtime_error("Bytecode read out of bounds at ip " + std::to_string(ip));
    }
}

uint8_t VM::readByte(const std::vector<uint8_t>& code, size_t& ip) {
    ensureReadable(code, ip, 1);
    return code[ip++];
}

uint16_t VM::readShort(const std::vector<uint8_t>& code, size_t& ip) {
    ensureReadable(code, ip, 2);
    uint8_t low = code[ip++];
    uint8_t high = code[ip++];
    return static_cast<uint16_t>(low) | (static_cast<uint16_t>(high) << 8);
}

int64_t VM::readInt(const std::vector<uint8_t>& code, size_t& ip) {
    ensureReadable(code, ip, 8);
    uint64_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw |= (static_cast<uint64_t>(code[ip++]) << (i * 8));
    }
    int64_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

double VM::readFloat(const std::vector<uint8_t>& code, size_t& ip) {
    ensureReadable(code, ip, 8);
    uint64_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw |= (static_cast<uint64_t>(code[ip++]) << (i * 8));
    }
    double value = 0.0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::string VM::readString(const std::vector<uint8_t>& code, size_t& ip) {
    uint16_t length = readShort(code, ip);
    ensureReadable(code, ip, length);
    auto offset = static_cast<BytecodeIteratorDiff>(ip);
    auto begin = code.begin() + offset;
    auto end = begin + static_cast<BytecodeIteratorDiff>(length);
    std::string str(begin, end);
    ip += length;
    return str;
}

void VM::setGlobal(const std::string& name, const Value& value) {
    globals[name] = value;
}

Value VM::getGlobal(const std::string& name) {
    auto it = globals.find(name);
    if (it != globals.end()) {
        return it->second;
    }
    throw std::runtime_error("Undefined variable: " + name);
}

void VM::registerFunction(const BytecodeFunction& func) {
    functions[func.name] = func;
}

bool VM::isJITCompiled(const std::string& name) const {
    return jit_ && jit_->isCompiled(name);
}

bool VM::invokeJIT(JITFnPtr fn, uint8_t argCount) {
    // Verify all args are integers (int-specialized JIT).
    for (size_t i = 0; i < argCount; i++) {
        if (stack[stack.size() - 1 - i].getType() != Value::Type::INTEGER)
            return false;
    }
    // Stack-based arg buffer — avoids heap allocation per call.
    int64_t stackArgs[kMaxStackArgs];
    std::unique_ptr<int64_t[]> heapArgs;
    int64_t* args = stackArgs;
    if (argCount > kMaxStackArgs) {
        heapArgs = std::make_unique<int64_t[]>(argCount);
        args = heapArgs.get();
    }
    for (size_t i = argCount; i > 0; i--)
        args[i - 1] = pop().unsafeAsInt();
    int64_t result = fn(args, static_cast<int>(argCount));
    push(Value(result));
    return true;
}

bool VM::invokeJITFloat(JITFloatFnPtr fn, uint8_t argCount) {
    // Verify all args are float (float-specialized JIT).
    for (size_t i = 0; i < argCount; i++) {
        if (stack[stack.size() - 1 - i].getType() != Value::Type::FLOAT)
            return false;
    }
    double stackArgs[kMaxStackArgs];
    std::unique_ptr<double[]> heapArgs;
    double* args = stackArgs;
    if (argCount > kMaxStackArgs) {
        heapArgs = std::make_unique<double[]>(argCount);
        args = heapArgs.get();
    }
    for (size_t i = argCount; i > 0; i--)
        args[i - 1] = pop().unsafeAsFloat();
    double result = fn(args, static_cast<int>(argCount));
    push(Value(result));
    return true;
}

void VM::classifyArgTypes(uint8_t argCount, bool& allInt, bool& allFloat) const {
    allInt = true;
    allFloat = true;
    for (size_t i = 0; i < argCount; i++) {
        auto t = stack[stack.size() - 1 - i].getType();
        if (t != Value::Type::INTEGER) allInt = false;
        if (t != Value::Type::FLOAT) allFloat = false;
    }
}

void VM::execute(const std::vector<uint8_t>& bytecode) {
    size_t ip = 0;
    stack.clear();
    lastReturn = Value();

// Use computed-goto dispatch on GCC/Clang for faster opcode dispatch.
// The dispatch table eliminates the overhead of the switch jump and branch
// prediction failures that occur with a traditional switch statement.
#if defined(__GNUC__) || defined(__clang__)
#define USE_COMPUTED_GOTO 1
#else
#define USE_COMPUTED_GOTO 0
#endif

#if USE_COMPUTED_GOTO
    static const void* dispatchTable[] = {
        &&op_PUSH_INT,    // 0
        &&op_PUSH_FLOAT,  // 1
        &&op_PUSH_STRING, // 2
        &&op_POP,         // 3
        &&op_ADD,         // 4
        &&op_SUB,         // 5
        &&op_MUL,         // 6
        &&op_DIV,         // 7
        &&op_MOD,         // 8
        &&op_NEG,         // 9
        &&op_EQ,          // 10
        &&op_NE,          // 11
        &&op_LT,          // 12
        &&op_LE,          // 13
        &&op_GT,          // 14
        &&op_GE,          // 15
        &&op_AND,         // 16
        &&op_OR,          // 17
        &&op_NOT,         // 18
        &&op_BIT_AND,     // 19
        &&op_BIT_OR,      // 20
        &&op_BIT_XOR,     // 21
        &&op_BIT_NOT,     // 22
        &&op_SHL,         // 23
        &&op_SHR,         // 24
        &&op_LOAD_VAR,    // 25
        &&op_STORE_VAR,   // 26
        &&op_LOAD_LOCAL,  // 27
        &&op_STORE_LOCAL, // 28
        &&op_JUMP,        // 29
        &&op_JUMP_IF_FALSE, // 30
        &&op_CALL,        // 31
        &&op_RETURN,      // 32
        &&op_PRINT,       // 33
        &&op_DUP,         // 34
        &&op_HALT,        // 35
    };

    static constexpr size_t kDispatchTableSize = sizeof(dispatchTable) / sizeof(dispatchTable[0]);

#define DISPATCH() \
    do { \
        if (ip >= bytecode.size()) goto vm_exit; \
        uint8_t opByte = readByte(bytecode, ip); \
        if (opByte >= kDispatchTableSize) goto op_UNKNOWN; \
        goto *dispatchTable[opByte]; \
    } while (0)

// Integer fast-path helpers — avoid the overhead of Value operators and
// pop()/push() bounds checks when both operands are known to be integers.
#define VM_INT_ARITH_FAST(OP) \
    do { \
        size_t sz = stack.size(); \
        if (sz >= 2 && \
            stack[sz-1].getType() == Value::Type::INTEGER && \
            stack[sz-2].getType() == Value::Type::INTEGER) { \
            int64_t bv = stack[sz-1].unsafeAsInt(); \
            int64_t av = stack[sz-2].unsafeAsInt(); \
            stack.pop_back(); \
            stack.back() = Value(av OP bv); \
            DISPATCH(); \
        } \
    } while (0)

#define VM_INT_CMP_FAST(OP) \
    do { \
        size_t sz = stack.size(); \
        if (sz >= 2 && \
            stack[sz-1].getType() == Value::Type::INTEGER && \
            stack[sz-2].getType() == Value::Type::INTEGER) { \
            int64_t bv = stack[sz-1].unsafeAsInt(); \
            int64_t av = stack[sz-2].unsafeAsInt(); \
            stack.pop_back(); \
            stack.back() = Value(static_cast<int64_t>(av OP bv)); \
            DISPATCH(); \
        } \
    } while (0)

// Integer bitwise fast-path — same pattern for &, |, ^, <<, >>
#define VM_INT_BITWISE_FAST(OP) \
    do { \
        size_t sz = stack.size(); \
        if (sz >= 2 && \
            stack[sz-1].getType() == Value::Type::INTEGER && \
            stack[sz-2].getType() == Value::Type::INTEGER) { \
            int64_t bv = stack[sz-1].unsafeAsInt(); \
            int64_t av = stack[sz-2].unsafeAsInt(); \
            stack.pop_back(); \
            stack.back() = Value(av OP bv); \
            DISPATCH(); \
        } \
    } while (0)

    DISPATCH();

    op_PUSH_INT: {
        int64_t value = readInt(bytecode, ip);
        push(Value(value));
        DISPATCH();
    }
    op_PUSH_FLOAT: {
        double value = readFloat(bytecode, ip);
        push(Value(value));
        DISPATCH();
    }
    op_PUSH_STRING: {
        std::string value = readString(bytecode, ip);
        push(Value(value));
        DISPATCH();
    }
    op_POP: {
        pop();
        DISPATCH();
    }
    op_ADD: {
        VM_INT_ARITH_FAST(+);
        Value b = pop();
        Value a = pop();
        push(a + b);
        DISPATCH();
    }
    op_SUB: {
        VM_INT_ARITH_FAST(-);
        Value b = pop();
        Value a = pop();
        push(a - b);
        DISPATCH();
    }
    op_MUL: {
        VM_INT_ARITH_FAST(*);
        Value b = pop();
        Value a = pop();
        push(a * b);
        DISPATCH();
    }
    op_DIV: {
        // Integer fast path with zero-check
        {
            size_t sz = stack.size();
            if (sz >= 2 &&
                stack[sz-1].getType() == Value::Type::INTEGER &&
                stack[sz-2].getType() == Value::Type::INTEGER) {
                int64_t bv = stack[sz-1].unsafeAsInt();
                int64_t av = stack[sz-2].unsafeAsInt();
                if (bv != 0) {
                    stack.pop_back();
                    stack.back() = Value(av / bv);
                    DISPATCH();
                }
            }
        }
        Value b = pop();
        Value a = pop();
        push(a / b);
        DISPATCH();
    }
    op_MOD: {
        // Integer fast path with zero-check
        {
            size_t sz = stack.size();
            if (sz >= 2 &&
                stack[sz-1].getType() == Value::Type::INTEGER &&
                stack[sz-2].getType() == Value::Type::INTEGER) {
                int64_t bv = stack[sz-1].unsafeAsInt();
                int64_t av = stack[sz-2].unsafeAsInt();
                if (bv != 0) {
                    stack.pop_back();
                    stack.back() = Value(av % bv);
                    DISPATCH();
                }
            }
        }
        Value b = pop();
        Value a = pop();
        push(a % b);
        DISPATCH();
    }
    op_NEG: {
        // Integer fast path for unary negation
        if (!stack.empty() && stack.back().getType() == Value::Type::INTEGER) {
            stack.back() = Value(-stack.back().unsafeAsInt());
            DISPATCH();
        }
        Value a = pop();
        push(-a);
        DISPATCH();
    }
    op_EQ: {
        VM_INT_CMP_FAST(==);
        Value b = pop();
        Value a = pop();
        push(Value(static_cast<int64_t>(a == b)));
        DISPATCH();
    }
    op_NE: {
        VM_INT_CMP_FAST(!=);
        Value b = pop();
        Value a = pop();
        push(Value(static_cast<int64_t>(a != b)));
        DISPATCH();
    }
    op_LT: {
        VM_INT_CMP_FAST(<);
        Value b = pop();
        Value a = pop();
        push(Value(static_cast<int64_t>(a < b)));
        DISPATCH();
    }
    op_LE: {
        VM_INT_CMP_FAST(<=);
        Value b = pop();
        Value a = pop();
        push(Value(static_cast<int64_t>(a <= b)));
        DISPATCH();
    }
    op_GT: {
        VM_INT_CMP_FAST(>);
        Value b = pop();
        Value a = pop();
        push(Value(static_cast<int64_t>(a > b)));
        DISPATCH();
    }
    op_GE: {
        VM_INT_CMP_FAST(>=);
        Value b = pop();
        Value a = pop();
        push(Value(static_cast<int64_t>(a >= b)));
        DISPATCH();
    }
    op_AND: {
        // Integer fast path: both ints → result = (a!=0 && b!=0) as i64
        {
            size_t sz = stack.size();
            if (sz >= 2 &&
                stack[sz-1].getType() == Value::Type::INTEGER &&
                stack[sz-2].getType() == Value::Type::INTEGER) {
                int64_t bv = stack[sz-1].unsafeAsInt();
                int64_t av = stack[sz-2].unsafeAsInt();
                stack.pop_back();
                stack.back() = Value(static_cast<int64_t>(av != 0 && bv != 0));
                DISPATCH();
            }
        }
        Value b = pop();
        Value a = pop();
        push(Value(static_cast<int64_t>(a.isTruthy() && b.isTruthy())));
        DISPATCH();
    }
    op_OR: {
        // Integer fast path
        {
            size_t sz = stack.size();
            if (sz >= 2 &&
                stack[sz-1].getType() == Value::Type::INTEGER &&
                stack[sz-2].getType() == Value::Type::INTEGER) {
                int64_t bv = stack[sz-1].unsafeAsInt();
                int64_t av = stack[sz-2].unsafeAsInt();
                stack.pop_back();
                stack.back() = Value(static_cast<int64_t>(av != 0 || bv != 0));
                DISPATCH();
            }
        }
        Value b = pop();
        Value a = pop();
        push(Value(static_cast<int64_t>(a.isTruthy() || b.isTruthy())));
        DISPATCH();
    }
    op_NOT: {
        // Integer fast path
        if (!stack.empty() && stack.back().getType() == Value::Type::INTEGER) {
            stack.back() = Value(static_cast<int64_t>(stack.back().unsafeAsInt() == 0));
            DISPATCH();
        }
        Value a = pop();
        push(Value(static_cast<int64_t>(!a.isTruthy())));
        DISPATCH();
    }
    op_BIT_AND: {
        VM_INT_BITWISE_FAST(&);
        Value b = pop();
        Value a = pop();
        push(a & b);
        DISPATCH();
    }
    op_BIT_OR: {
        VM_INT_BITWISE_FAST(|);
        Value b = pop();
        Value a = pop();
        push(a | b);
        DISPATCH();
    }
    op_BIT_XOR: {
        VM_INT_BITWISE_FAST(^);
        Value b = pop();
        Value a = pop();
        push(a ^ b);
        DISPATCH();
    }
    op_BIT_NOT: {
        // Integer fast path for unary bitwise NOT
        if (!stack.empty() && stack.back().getType() == Value::Type::INTEGER) {
            stack.back() = Value(~stack.back().unsafeAsInt());
            DISPATCH();
        }
        Value a = pop();
        push(~a);
        DISPATCH();
    }
    op_SHL: {
        // Integer fast path with shift range validation
        {
            size_t sz = stack.size();
            if (sz >= 2 &&
                stack[sz-1].getType() == Value::Type::INTEGER &&
                stack[sz-2].getType() == Value::Type::INTEGER) {
                int64_t bv = stack[sz-1].unsafeAsInt();
                int64_t av = stack[sz-2].unsafeAsInt();
                if (bv >= 0 && bv < kInt64BitWidth) {
                    stack.pop_back();
                    stack.back() = Value(av << bv);
                    DISPATCH();
                }
            }
        }
        Value b = pop();
        Value a = pop();
        push(a << b);
        DISPATCH();
    }
    op_SHR: {
        // Integer fast path with shift range validation
        {
            size_t sz = stack.size();
            if (sz >= 2 &&
                stack[sz-1].getType() == Value::Type::INTEGER &&
                stack[sz-2].getType() == Value::Type::INTEGER) {
                int64_t bv = stack[sz-1].unsafeAsInt();
                int64_t av = stack[sz-2].unsafeAsInt();
                if (bv >= 0 && bv < kInt64BitWidth) {
                    stack.pop_back();
                    stack.back() = Value(av >> bv);
                    DISPATCH();
                }
            }
        }
        Value b = pop();
        Value a = pop();
        push(a >> b);
        DISPATCH();
    }
    op_LOAD_VAR: {
        std::string name = readString(bytecode, ip);
        push(getGlobal(name));
        DISPATCH();
    }
    op_STORE_VAR: {
        std::string name = readString(bytecode, ip);
        setGlobal(name, peek(0));
        DISPATCH();
    }
    op_LOAD_LOCAL: {
        uint8_t index = readByte(bytecode, ip);
        if (index >= locals.size()) {
            throw std::runtime_error("Local variable index out of range: " +
                std::to_string(index) + " at ip " + std::to_string(ip - 1));
        }
        push(locals[index]);
        DISPATCH();
    }
    op_STORE_LOCAL: {
        uint8_t index = readByte(bytecode, ip);
        if (index >= locals.size()) {
            locals.resize(static_cast<size_t>(index) + 1);
        }
        locals[index] = peek(0);
        DISPATCH();
    }
    op_JUMP: {
        uint16_t offset = readShort(bytecode, ip);
        if (offset > bytecode.size()) {
            throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
        }
        ip = offset;
        DISPATCH();
    }
    op_JUMP_IF_FALSE: {
        uint16_t offset = readShort(bytecode, ip);
        // Integer fast path: avoid pop()/isTruthy() overhead.
        if (!stack.empty() && stack.back().getType() == Value::Type::INTEGER) {
            bool isZero = stack.back().unsafeAsInt() == 0;
            stack.pop_back();
            if (isZero) {
                if (offset > bytecode.size()) {
                    throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
                }
                ip = offset;
            }
            DISPATCH();
        }
        Value condition = pop();
        if (!condition.isTruthy()) {
            if (offset > bytecode.size()) {
                throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
            }
            ip = offset;
        }
        DISPATCH();
    }
    op_RETURN: {
        constexpr int64_t defaultReturnValue = 0;
        Value returnValue = stack.empty() ? Value(defaultReturnValue) : pop();
        stack.clear();
        lastReturn = returnValue;
        return;
    }
    op_PRINT: {
        Value a = pop();
        std::cout << a.toString() << std::endl;
        DISPATCH();
    }
    op_DUP: {
        push(peek(0));
        DISPATCH();
    }
    op_HALT: {
        lastReturn = Value();
        stack.clear();
        return;
    }
    op_CALL: {
        std::string funcName = readString(bytecode, ip);
        uint8_t argCount = readByte(bytecode, ip);

        // ---- Record argument types for type-profiled JIT ----
        if (jit_ && argCount > 0) {
            bool allInt, allFloat;
            classifyArgTypes(argCount, allInt, allFloat);
            jit_->recordTypes(funcName, allInt, allFloat);
        }

        // ---- JIT fast path: float-specialized ----
        {
            auto floatIt = jitFloatCache_.find(funcName);
            if (floatIt != jitFloatCache_.end()) {
                if (invokeJITFloat(floatIt->second, argCount)) {
                    if (jit_) jit_->recordPostJITCall(funcName);
                    DISPATCH();
                }
            }
        }

        // ---- JIT fast path: int-specialized ----
        {
            auto cacheIt = jitCache_.find(funcName);
            if (cacheIt != jitCache_.end()) {
                if (invokeJIT(cacheIt->second, argCount)) {
                    // Track post-JIT calls for type-specialized recompilation.
                    if (jit_ && jit_->recordPostJITCall(funcName)) {
                        auto fit = functions.find(funcName);
                        if (fit != functions.end()) {
                            jit_->recompile(fit->second);
                            // Update caches based on new specialization.
                            auto newIntPtr = jit_->getCompiled(funcName);
                            auto newFloatPtr = jit_->getCompiledFloat(funcName);
                            if (newIntPtr) jitCache_[funcName] = newIntPtr;
                            if (newFloatPtr) jitFloatCache_[funcName] = newFloatPtr;
                        }
                    }
                    DISPATCH();
                }
            }
        }

        // ---- JIT compilation trigger ----
        if (jit_) {
            if (jit_->recordCall(funcName)) {
                auto fit = functions.find(funcName);
                if (fit != functions.end()) {
                    // Choose specialization based on type profile.
                    auto spec = jit_->getTypeProfile(funcName).bestSpecialization();
                    if (jit_->compile(fit->second, spec)) {
                        auto intPtr = jit_->getCompiled(funcName);
                        auto floatPtr = jit_->getCompiledFloat(funcName);
                        if (intPtr) jitCache_[funcName] = intPtr;
                        if (floatPtr) jitFloatCache_[funcName] = floatPtr;
                    }
                }
            }
        }

        auto it = functions.find(funcName);
        if (it == functions.end()) {
            throw std::runtime_error("Undefined function: " + funcName);
        }
        const BytecodeFunction& func = it->second;
        if (argCount != func.arity) {
            throw std::runtime_error("Function '" + funcName + "' expects " +
                std::to_string(func.arity) + " arguments but got " +
                std::to_string(argCount));
        }

        if (callStack.size() >= kMaxCallDepth) {
            throw std::runtime_error("Call stack overflow: exceeded maximum call depth of " +
                                     std::to_string(kMaxCallDepth));
        }

        CallFrame frame;
        frame.function = &func;
        frame.returnIp = ip;
        frame.returnBytecode = &bytecode;
        frame.savedLocals = std::move(locals);
        callStack.push_back(std::move(frame));

        locals.clear();
        locals.resize(argCount);
        for (size_t i = argCount; i > 0; i--) {
            locals[i - 1] = pop();
        }

        std::vector<Value> callerStack = std::move(stack);
        stack.clear();

        execute(func.bytecode);

        Value returnValue = lastReturn;
        stack = std::move(callerStack);
        CallFrame& top = callStack.back();
        locals = std::move(top.savedLocals);
        callStack.pop_back();

        push(returnValue);
        DISPATCH();
    }

    op_UNKNOWN:
        throw std::runtime_error("Unknown opcode " + std::to_string(bytecode[ip - 1]) +
                                 " at ip " + std::to_string(ip - 1));

vm_exit:
    return;

#undef DISPATCH
#undef VM_INT_ARITH_FAST
#undef VM_INT_CMP_FAST
#undef VM_INT_BITWISE_FAST
#undef USE_COMPUTED_GOTO

#else // Fallback: standard switch dispatch
    
    while (ip < bytecode.size()) {
        OpCode op = static_cast<OpCode>(readByte(bytecode, ip));
        
        switch (op) {
            case OpCode::PUSH_INT: {
                int64_t value = readInt(bytecode, ip);
                push(Value(value));
                break;
            }
            
            case OpCode::PUSH_FLOAT: {
                double value = readFloat(bytecode, ip);
                push(Value(value));
                break;
            }
            
            case OpCode::PUSH_STRING: {
                std::string value = readString(bytecode, ip);
                push(Value(value));
                break;
            }
            
            case OpCode::POP:
                pop();
                break;
            
            case OpCode::ADD: {
                Value b = pop();
                Value a = pop();
                push(a + b);
                break;
            }
            
            case OpCode::SUB: {
                Value b = pop();
                Value a = pop();
                push(a - b);
                break;
            }
            
            case OpCode::MUL: {
                Value b = pop();
                Value a = pop();
                push(a * b);
                break;
            }
            
            case OpCode::DIV: {
                Value b = pop();
                Value a = pop();
                push(a / b);
                break;
            }
            
            case OpCode::MOD: {
                Value b = pop();
                Value a = pop();
                push(a % b);
                break;
            }
            
            case OpCode::NEG: {
                Value a = pop();
                push(-a);
                break;
            }
            
            case OpCode::EQ: {
                Value b = pop();
                Value a = pop();
                push(Value(static_cast<int64_t>(a == b)));
                break;
            }
            
            case OpCode::NE: {
                Value b = pop();
                Value a = pop();
                push(Value(static_cast<int64_t>(a != b)));
                break;
            }
            
            case OpCode::LT: {
                Value b = pop();
                Value a = pop();
                push(Value(static_cast<int64_t>(a < b)));
                break;
            }
            
            case OpCode::LE: {
                Value b = pop();
                Value a = pop();
                push(Value(static_cast<int64_t>(a <= b)));
                break;
            }
            
            case OpCode::GT: {
                Value b = pop();
                Value a = pop();
                push(Value(static_cast<int64_t>(a > b)));
                break;
            }
            
            case OpCode::GE: {
                Value b = pop();
                Value a = pop();
                push(Value(static_cast<int64_t>(a >= b)));
                break;
            }
            
            case OpCode::AND: {
                Value b = pop();
                Value a = pop();
                push(Value(static_cast<int64_t>(a.isTruthy() && b.isTruthy())));
                break;
            }
            
            case OpCode::OR: {
                Value b = pop();
                Value a = pop();
                push(Value(static_cast<int64_t>(a.isTruthy() || b.isTruthy())));
                break;
            }
            
            case OpCode::NOT: {
                Value a = pop();
                push(Value(static_cast<int64_t>(!a.isTruthy())));
                break;
            }
            
            case OpCode::BIT_AND: {
                Value b = pop();
                Value a = pop();
                push(a & b);
                break;
            }
            
            case OpCode::BIT_OR: {
                Value b = pop();
                Value a = pop();
                push(a | b);
                break;
            }
            
            case OpCode::BIT_XOR: {
                Value b = pop();
                Value a = pop();
                push(a ^ b);
                break;
            }
            
            case OpCode::BIT_NOT: {
                Value a = pop();
                push(~a);
                break;
            }
            
            case OpCode::SHL: {
                Value b = pop();
                Value a = pop();
                push(a << b);
                break;
            }
            
            case OpCode::SHR: {
                Value b = pop();
                Value a = pop();
                push(a >> b);
                break;
            }
            
            case OpCode::LOAD_VAR: {
                std::string name = readString(bytecode, ip);
                push(getGlobal(name));
                break;
            }
            
            case OpCode::STORE_VAR: {
                std::string name = readString(bytecode, ip);
                // Use peek to read the value without popping, preserving assignment semantics.
                setGlobal(name, peek(0));
                break;
            }
            
            case OpCode::LOAD_LOCAL: {
                uint8_t index = readByte(bytecode, ip);
                if (index >= locals.size()) {
                    throw std::runtime_error("Local variable index out of range: " +
                        std::to_string(index) + " at ip " + std::to_string(ip - 1));
                }
                push(locals[index]);
                break;
            }
            
            case OpCode::STORE_LOCAL: {
                uint8_t index = readByte(bytecode, ip);
                if (index >= locals.size()) {
                    locals.resize(static_cast<size_t>(index) + 1);
                }
                locals[index] = peek(0);
                break;
            }
            
            case OpCode::JUMP: {
                // Jump offsets are absolute bytecode positions.
                uint16_t offset = readShort(bytecode, ip);
                if (offset > bytecode.size()) {
                    throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
                }
                ip = offset;
                break;
            }
            
            case OpCode::JUMP_IF_FALSE: {
                // Jump offsets are absolute bytecode positions.
                uint16_t offset = readShort(bytecode, ip);
                // Integer fast path: avoid pop()/isTruthy() overhead.
                if (!stack.empty() && stack.back().getType() == Value::Type::INTEGER) {
                    bool isZero = stack.back().unsafeAsInt() == 0;
                    stack.pop_back();
                    if (isZero) {
                        if (offset > bytecode.size()) {
                            throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
                        }
                        ip = offset;
                    }
                    break;
                }
                Value condition = pop();
                if (!condition.isTruthy()) {
                    if (offset > bytecode.size()) {
                        throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
                    }
                    ip = offset;
                }
                break;
            }
            
            case OpCode::RETURN:
                // Return from current function, preserving the top of stack as the return value.
                // If the stack is empty, return 0 to match compiler default return semantics.
                {
                    constexpr int64_t defaultReturnValue = 0;
                    Value returnValue = stack.empty() ? Value(defaultReturnValue) : pop();
                    stack.clear();
                    lastReturn = returnValue;
                }
                return;
            
            case OpCode::PRINT: {
                Value a = pop();
                std::cout << a.toString() << std::endl;
                break;
            }
            
            case OpCode::DUP: {
                push(peek(0));
                break;
            }
            
            case OpCode::HALT:
                lastReturn = Value();
                stack.clear();
                return;
            
            case OpCode::CALL: {
                std::string funcName = readString(bytecode, ip);
                uint8_t argCount = readByte(bytecode, ip);

                // ---- Record argument types for type-profiled JIT ----
                if (jit_ && argCount > 0) {
                    bool allInt, allFloat;
                    classifyArgTypes(argCount, allInt, allFloat);
                    jit_->recordTypes(funcName, allInt, allFloat);
                }

                // ---- JIT fast path: float-specialized ----
                {
                    auto floatIt = jitFloatCache_.find(funcName);
                    if (floatIt != jitFloatCache_.end()) {
                        if (invokeJITFloat(floatIt->second, argCount)) {
                            if (jit_) jit_->recordPostJITCall(funcName);
                            break;
                        }
                    }
                }

                // ---- JIT fast path: int-specialized ----
                {
                    auto cacheIt = jitCache_.find(funcName);
                    if (cacheIt != jitCache_.end()) {
                        if (invokeJIT(cacheIt->second, argCount)) {
                            if (jit_ && jit_->recordPostJITCall(funcName)) {
                                auto fit = functions.find(funcName);
                                if (fit != functions.end()) {
                                    jit_->recompile(fit->second);
                                    auto newIntPtr = jit_->getCompiled(funcName);
                                    auto newFloatPtr = jit_->getCompiledFloat(funcName);
                                    if (newIntPtr) jitCache_[funcName] = newIntPtr;
                                    if (newFloatPtr) jitFloatCache_[funcName] = newFloatPtr;
                                }
                            }
                            break;
                        }
                    }
                }

                // ---- JIT compilation trigger ----
                if (jit_) {
                    if (jit_->recordCall(funcName)) {
                        auto fit = functions.find(funcName);
                        if (fit != functions.end()) {
                            auto spec = jit_->getTypeProfile(funcName).bestSpecialization();
                            if (jit_->compile(fit->second, spec)) {
                                auto intPtr = jit_->getCompiled(funcName);
                                auto floatPtr = jit_->getCompiledFloat(funcName);
                                if (intPtr) jitCache_[funcName] = intPtr;
                                if (floatPtr) jitFloatCache_[funcName] = floatPtr;
                            }
                        }
                    }
                }

                auto it = functions.find(funcName);
                if (it == functions.end()) {
                    throw std::runtime_error("Undefined function: " + funcName);
                }
                const BytecodeFunction& func = it->second;
                if (argCount != func.arity) {
                    throw std::runtime_error("Function '" + funcName + "' expects " +
                        std::to_string(func.arity) + " arguments but got " +
                        std::to_string(argCount));
                }

                if (callStack.size() >= kMaxCallDepth) {
                    throw std::runtime_error("Call stack overflow: exceeded maximum call depth of " +
                                             std::to_string(kMaxCallDepth));
                }

                // Save the current call frame so we can resume after the
                // callee returns.
                CallFrame frame;
                frame.function = &func;
                frame.returnIp = ip;
                frame.returnBytecode = &bytecode;
                frame.savedLocals = std::move(locals);
                callStack.push_back(std::move(frame));

                // Bind arguments as local variables.  Arguments are pushed
                // left-to-right so the first argument is deepest on the stack.
                locals.clear();
                locals.resize(argCount);
                for (size_t i = argCount; i > 0; i--) {
                    locals[i - 1] = pop();
                }

                // Save the caller's stack so the callee starts with a clean
                // stack.  execute() calls stack.clear() at entry which would
                // otherwise destroy any intermediate values the caller still
                // needs after the call returns.
                std::vector<Value> callerStack = std::move(stack);
                stack.clear();

                // Execute the callee's bytecode inline using a recursive call
                // to execute().  RETURN inside the callee will cause that
                // execute() to return, at which point we restore state.
                execute(func.bytecode);

                // Restore caller state from the call frame.
                Value returnValue = lastReturn;
                stack = std::move(callerStack);
                CallFrame& top = callStack.back();
                locals = std::move(top.savedLocals);
                callStack.pop_back();

                // Push the callee's return value onto the restored caller stack.
                push(returnValue);
                break;
            }
            
            default:
                throw std::runtime_error("Unknown opcode " + std::to_string(static_cast<uint8_t>(op)) +
                                         " at ip " + std::to_string(ip - 1));
        }
    }
#endif // USE_COMPUTED_GOTO
}

} // namespace omscript
