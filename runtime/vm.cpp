#include "vm.h"
#include "../include/bytecode.h"
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <string>

namespace omscript {

VM::VM() : lastReturn() {}

void VM::push(const Value& value) {
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
    using CodeDiff = std::vector<uint8_t>::difference_type;
    auto offset = static_cast<CodeDiff>(ip);
    auto begin = code.begin() + offset;
    auto end = begin + static_cast<CodeDiff>(length);
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
                // STORE_VAR now pops then pushes to avoid stack growth while preserving assignment semantics.
                Value value = pop();
                setGlobal(name, value);
                push(value);
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
                    constexpr int64_t kDefaultReturnValue = 0;
                    Value returnValue = stack.empty() ? Value(kDefaultReturnValue) : pop();
                    stack.clear();
                    lastReturn = returnValue;
                }
                return;
            
            case OpCode::HALT:
                lastReturn = Value();
                stack.clear();
                return;
            
            case OpCode::CALL:
                throw std::runtime_error("CALL opcode not implemented");
            
            default:
                throw std::runtime_error("Unknown opcode " + std::to_string(static_cast<uint8_t>(op)) +
                                         " at ip " + std::to_string(ip - 1));
        }
    }
}

} // namespace omscript
