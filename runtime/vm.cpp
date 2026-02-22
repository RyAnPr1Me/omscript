#include "vm.h"
#include "../include/bytecode.h"
#include "jit.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace omscript {

using BytecodeIteratorDiff = std::vector<uint8_t>::difference_type;

// Maximum valid shift amount for 64-bit integers (0-63).
static constexpr int64_t kInt64BitWidth = 64;

VM::VM() : lastReturn(), jit_(std::make_unique<BytecodeJIT>()) {
    locals.reserve(16);
    std::fill_n(registers, kMaxRegisters, Value());
}

VM::~VM() = default;

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

bool VM::invokeJIT(JITFnPtr fn, uint8_t argCount, const uint8_t* argRegs, uint8_t rd) {
    for (size_t i = 0; i < argCount; i++) {
        if (argRegs[i] >= kMaxRegisters)
            return false;
        if (registers[argRegs[i]].getType() != Value::Type::INTEGER)
            return false;
    }
    int64_t stackArgs[kMaxStackArgs];
    std::unique_ptr<int64_t[]> heapArgs;
    int64_t* args = stackArgs;
    if (argCount > kMaxStackArgs) {
        heapArgs = std::make_unique<int64_t[]>(argCount);
        args = heapArgs.get();
    }
    for (size_t i = 0; i < argCount; i++)
        args[i] = registers[argRegs[i]].unsafeAsInt();
    int64_t result = fn(args, static_cast<int>(argCount));
    registers[rd] = Value(result);
    return true;
}

bool VM::invokeJITFloat(JITFloatFnPtr fn, uint8_t argCount, const uint8_t* argRegs, uint8_t rd) {
    for (size_t i = 0; i < argCount; i++) {
        if (argRegs[i] >= kMaxRegisters)
            return false;
        if (registers[argRegs[i]].getType() != Value::Type::FLOAT)
            return false;
    }
    double stackArgs[kMaxStackArgs];
    std::unique_ptr<double[]> heapArgs;
    double* args = stackArgs;
    if (argCount > kMaxStackArgs) {
        heapArgs = std::make_unique<double[]>(argCount);
        args = heapArgs.get();
    }
    for (size_t i = 0; i < argCount; i++)
        args[i] = registers[argRegs[i]].unsafeAsFloat();
    double result = fn(args, static_cast<int>(argCount));
    registers[rd] = Value(result);
    return true;
}

void VM::classifyArgTypes(uint8_t argCount, const uint8_t* argRegs, bool& allInt, bool& allFloat) const {
    allInt = true;
    allFloat = true;
    for (size_t i = 0; i < argCount; i++) {
        if (argRegs[i] >= kMaxRegisters) {
            allInt = false;
            allFloat = false;
            return;
        }
        auto t = registers[argRegs[i]].getType();
        if (t != Value::Type::INTEGER)
            allInt = false;
        if (t != Value::Type::FLOAT)
            allFloat = false;
    }
}

