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
    size_t pos = code.size();
    code.resize(pos + sizeof(value));
    std::memcpy(code.data() + pos, &value, sizeof(value));
}

void BytecodeEmitter::emitFloat(double value) {
    size_t pos = code.size();
    code.resize(pos + sizeof(value));
    std::memcpy(code.data() + pos, &value, sizeof(value));
}

void BytecodeEmitter::emitString(const std::string& str) {
    if (str.length() > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("String literal too long for bytecode encoding");
    }
    emitShort(static_cast<uint16_t>(str.length()));
    size_t pos = code.size();
    code.resize(pos + str.length());
    std::memcpy(code.data() + pos, str.data(), str.length());
}

void BytecodeEmitter::emitShort(uint16_t value) {
    size_t pos = code.size();
    code.resize(pos + sizeof(value));
    std::memcpy(code.data() + pos, &value, sizeof(value));
}

size_t BytecodeEmitter::currentOffset() const {
    return code.size();
}

void BytecodeEmitter::patchJump(size_t offset, uint16_t jump) {
    if (offset + 2 > code.size()) {
        throw std::runtime_error("Invalid jump patch offset");
    }
    code[offset] = static_cast<uint8_t>(jump & 0xFF);
    code[offset + 1] = static_cast<uint8_t>((jump >> 8) & 0xFF);
}

} // namespace omscript
