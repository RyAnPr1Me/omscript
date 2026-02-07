#include "bytecode.h"
#include <cstring>

namespace omscript {

void BytecodeEmitter::emit(OpCode op) {
    code.push_back(static_cast<uint8_t>(op));
}

void BytecodeEmitter::emitByte(uint8_t byte) {
    code.push_back(byte);
}

void BytecodeEmitter::emitInt(int64_t value) {
    uint8_t bytes[8];
    std::memcpy(bytes, &value, sizeof(value));
    for (int i = 0; i < 8; i++) {
        code.push_back(bytes[i]);
    }
}

void BytecodeEmitter::emitFloat(double value) {
    uint8_t bytes[8];
    std::memcpy(bytes, &value, sizeof(value));
    for (int i = 0; i < 8; i++) {
        code.push_back(bytes[i]);
    }
}

void BytecodeEmitter::emitString(const std::string& str) {
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
    code[offset] = static_cast<uint8_t>(jump & 0xFF);
    code[offset + 1] = static_cast<uint8_t>((jump >> 8) & 0xFF);
}

} // namespace omscript
