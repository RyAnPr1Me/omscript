#include "vm.h"
#include "../include/bytecode.h"
#include <stdexcept>
#include <iostream>
#include <cstring>

namespace omscript {

VM::VM() {}

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
    if (stack.size() <= static_cast<size_t>(offset)) {
        throw std::runtime_error("Stack underflow");
    }
    return stack[stack.size() - 1 - offset];
}

uint8_t VM::readByte(const std::vector<uint8_t>& code, size_t& ip) {
    return code[ip++];
}

uint16_t VM::readShort(const std::vector<uint8_t>& code, size_t& ip) {
    uint8_t low = code[ip++];
    uint8_t high = code[ip++];
    return static_cast<uint16_t>(low) | (static_cast<uint16_t>(high) << 8);
}

int64_t VM::readInt(const std::vector<uint8_t>& code, size_t& ip) {
    int64_t value;
    std::memcpy(&value, &code[ip], sizeof(value));
    ip += sizeof(value);
    return value;
}

double VM::readFloat(const std::vector<uint8_t>& code, size_t& ip) {
    double value;
    std::memcpy(&value, &code[ip], sizeof(value));
    ip += sizeof(value);
    return value;
}

std::string VM::readString(const std::vector<uint8_t>& code, size_t& ip) {
    uint16_t length = readShort(code, ip);
    std::string str;
    for (uint16_t i = 0; i < length; i++) {
        str += static_cast<char>(code[ip++]);
    }
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
                Value value = peek();
                setGlobal(name, value);
                break;
            }
            
            case OpCode::JUMP: {
                uint16_t offset = readShort(bytecode, ip);
                ip = offset;
                break;
            }
            
            case OpCode::JUMP_IF_FALSE: {
                uint16_t offset = readShort(bytecode, ip);
                Value condition = pop();
                if (!condition.isTruthy()) {
                    ip = offset;
                }
                break;
            }
            
            case OpCode::RETURN:
                // Return from current function
                return;
            
            case OpCode::HALT:
                return;
            
            default:
                throw std::runtime_error("Unknown opcode");
        }
    }
}

} // namespace omscript
