#include "vm.h"
#include "../include/bytecode.h"
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <string>

namespace omscript {

using BytecodeIteratorDiff = std::vector<uint8_t>::difference_type;

VM::VM() : lastReturn() {}

void VM::push(const Value& value) {
    if (stack.size() >= kMaxStackSize) {
        throw std::runtime_error("Stack overflow: exceeded maximum stack size of " +
                                 std::to_string(kMaxStackSize));
    }
    stack.push_back(value);
}

Value VM::pop() {
    if (stack.empty()) {
        throw std::runtime_error("Stack underflow");
    }
    Value value = stack.back();
    stack.pop_back();
    return value;
}

Value VM::peek(int offset) {
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

void VM::execute(const std::vector<uint8_t>& bytecode) {
    size_t ip = 0;
    stack.clear();
    lastReturn = Value();
    
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
            
            case OpCode::LOAD_VAR: {
                std::string name = readString(bytecode, ip);
                push(getGlobal(name));
                break;
            }
            
            case OpCode::STORE_VAR: {
                std::string name = readString(bytecode, ip);
                // Use peek to read the value without popping, preserving assignment semantics.
                Value value = peek(0);
                setGlobal(name, value);
                break;
            }
            
            case OpCode::LOAD_LOCAL: {
                uint8_t index = readByte(bytecode, ip);
                if (index >= locals.size()) {
                    throw std::runtime_error("Local variable index out of range: " +
                        std::to_string(index));
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
                    throw std::runtime_error("Jump offset out of bounds");
                }
                ip = offset;
                break;
            }
            
            case OpCode::JUMP_IF_FALSE: {
                // Jump offsets are absolute bytecode positions.
                uint16_t offset = readShort(bytecode, ip);
                Value condition = pop();
                if (!condition.isTruthy()) {
                    if (offset > bytecode.size()) {
                        throw std::runtime_error("Jump offset out of bounds");
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
            
            case OpCode::HALT:
                lastReturn = Value();
                stack.clear();
                return;
            
            case OpCode::CALL: {
                std::string funcName = readString(bytecode, ip);
                uint8_t argCount = readByte(bytecode, ip);

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
                frame.savedLocals = locals;
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
}

} // namespace omscript
