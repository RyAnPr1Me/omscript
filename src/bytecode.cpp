#include "bytecode.h"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace omscript {

void BytecodeEmitter::emit(OpCode op) {
    code.push_back(static_cast<uint8_t>(op));
}

void BytecodeEmitter::emitByte(uint8_t byte) {
    code.push_back(byte);
}

void BytecodeEmitter::emitInt(int64_t value) {
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(value));
    for (int i = 0; i < 8; i++) {
        code.push_back(static_cast<uint8_t>((raw >> (i * 8)) & 0xFF));
    }
}

void BytecodeEmitter::emitFloat(double value) {
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(value));
    for (int i = 0; i < 8; i++) {
        code.push_back(static_cast<uint8_t>((raw >> (i * 8)) & 0xFF));
    }
}

void BytecodeEmitter::emitString(const std::string& str) {
    if (str.length() > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("String literal too long for bytecode encoding");
    }
    emitShort(static_cast<uint16_t>(str.length()));
    for (char c : str) {
        code.push_back(static_cast<uint8_t>(c));
    }
}

void BytecodeEmitter::emitShort(uint16_t value) {
    code.push_back(static_cast<uint8_t>(value & 0xFF));
    code.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

size_t BytecodeEmitter::currentOffset() const {
    return code.size();
}

void BytecodeEmitter::patchJump(size_t offset, uint16_t jump) {
    if (offset + 1 >= code.size()) {
        throw std::runtime_error("Invalid jump patch offset");
    }
    code[offset] = static_cast<uint8_t>(jump & 0xFF);
    code[offset + 1] = static_cast<uint8_t>((jump >> 8) & 0xFF);
}

} // namespace omscript