void VM::execute(const std::vector<uint8_t>& bytecode) {
    size_t ip = 0;
    lastReturn = Value();
    std::fill_n(registers, kMaxRegisters, Value());

// Use computed-goto dispatch on GCC/Clang for faster opcode dispatch.
#if defined(__GNUC__) || defined(__clang__)
#define USE_COMPUTED_GOTO 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#else
#define USE_COMPUTED_GOTO 0
#endif

#if USE_COMPUTED_GOTO
    static const void* dispatchTable[] = {
        &&op_PUSH_INT,      // 0
        &&op_PUSH_FLOAT,    // 1
        &&op_PUSH_STRING,   // 2
        &&op_POP,           // 3
        &&op_ADD,           // 4
        &&op_SUB,           // 5
        &&op_MUL,           // 6
        &&op_DIV,           // 7
        &&op_MOD,           // 8
        &&op_NEG,           // 9
        &&op_EQ,            // 10
        &&op_NE,            // 11
        &&op_LT,            // 12
        &&op_LE,            // 13
        &&op_GT,            // 14
        &&op_GE,            // 15
        &&op_AND,           // 16
        &&op_OR,            // 17
        &&op_NOT,           // 18
        &&op_BIT_AND,       // 19
        &&op_BIT_OR,        // 20
        &&op_BIT_XOR,       // 21
        &&op_BIT_NOT,       // 22
        &&op_SHL,           // 23
        &&op_SHR,           // 24
        &&op_LOAD_VAR,      // 25
        &&op_STORE_VAR,     // 26
        &&op_LOAD_LOCAL,    // 27
        &&op_STORE_LOCAL,   // 28
        &&op_JUMP,          // 29
        &&op_JUMP_IF_FALSE, // 30
        &&op_CALL,          // 31
        &&op_RETURN,        // 32
        &&op_PRINT,         // 33
        &&op_DUP,           // 34
        &&op_HALT,          // 35
        &&op_MOV,           // 36
    };

    static constexpr size_t kDispatchTableSize = sizeof(dispatchTable) / sizeof(dispatchTable[0]);

#define DISPATCH()                                                                                                     \
    do {                                                                                                               \
        if (ip >= bytecode.size())                                                                                     \
            goto vm_exit;                                                                                              \
        uint8_t opByte = readByte(bytecode, ip);                                                                       \
        if (opByte >= kDispatchTableSize)                                                                              \
            goto op_UNKNOWN;                                                                                           \
        goto* dispatchTable[opByte];                                                                                   \
    } while (0)

    DISPATCH();

op_PUSH_INT: {
    uint8_t rd = readByte(bytecode, ip);
    int64_t value = readInt(bytecode, ip);
    registers[rd] = Value(value);
    DISPATCH();
}
op_PUSH_FLOAT: {
    uint8_t rd = readByte(bytecode, ip);
    double value = readFloat(bytecode, ip);
    registers[rd] = Value(value);
    DISPATCH();
}
op_PUSH_STRING: {
    uint8_t rd = readByte(bytecode, ip);
    std::string value = readString(bytecode, ip);
    registers[rd] = Value(value);
    DISPATCH();
}
op_POP: {
    // No-op in register-based mode
    DISPATCH();
}
op_ADD: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() + registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = registers[rs1] + registers[rs2];
    DISPATCH();
}
op_SUB: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() - registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = registers[rs1] - registers[rs2];
    DISPATCH();
}
op_MUL: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() * registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = registers[rs1] * registers[rs2];
    DISPATCH();
}
op_DIV: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        int64_t av = registers[rs1].unsafeAsInt();
        int64_t bv = registers[rs2].unsafeAsInt();
        // Guard against division by zero AND INT64_MIN / -1 overflow (UB).
        if (bv != 0 && !(av == INT64_MIN && bv == -1)) {
            registers[rd] = Value(av / bv);
            DISPATCH();
        }
    }
    registers[rd] = registers[rs1] / registers[rs2];
    DISPATCH();
}
op_MOD: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        int64_t av = registers[rs1].unsafeAsInt();
        int64_t bv = registers[rs2].unsafeAsInt();
        // Guard against modulo by zero; INT64_MIN % -1 is handled by
        // the Value::operator% fallback which returns 0.
        if (bv != 0 && !(av == INT64_MIN && bv == -1)) {
            registers[rd] = Value(av % bv);
            DISPATCH();
        }
    }
    registers[rd] = registers[rs1] % registers[rs2];
    DISPATCH();
}
op_NEG: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs = readByte(bytecode, ip);
    if (registers[rs].getType() == Value::Type::INTEGER) {
        int64_t val = registers[rs].unsafeAsInt();
        // Guard against -INT64_MIN overflow; fall through to Value::operator-
        // which throws a descriptive error.
        if (val != INT64_MIN) {
            registers[rd] = Value(-val);
            DISPATCH();
        }
    }
    registers[rd] = -registers[rs];
    DISPATCH();
}
op_EQ: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() == registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] == registers[rs2]));
    DISPATCH();
}
op_NE: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() != registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] != registers[rs2]));
    DISPATCH();
}
op_LT: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() < registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] < registers[rs2]));
    DISPATCH();
}
op_LE: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() <= registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] <= registers[rs2]));
    DISPATCH();
}
op_GT: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() > registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] > registers[rs2]));
    DISPATCH();
}
op_GE: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() >= registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] >= registers[rs2]));
    DISPATCH();
}
op_AND: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] =
            Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() != 0 && registers[rs2].unsafeAsInt() != 0));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1].isTruthy() && registers[rs2].isTruthy()));
    DISPATCH();
}
op_OR: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] =
            Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() != 0 || registers[rs2].unsafeAsInt() != 0));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1].isTruthy() || registers[rs2].isTruthy()));
    DISPATCH();
}
op_NOT: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs = readByte(bytecode, ip);
    if (registers[rs].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs].unsafeAsInt() == 0));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(!registers[rs].isTruthy()));
    DISPATCH();
}
op_BIT_AND: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() & registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = registers[rs1] & registers[rs2];
    DISPATCH();
}
op_BIT_OR: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() | registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = registers[rs1] | registers[rs2];
    DISPATCH();
}
op_BIT_XOR: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() ^ registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = registers[rs1] ^ registers[rs2];
    DISPATCH();
}
op_BIT_NOT: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs = readByte(bytecode, ip);
    if (registers[rs].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(~registers[rs].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = ~registers[rs];
    DISPATCH();
}
op_SHL: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        int64_t bv = registers[rs2].unsafeAsInt();
        if (bv >= 0 && bv < kInt64BitWidth) {
            registers[rd] = Value(registers[rs1].unsafeAsInt() << bv);
            DISPATCH();
        }
    }
    registers[rd] = registers[rs1] << registers[rs2];
    DISPATCH();
}
op_SHR: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs1 = readByte(bytecode, ip);
    uint8_t rs2 = readByte(bytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        int64_t bv = registers[rs2].unsafeAsInt();
        if (bv >= 0 && bv < kInt64BitWidth) {
            registers[rd] = Value(registers[rs1].unsafeAsInt() >> bv);
            DISPATCH();
        }
    }
    registers[rd] = registers[rs1] >> registers[rs2];
    DISPATCH();
}
op_LOAD_VAR: {
    uint8_t rd = readByte(bytecode, ip);
    std::string name = readString(bytecode, ip);
    registers[rd] = getGlobal(name);
    DISPATCH();
}
op_STORE_VAR: {
    uint8_t rs = readByte(bytecode, ip);
    std::string name = readString(bytecode, ip);
    setGlobal(name, registers[rs]);
    DISPATCH();
}
op_LOAD_LOCAL: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t index = readByte(bytecode, ip);
    if (index >= locals.size()) {
        throw std::runtime_error("Local variable index out of range: " + std::to_string(index) + " at ip " +
                                 std::to_string(ip - 1));
    }
    registers[rd] = locals[index];
    DISPATCH();
}
op_STORE_LOCAL: {
    uint8_t index = readByte(bytecode, ip);
    uint8_t rs = readByte(bytecode, ip);
    if (index >= locals.size()) {
        if (static_cast<size_t>(index) + 1 > kMaxLocals) {
            throw std::runtime_error("Local variable index exceeds maximum (" + std::to_string(kMaxLocals) + ")");
        }
        locals.resize(static_cast<size_t>(index) + 1);
    }
    locals[index] = registers[rs];
    DISPATCH();
}
op_JUMP: {
    uint16_t offset = readShort(bytecode, ip);
    if (offset >= bytecode.size()) {
        throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
    }
    ip = offset;
    DISPATCH();
}
op_JUMP_IF_FALSE: {
    uint8_t rs = readByte(bytecode, ip);
    uint16_t offset = readShort(bytecode, ip);
    if (registers[rs].getType() == Value::Type::INTEGER) {
        if (registers[rs].unsafeAsInt() == 0) {
            if (offset >= bytecode.size()) {
                throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
            }
            ip = offset;
        }
        DISPATCH();
    }
    if (!registers[rs].isTruthy()) {
        if (offset >= bytecode.size()) {
            throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
        }
        ip = offset;
    }
    DISPATCH();
}
op_RETURN: {
    uint8_t rs = readByte(bytecode, ip);
    lastReturn = registers[rs];
    return;
}
op_PRINT: {
    uint8_t rs = readByte(bytecode, ip);
    std::cout << registers[rs].toString() << std::endl;
    DISPATCH();
}
op_DUP: {
    // No-op in register-based mode
    DISPATCH();
}
op_HALT: {
    lastReturn = Value();
    return;
}
op_MOV: {
    uint8_t rd = readByte(bytecode, ip);
    uint8_t rs = readByte(bytecode, ip);
    registers[rd] = registers[rs];
    DISPATCH();
}
op_CALL: {
    uint8_t rd = readByte(bytecode, ip);
    std::string funcName = readString(bytecode, ip);
    uint8_t argCount = readByte(bytecode, ip);
    uint8_t argRegs[256];
    for (uint8_t i = 0; i < argCount; i++) {
        argRegs[i] = readByte(bytecode, ip);
    }

    // ---- Record argument types for type-profiled JIT ----
    if (jit_ && argCount > 0) {
        bool allInt, allFloat;
        classifyArgTypes(argCount, argRegs, allInt, allFloat);
        jit_->recordTypes(funcName, allInt, allFloat);
    }

    // ---- JIT fast path: float-specialized ----
    {
        auto floatIt = jitFloatCache_.find(funcName);
        if (floatIt != jitFloatCache_.end()) {
            if (invokeJITFloat(floatIt->second, argCount, argRegs, rd)) {
                if (jit_)
                    jit_->recordPostJITCall(funcName);
                DISPATCH();
            }
        }
    }

    // ---- JIT fast path: int-specialized ----
    {
        auto cacheIt = jitCache_.find(funcName);
        if (cacheIt != jitCache_.end()) {
            if (invokeJIT(cacheIt->second, argCount, argRegs, rd)) {
                if (jit_ && jit_->recordPostJITCall(funcName)) {
                    auto fit = functions.find(funcName);
                    if (fit != functions.end()) {
                        jit_->recompile(fit->second);
                        auto newIntPtr = jit_->getCompiled(funcName);
                        auto newFloatPtr = jit_->getCompiledFloat(funcName);
                        if (newIntPtr)
                            jitCache_[funcName] = newIntPtr;
                        if (newFloatPtr)
                            jitFloatCache_[funcName] = newFloatPtr;
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
                auto spec = jit_->getTypeProfile(funcName).bestSpecialization();
                if (jit_->compile(fit->second, spec)) {
                    auto intPtr = jit_->getCompiled(funcName);
                    auto floatPtr = jit_->getCompiledFloat(funcName);
                    if (intPtr)
                        jitCache_[funcName] = intPtr;
                    if (floatPtr)
                        jitFloatCache_[funcName] = floatPtr;
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
        throw std::runtime_error("Function '" + funcName + "' expects " + std::to_string(func.arity) +
                                 " arguments but got " + std::to_string(argCount));
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
    frame.savedRegisters.assign(registers, registers + kMaxRegisters);
    frame.returnReg = rd;
    callStack.push_back(std::move(frame));

    locals.clear();
    locals.resize(argCount);
    for (uint8_t i = 0; i < argCount; i++) {
        locals[i] = callStack.back().savedRegisters[argRegs[i]];
    }

    execute(func.bytecode);

    Value returnValue = lastReturn;
    CallFrame& top = callStack.back();
    locals = std::move(top.savedLocals);
    std::copy(top.savedRegisters.begin(), top.savedRegisters.end(), registers);
    uint8_t retReg = top.returnReg;
    callStack.pop_back();

    registers[retReg] = returnValue;
    DISPATCH();
}

op_UNKNOWN:
    throw std::runtime_error("Unknown opcode " + std::to_string(bytecode[ip - 1]) + " at ip " + std::to_string(ip - 1));

vm_exit:
    return;

#undef DISPATCH
#undef USE_COMPUTED_GOTO

#else  // Fallback: standard switch dispatch

    while (ip < bytecode.size()) {
        OpCode op = static_cast<OpCode>(readByte(bytecode, ip));

        switch (op) {
        case OpCode::PUSH_INT: {
            uint8_t rd = readByte(bytecode, ip);
            int64_t value = readInt(bytecode, ip);
            registers[rd] = Value(value);
            break;
        }
        case OpCode::PUSH_FLOAT: {
            uint8_t rd = readByte(bytecode, ip);
            double value = readFloat(bytecode, ip);
            registers[rd] = Value(value);
            break;
        }
        case OpCode::PUSH_STRING: {
            uint8_t rd = readByte(bytecode, ip);
            std::string value = readString(bytecode, ip);
            registers[rd] = Value(value);
            break;
        }
        case OpCode::POP:
            break;
        case OpCode::ADD:
        case OpCode::SUB:
        case OpCode::MUL:
        case OpCode::DIV:
        case OpCode::MOD: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t rs1 = readByte(bytecode, ip);
            uint8_t rs2 = readByte(bytecode, ip);
            switch (op) {
            case OpCode::ADD:
                registers[rd] = registers[rs1] + registers[rs2];
                break;
            case OpCode::SUB:
                registers[rd] = registers[rs1] - registers[rs2];
                break;
            case OpCode::MUL:
                registers[rd] = registers[rs1] * registers[rs2];
                break;
            case OpCode::DIV:
                registers[rd] = registers[rs1] / registers[rs2];
                break;
            case OpCode::MOD:
                registers[rd] = registers[rs1] % registers[rs2];
                break;
            default:
                break;
            }
            break;
        }
        case OpCode::NEG: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t rs = readByte(bytecode, ip);
            registers[rd] = -registers[rs];
            break;
        }
        case OpCode::EQ:
        case OpCode::NE:
        case OpCode::LT:
        case OpCode::LE:
        case OpCode::GT:
        case OpCode::GE: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t rs1 = readByte(bytecode, ip);
            uint8_t rs2 = readByte(bytecode, ip);
            switch (op) {
            case OpCode::EQ:
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] == registers[rs2]));
                break;
            case OpCode::NE:
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] != registers[rs2]));
                break;
            case OpCode::LT:
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] < registers[rs2]));
                break;
            case OpCode::LE:
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] <= registers[rs2]));
                break;
            case OpCode::GT:
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] > registers[rs2]));
                break;
            case OpCode::GE:
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] >= registers[rs2]));
                break;
            default:
                break;
            }
            break;
        }
        case OpCode::AND: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t rs1 = readByte(bytecode, ip);
            uint8_t rs2 = readByte(bytecode, ip);
            registers[rd] = Value(static_cast<int64_t>(registers[rs1].isTruthy() && registers[rs2].isTruthy()));
            break;
        }
        case OpCode::OR: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t rs1 = readByte(bytecode, ip);
            uint8_t rs2 = readByte(bytecode, ip);
            registers[rd] = Value(static_cast<int64_t>(registers[rs1].isTruthy() || registers[rs2].isTruthy()));
            break;
        }
        case OpCode::NOT: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t rs = readByte(bytecode, ip);
            registers[rd] = Value(static_cast<int64_t>(!registers[rs].isTruthy()));
            break;
        }
        case OpCode::BIT_AND:
        case OpCode::BIT_OR:
        case OpCode::BIT_XOR:
        case OpCode::SHL:
        case OpCode::SHR: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t rs1 = readByte(bytecode, ip);
            uint8_t rs2 = readByte(bytecode, ip);
            switch (op) {
            case OpCode::BIT_AND:
                registers[rd] = registers[rs1] & registers[rs2];
                break;
            case OpCode::BIT_OR:
                registers[rd] = registers[rs1] | registers[rs2];
                break;
            case OpCode::BIT_XOR:
                registers[rd] = registers[rs1] ^ registers[rs2];
                break;
            case OpCode::SHL:
                registers[rd] = registers[rs1] << registers[rs2];
                break;
            case OpCode::SHR:
                registers[rd] = registers[rs1] >> registers[rs2];
                break;
            default:
                break;
            }
            break;
        }
        case OpCode::BIT_NOT: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t rs = readByte(bytecode, ip);
            registers[rd] = ~registers[rs];
            break;
        }
        case OpCode::LOAD_VAR: {
            uint8_t rd = readByte(bytecode, ip);
            std::string name = readString(bytecode, ip);
            registers[rd] = getGlobal(name);
            break;
        }
        case OpCode::STORE_VAR: {
            uint8_t rs = readByte(bytecode, ip);
            std::string name = readString(bytecode, ip);
            setGlobal(name, registers[rs]);
            break;
        }
        case OpCode::LOAD_LOCAL: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t index = readByte(bytecode, ip);
            if (index >= locals.size()) {
                throw std::runtime_error("Local variable index out of range: " + std::to_string(index) + " at ip " +
                                         std::to_string(ip - 1));
            }
            registers[rd] = locals[index];
            break;
        }
        case OpCode::STORE_LOCAL: {
            uint8_t index = readByte(bytecode, ip);
            uint8_t rs = readByte(bytecode, ip);
            if (index >= locals.size()) {
                if (static_cast<size_t>(index) + 1 > kMaxLocals) {
                    throw std::runtime_error("Local variable index exceeds maximum (" + std::to_string(kMaxLocals) +
                                             ")");
                }
                locals.resize(static_cast<size_t>(index) + 1);
            }
            locals[index] = registers[rs];
            break;
        }
        case OpCode::JUMP: {
            uint16_t offset = readShort(bytecode, ip);
            if (offset >= bytecode.size()) {
                throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
            }
            ip = offset;
            break;
        }
        case OpCode::JUMP_IF_FALSE: {
            uint8_t rs = readByte(bytecode, ip);
            uint16_t offset = readShort(bytecode, ip);
            if (!registers[rs].isTruthy()) {
                if (offset >= bytecode.size()) {
                    throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
                }
                ip = offset;
            }
            break;
        }
        case OpCode::RETURN: {
            uint8_t rs = readByte(bytecode, ip);
            lastReturn = registers[rs];
            return;
        }
        case OpCode::PRINT: {
            uint8_t rs = readByte(bytecode, ip);
            std::cout << registers[rs].toString() << std::endl;
            break;
        }
        case OpCode::DUP:
            break;
        case OpCode::HALT:
            lastReturn = Value();
            return;
        case OpCode::MOV: {
            uint8_t rd = readByte(bytecode, ip);
            uint8_t rs = readByte(bytecode, ip);
            registers[rd] = registers[rs];
            break;
        }
        case OpCode::CALL: {
            uint8_t rd = readByte(bytecode, ip);
            std::string funcName = readString(bytecode, ip);
            uint8_t argCount = readByte(bytecode, ip);
            uint8_t argRegs[256];
            for (uint8_t i = 0; i < argCount; i++) {
                argRegs[i] = readByte(bytecode, ip);
            }

            if (jit_ && argCount > 0) {
                bool allInt, allFloat;
                classifyArgTypes(argCount, argRegs, allInt, allFloat);
                jit_->recordTypes(funcName, allInt, allFloat);
            }

            {
                auto floatIt = jitFloatCache_.find(funcName);
                if (floatIt != jitFloatCache_.end()) {
                    if (invokeJITFloat(floatIt->second, argCount, argRegs, rd)) {
                        if (jit_)
                            jit_->recordPostJITCall(funcName);
                        break;
                    }
                }
            }
            {
                auto cacheIt = jitCache_.find(funcName);
                if (cacheIt != jitCache_.end()) {
                    if (invokeJIT(cacheIt->second, argCount, argRegs, rd)) {
                        if (jit_ && jit_->recordPostJITCall(funcName)) {
                            auto fit = functions.find(funcName);
                            if (fit != functions.end()) {
                                jit_->recompile(fit->second);
                                auto newIntPtr = jit_->getCompiled(funcName);
                                auto newFloatPtr = jit_->getCompiledFloat(funcName);
                                if (newIntPtr)
                                    jitCache_[funcName] = newIntPtr;
                                if (newFloatPtr)
                                    jitFloatCache_[funcName] = newFloatPtr;
                            }
                        }
                        break;
                    }
                }
            }
            if (jit_) {
                if (jit_->recordCall(funcName)) {
                    auto fit = functions.find(funcName);
                    if (fit != functions.end()) {
                        auto spec = jit_->getTypeProfile(funcName).bestSpecialization();
                        if (jit_->compile(fit->second, spec)) {
                            auto intPtr = jit_->getCompiled(funcName);
                            auto floatPtr = jit_->getCompiledFloat(funcName);
                            if (intPtr)
                                jitCache_[funcName] = intPtr;
                            if (floatPtr)
                                jitFloatCache_[funcName] = floatPtr;
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
                throw std::runtime_error("Function '" + funcName + "' expects " + std::to_string(func.arity) +
                                         " arguments but got " + std::to_string(argCount));
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
            frame.savedRegisters.assign(registers, registers + kMaxRegisters);
            frame.returnReg = rd;
            callStack.push_back(std::move(frame));

            locals.clear();
            locals.resize(argCount);
            for (uint8_t i = 0; i < argCount; i++) {
                locals[i] = callStack.back().savedRegisters[argRegs[i]];
            }

            execute(func.bytecode);

            Value returnValue = lastReturn;
            CallFrame& top = callStack.back();
            locals = std::move(top.savedLocals);
            std::copy(top.savedRegisters.begin(), top.savedRegisters.end(), registers);
            uint8_t retReg = top.returnReg;
            callStack.pop_back();

            registers[retReg] = returnValue;
            break;
        }
        default:
            throw std::runtime_error("Unknown opcode " + std::to_string(static_cast<uint8_t>(op)) + " at ip " +
                                     std::to_string(ip - 1));
        }
    }
#endif // USE_COMPUTED_GOTO
}

} // namespace omscript
