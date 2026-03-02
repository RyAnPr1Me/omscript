#include "vm.h"
#include "../include/bytecode.h"
#include "jit.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace omscript {

// Maximum valid shift amount for 64-bit integers (0-63).
static constexpr int64_t kInt64BitWidth = 64;

// Maximum exponent value for the ** operator to prevent resource exhaustion.
static constexpr int64_t kMaxExponent = 1000;

/// Return true if multiplying a * b would overflow int64_t.
/// Uses unsigned arithmetic to avoid undefined behavior.
static inline bool wouldMultiplyOverflow(int64_t a, int64_t b) {
    if (a == 0 || a == 1 || a == -1 || b == 0 || b == 1 || b == -1)
        return false;
    uint64_t ua = (a < 0) ? static_cast<uint64_t>(-static_cast<uint64_t>(a))
                           : static_cast<uint64_t>(a);
    uint64_t ub = (b < 0) ? static_cast<uint64_t>(-static_cast<uint64_t>(b))
                           : static_cast<uint64_t>(b);
    return ua > static_cast<uint64_t>(INT64_MAX) / ub;
}

VM::VM() : lastReturn(), jit_(std::make_unique<BytecodeJIT>()) {
    locals.reserve(16);
    callStack.reserve(16);
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
    uint16_t value = 0;
    std::memcpy(&value, code.data() + ip, sizeof(value));
    ip += 2;
    return value;
}

int64_t VM::readInt(const std::vector<uint8_t>& code, size_t& ip) {
    ensureReadable(code, ip, 8);
    int64_t value = 0;
    std::memcpy(&value, code.data() + ip, sizeof(value));
    ip += 8;
    return value;
}

double VM::readFloat(const std::vector<uint8_t>& code, size_t& ip) {
    ensureReadable(code, ip, 8);
    double value = 0.0;
    std::memcpy(&value, code.data() + ip, sizeof(value));
    ip += 8;
    return value;
}

std::string VM::readString(const std::vector<uint8_t>& code, size_t& ip) {
    uint16_t length = readShort(code, ip);
    ensureReadable(code, ip, length);
    const char* ptr = reinterpret_cast<const char*>(code.data() + ip);
    ip += length;
    return std::string(ptr, length);
}

std::string_view VM::readStringView(const std::vector<uint8_t>& code, size_t& ip) {
    uint16_t length = readShort(code, ip);
    ensureReadable(code, ip, length);
    const char* ptr = reinterpret_cast<const char*>(code.data() + ip);
    ip += length;
    return std::string_view(ptr, length);
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

void VM::registerFunction(BytecodeFunction&& func) {
    functions[func.name] = std::move(func);
}

bool VM::isJITCompiled(const std::string& name) const {
    return jit_ && jit_->isCompiled(name);
}

bool VM::invokeJIT(JITFnPtr fn, uint8_t argCount, const uint8_t* argRegs, uint8_t rd) {
    if (rd >= kMaxRegisters)
        return false;
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
    if (rd > maxRegUsed_) maxRegUsed_ = rd;
    return true;
}

bool VM::invokeJITFloat(JITFloatFnPtr fn, uint8_t argCount, const uint8_t* argRegs, uint8_t rd) {
    if (rd >= kMaxRegisters)
        return false;
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
    if (rd > maxRegUsed_) maxRegUsed_ = rd;
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

    // Use a mutable pointer so CALL/RETURN can switch bytecode streams
    // without recursive execute() calls.
    const std::vector<uint8_t>* curBytecode = &bytecode;
    // Track the initial call depth so RETURN knows when we've returned
    // from the top-level invocation vs. a nested bytecode call.
    const size_t baseCallDepth = callStack.size();

    // Branchless high-water-mark update for register writes.
    // Uses conditional move (cmov) on x86 — no branch misprediction.
#define TRACK_REG(r) do { if ((r) > maxRegUsed_) maxRegUsed_ = (r); } while (0)

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
        &&op_POW,           // 10
        &&op_EQ,            // 11
        &&op_NE,            // 12
        &&op_LT,            // 13
        &&op_LE,            // 14
        &&op_GT,            // 15
        &&op_GE,            // 16
        &&op_AND,           // 17
        &&op_OR,            // 18
        &&op_NOT,           // 19
        &&op_BIT_AND,       // 20
        &&op_BIT_OR,        // 21
        &&op_BIT_XOR,       // 22
        &&op_BIT_NOT,       // 23
        &&op_SHL,           // 24
        &&op_SHR,           // 25
        &&op_LOAD_VAR,      // 26
        &&op_STORE_VAR,     // 27
        &&op_LOAD_LOCAL,    // 28
        &&op_STORE_LOCAL,   // 29
        &&op_JUMP,          // 30
        &&op_JUMP_IF_FALSE, // 31
        &&op_CALL,          // 32
        &&op_RETURN,        // 33
        &&op_PRINT,         // 34
        &&op_DUP,           // 35
        &&op_HALT,          // 36
        &&op_MOV,           // 37
    };

    static constexpr size_t kDispatchTableSize = sizeof(dispatchTable) / sizeof(dispatchTable[0]);

#define DISPATCH()                                                                                                     \
    do {                                                                                                               \
        if (ip >= curBytecode->size())                                                                                 \
            goto vm_exit;                                                                                              \
        uint8_t opByte = readByte(*curBytecode, ip);                                                                   \
        if (opByte >= kDispatchTableSize)                                                                              \
            goto op_UNKNOWN;                                                                                           \
        goto* dispatchTable[opByte];                                                                                   \
    } while (0)

    DISPATCH();

op_PUSH_INT: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    int64_t value = readInt(*curBytecode, ip);
    registers[rd] = Value(value);
    DISPATCH();
}
op_PUSH_FLOAT: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    double value = readFloat(*curBytecode, ip);
    registers[rd] = Value(value);
    DISPATCH();
}
op_PUSH_STRING: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    {
        std::string value = readString(*curBytecode, ip);
        registers[rd] = Value(value);
    }
    DISPATCH();
}
op_POP: {
    // No-op in register-based mode
    DISPATCH();
}
op_ADD: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() + registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(registers[rs1].unsafeAsFloat() + registers[rs2].unsafeAsFloat());
        DISPATCH();
    }
    registers[rd] = registers[rs1] + registers[rs2];
    DISPATCH();
}
op_SUB: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() - registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(registers[rs1].unsafeAsFloat() - registers[rs2].unsafeAsFloat());
        DISPATCH();
    }
    registers[rd] = registers[rs1] - registers[rs2];
    DISPATCH();
}
op_MUL: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() * registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(registers[rs1].unsafeAsFloat() * registers[rs2].unsafeAsFloat());
        DISPATCH();
    }
    registers[rd] = registers[rs1] * registers[rs2];
    DISPATCH();
}
op_DIV: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        int64_t av = registers[rs1].unsafeAsInt();
        int64_t bv = registers[rs2].unsafeAsInt();
        // Guard against division by zero AND INT64_MIN / -1 overflow (UB).
        if (bv != 0 && !(av == INT64_MIN && bv == -1)) {
            registers[rd] = Value(av / bv);
            DISPATCH();
        }
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        double bv = registers[rs2].unsafeAsFloat();
        if (bv != 0.0) {
            registers[rd] = Value(registers[rs1].unsafeAsFloat() / bv);
            DISPATCH();
        }
    }
    registers[rd] = registers[rs1] / registers[rs2];
    DISPATCH();
}
op_MOD: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
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
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs = readByte(*curBytecode, ip);
    if (registers[rs].getType() == Value::Type::INTEGER) {
        int64_t val = registers[rs].unsafeAsInt();
        // Guard against -INT64_MIN overflow; fall through to Value::operator-
        // which throws a descriptive error.
        if (val != INT64_MIN) {
            registers[rd] = Value(-val);
            DISPATCH();
        }
    }
    if (registers[rs].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(-registers[rs].unsafeAsFloat());
        DISPATCH();
    }
    registers[rd] = -registers[rs];
    DISPATCH();
}
op_POW: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    {
        Value base = registers[rs1];
        Value exp = registers[rs2];
        if (exp.getType() != Value::Type::INTEGER) {
            throw std::runtime_error("Exponent must be an integer for ** operator");
        }
        int64_t n = exp.unsafeAsInt();
        if (n < 0) {
            registers[rd] = Value(static_cast<int64_t>(0));
        } else {
            if (n > kMaxExponent) {
                throw std::runtime_error("Exponent too large for ** operator (max " + std::to_string(kMaxExponent) +
                                         ")");
            }
            // Fast path: integer base — use exponentiation by squaring O(log n)
            if (base.getType() == Value::Type::INTEGER) {
                int64_t b = base.unsafeAsInt();
                int64_t result = 1;
                int64_t e = n;
                while (e > 0) {
                    if (e & 1) {
                        if (wouldMultiplyOverflow(result, b))
                            throw std::runtime_error("Integer overflow in ** operator");
                        result *= b;
                    }
                    e >>= 1;
                    if (e > 0) {
                        if (wouldMultiplyOverflow(b, b))
                            throw std::runtime_error("Integer overflow in ** operator");
                        b *= b;
                    }
                }
                registers[rd] = Value(result);
            } else {
                Value result(static_cast<int64_t>(1));
                Value b = base;
                int64_t e = n;
                while (e > 0) {
                    if (e & 1)
                        result = result * b;
                    b = b * b;
                    e >>= 1;
                }
                registers[rd] = result;
            }
        }
    }
    DISPATCH();
}
op_EQ: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() == registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsFloat() == registers[rs2].unsafeAsFloat()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] == registers[rs2]));
    DISPATCH();
}
op_NE: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() != registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsFloat() != registers[rs2].unsafeAsFloat()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] != registers[rs2]));
    DISPATCH();
}
op_LT: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() < registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsFloat() < registers[rs2].unsafeAsFloat()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] < registers[rs2]));
    DISPATCH();
}
op_LE: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() <= registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsFloat() <= registers[rs2].unsafeAsFloat()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] <= registers[rs2]));
    DISPATCH();
}
op_GT: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() > registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsFloat() > registers[rs2].unsafeAsFloat()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] > registers[rs2]));
    DISPATCH();
}
op_GE: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() >= registers[rs2].unsafeAsInt()));
        DISPATCH();
    }
    if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsFloat() >= registers[rs2].unsafeAsFloat()));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1] >= registers[rs2]));
    DISPATCH();
}
op_AND: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] =
            Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() != 0 && registers[rs2].unsafeAsInt() != 0));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1].isTruthy() && registers[rs2].isTruthy()));
    DISPATCH();
}
op_OR: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] =
            Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() != 0 || registers[rs2].unsafeAsInt() != 0));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(registers[rs1].isTruthy() || registers[rs2].isTruthy()));
    DISPATCH();
}
op_NOT: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs = readByte(*curBytecode, ip);
    if (registers[rs].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(static_cast<int64_t>(registers[rs].unsafeAsInt() == 0));
        DISPATCH();
    }
    registers[rd] = Value(static_cast<int64_t>(!registers[rs].isTruthy()));
    DISPATCH();
}
op_BIT_AND: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() & registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = registers[rs1] & registers[rs2];
    DISPATCH();
}
op_BIT_OR: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() | registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = registers[rs1] | registers[rs2];
    DISPATCH();
}
op_BIT_XOR: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
    if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(registers[rs1].unsafeAsInt() ^ registers[rs2].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = registers[rs1] ^ registers[rs2];
    DISPATCH();
}
op_BIT_NOT: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs = readByte(*curBytecode, ip);
    if (registers[rs].getType() == Value::Type::INTEGER) {
        registers[rd] = Value(~registers[rs].unsafeAsInt());
        DISPATCH();
    }
    registers[rd] = ~registers[rs];
    DISPATCH();
}
op_SHL: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
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
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs1 = readByte(*curBytecode, ip);
    uint8_t rs2 = readByte(*curBytecode, ip);
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
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    {
        std::string name = readString(*curBytecode, ip);
        registers[rd] = getGlobal(name);
    }
    DISPATCH();
}
op_STORE_VAR: {
    uint8_t rs = readByte(*curBytecode, ip);
    {
        std::string name = readString(*curBytecode, ip);
        setGlobal(name, registers[rs]);
    }
    DISPATCH();
}
op_LOAD_LOCAL: {
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t index = readByte(*curBytecode, ip);
    if (index >= locals.size()) {
        throw std::runtime_error("Local variable index out of range: " + std::to_string(index) + " at ip " +
                                 std::to_string(ip - 1));
    }
    registers[rd] = locals[index];
    DISPATCH();
}
op_STORE_LOCAL: {
    uint8_t index = readByte(*curBytecode, ip);
    uint8_t rs = readByte(*curBytecode, ip);
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
    uint16_t offset = readShort(*curBytecode, ip);
    if (offset >= curBytecode->size()) {
        throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
    }
    ip = offset;
    DISPATCH();
}
op_JUMP_IF_FALSE: {
    uint8_t rs = readByte(*curBytecode, ip);
    uint16_t offset = readShort(*curBytecode, ip);
    if (registers[rs].getType() == Value::Type::INTEGER) {
        if (registers[rs].unsafeAsInt() == 0) {
            if (offset >= curBytecode->size()) {
                throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
            }
            ip = offset;
        }
        DISPATCH();
    }
    if (!registers[rs].isTruthy()) {
        if (offset >= curBytecode->size()) {
            throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
        }
        ip = offset;
    }
    DISPATCH();
}
op_RETURN: {
    uint8_t rs = readByte(*curBytecode, ip);
    lastReturn = registers[rs];
    // If there's a caller frame, restore it and continue iteratively.
    if (callStack.size() > baseCallDepth) {
        CallFrame& top = callStack.back();
        locals = std::move(top.savedLocals);
        // Restore only the registers that were actually saved (partial restore).
        std::copy(top.savedRegisters.begin(), top.savedRegisters.end(), registers);
        maxRegUsed_ = top.savedMaxReg;
        uint8_t retReg = top.returnReg;
        ip = top.returnIp;
        curBytecode = top.returnBytecode;
        callStack.pop_back();
        registers[retReg] = lastReturn;
        TRACK_REG(retReg);
        DISPATCH();
    }
    return;
}
op_PRINT: {
    uint8_t rs = readByte(*curBytecode, ip);
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
    uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
    uint8_t rs = readByte(*curBytecode, ip);
    registers[rd] = registers[rs];
    DISPATCH();
}
op_CALL: {
    do {
        uint8_t rd = readByte(*curBytecode, ip);
    TRACK_REG(rd);
        std::string funcName = readString(*curBytecode, ip);
        uint8_t argCount = readByte(*curBytecode, ip);
        uint8_t argRegs[256];
        for (uint8_t i = 0; i < argCount; i++) {
            argRegs[i] = readByte(*curBytecode, ip);
        }

        // ---- JIT fast path: float-specialized ----
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
                    break;
                }
            }
        }

        // ---- Record argument types for type-profiled JIT ----
        // Only profile when not already JIT-cached (the fast paths above
        // would have taken a break if execution was handled by JIT).
        if (jit_ && argCount > 0) {
            bool allInt, allFloat;
            classifyArgTypes(argCount, argRegs, allInt, allFloat);
            jit_->recordTypes(funcName, allInt, allFloat);
        }

        auto it = functions.find(funcName);

        // ---- JIT compilation trigger ----
        if (jit_) {
            if (jit_->recordCall(funcName)) {
                if (it != functions.end()) {
                    auto spec = jit_->getTypeProfile(funcName).bestSpecialization();
                    if (jit_->compile(it->second, spec)) {
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
        frame.returnBytecode = curBytecode;
        frame.savedLocals = std::move(locals);
        // Save only the registers actually used (high-water mark), not all 256.
        {
            size_t saveCount = static_cast<size_t>(maxRegUsed_) + 1;
            frame.savedRegisters.assign(registers, registers + saveCount);
        }
        frame.returnReg = rd;
        frame.savedMaxReg = maxRegUsed_;
        callStack.push_back(std::move(frame));

        locals.clear();
        locals.resize(argCount);
        for (uint8_t i = 0; i < argCount; i++) {
            locals[i] = callStack.back().savedRegisters[argRegs[i]];
        }

        // Switch to callee's bytecode iteratively (no recursive execute()).
        curBytecode = &func.bytecode;
        ip = 0;
        maxRegUsed_ = 0;
    } while (0);
    DISPATCH();
}

op_UNKNOWN:
    throw std::runtime_error("Unknown opcode " + std::to_string((*curBytecode)[ip - 1]) + " at ip " + std::to_string(ip - 1));

vm_exit:
    return;

#undef DISPATCH
#undef USE_COMPUTED_GOTO

#else  // Fallback: standard switch dispatch

    while (ip < curBytecode->size()) {
        OpCode op = static_cast<OpCode>(readByte(*curBytecode, ip));

        switch (op) {
        case OpCode::PUSH_INT: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            int64_t value = readInt(*curBytecode, ip);
            registers[rd] = Value(value);
            break;
        }
        case OpCode::PUSH_FLOAT: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            double value = readFloat(*curBytecode, ip);
            registers[rd] = Value(value);
            break;
        }
        case OpCode::PUSH_STRING: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            std::string value = readString(*curBytecode, ip);
            registers[rd] = Value(value);
            break;
        }
        case OpCode::POP:
            break;
        case OpCode::ADD: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(registers[rs1].unsafeAsInt() + registers[rs2].unsafeAsInt());
            } else if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
                registers[rd] = Value(registers[rs1].unsafeAsFloat() + registers[rs2].unsafeAsFloat());
            } else {
                registers[rd] = registers[rs1] + registers[rs2];
            }
            break;
        }
        case OpCode::SUB: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(registers[rs1].unsafeAsInt() - registers[rs2].unsafeAsInt());
            } else if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
                registers[rd] = Value(registers[rs1].unsafeAsFloat() - registers[rs2].unsafeAsFloat());
            } else {
                registers[rd] = registers[rs1] - registers[rs2];
            }
            break;
        }
        case OpCode::MUL: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(registers[rs1].unsafeAsInt() * registers[rs2].unsafeAsInt());
            } else if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
                registers[rd] = Value(registers[rs1].unsafeAsFloat() * registers[rs2].unsafeAsFloat());
            } else {
                registers[rd] = registers[rs1] * registers[rs2];
            }
            break;
        }
        case OpCode::DIV: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                int64_t av = registers[rs1].unsafeAsInt();
                int64_t bv = registers[rs2].unsafeAsInt();
                if (bv != 0 && !(av == INT64_MIN && bv == -1)) {
                    registers[rd] = Value(av / bv);
                    break;
                }
            } else if (registers[rs1].getType() == Value::Type::FLOAT && registers[rs2].getType() == Value::Type::FLOAT) {
                double bv = registers[rs2].unsafeAsFloat();
                if (bv != 0.0) {
                    registers[rd] = Value(registers[rs1].unsafeAsFloat() / bv);
                    break;
                }
            }
            registers[rd] = registers[rs1] / registers[rs2];
            break;
        }
        case OpCode::MOD: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                int64_t av = registers[rs1].unsafeAsInt();
                int64_t bv = registers[rs2].unsafeAsInt();
                if (bv != 0 && !(av == INT64_MIN && bv == -1)) {
                    registers[rd] = Value(av % bv);
                    break;
                }
            }
            registers[rd] = registers[rs1] % registers[rs2];
            break;
        }
        case OpCode::POW: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            // Integer exponentiation: base ** exp
            Value base = registers[rs1];
            Value exp = registers[rs2];
            if (exp.getType() != Value::Type::INTEGER) {
                throw std::runtime_error("Exponent must be an integer for ** operator");
            }
            int64_t n = exp.unsafeAsInt();
            if (n < 0) {
                registers[rd] = Value(static_cast<int64_t>(0));
            } else {
                if (n > kMaxExponent) {
                    throw std::runtime_error("Exponent too large for ** operator (max " + std::to_string(kMaxExponent) +
                                             ")");
                }
                // Exponentiation by squaring: O(log n) instead of O(n)
                if (base.getType() == Value::Type::INTEGER) {
                    int64_t b = base.unsafeAsInt();
                    int64_t result = 1;
                    int64_t e = n;
                    while (e > 0) {
                        if (e & 1) {
                            if (wouldMultiplyOverflow(result, b))
                                throw std::runtime_error("Integer overflow in ** operator");
                            result *= b;
                        }
                        e >>= 1;
                        if (e > 0) {
                            if (wouldMultiplyOverflow(b, b))
                                throw std::runtime_error("Integer overflow in ** operator");
                            b *= b;
                        }
                    }
                    registers[rd] = Value(result);
                } else {
                    Value result(static_cast<int64_t>(1));
                    Value b = base;
                    int64_t e = n;
                    while (e > 0) {
                        if (e & 1)
                            result = result * b;
                        b = b * b;
                        e >>= 1;
                    }
                    registers[rd] = result;
                }
            }
            break;
        }
        case OpCode::NEG: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs = readByte(*curBytecode, ip);
            if (registers[rs].getType() == Value::Type::INTEGER) {
                int64_t val = registers[rs].unsafeAsInt();
                if (val != INT64_MIN) {
                    registers[rd] = Value(-val);
                    break;
                }
            } else if (registers[rs].getType() == Value::Type::FLOAT) {
                registers[rd] = Value(-registers[rs].unsafeAsFloat());
                break;
            }
            registers[rd] = -registers[rs];
            break;
        }
        case OpCode::EQ: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() == registers[rs2].unsafeAsInt()));
            } else {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] == registers[rs2]));
            }
            break;
        }
        case OpCode::NE: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() != registers[rs2].unsafeAsInt()));
            } else {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] != registers[rs2]));
            }
            break;
        }
        case OpCode::LT: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() < registers[rs2].unsafeAsInt()));
            } else {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] < registers[rs2]));
            }
            break;
        }
        case OpCode::LE: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() <= registers[rs2].unsafeAsInt()));
            } else {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] <= registers[rs2]));
            }
            break;
        }
        case OpCode::GT: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() > registers[rs2].unsafeAsInt()));
            } else {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] > registers[rs2]));
            }
            break;
        }
        case OpCode::GE: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() >= registers[rs2].unsafeAsInt()));
            } else {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1] >= registers[rs2]));
            }
            break;
        }
        case OpCode::AND: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() != 0 && registers[rs2].unsafeAsInt() != 0));
            } else {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].isTruthy() && registers[rs2].isTruthy()));
            }
            break;
        }
        case OpCode::OR: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].unsafeAsInt() != 0 || registers[rs2].unsafeAsInt() != 0));
            } else {
                registers[rd] = Value(static_cast<int64_t>(registers[rs1].isTruthy() || registers[rs2].isTruthy()));
            }
            break;
        }
        case OpCode::NOT: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs = readByte(*curBytecode, ip);
            if (registers[rs].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(static_cast<int64_t>(registers[rs].unsafeAsInt() == 0));
            } else {
                registers[rd] = Value(static_cast<int64_t>(!registers[rs].isTruthy()));
            }
            break;
        }
        case OpCode::BIT_AND: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(registers[rs1].unsafeAsInt() & registers[rs2].unsafeAsInt());
            } else {
                registers[rd] = registers[rs1] & registers[rs2];
            }
            break;
        }
        case OpCode::BIT_OR: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(registers[rs1].unsafeAsInt() | registers[rs2].unsafeAsInt());
            } else {
                registers[rd] = registers[rs1] | registers[rs2];
            }
            break;
        }
        case OpCode::BIT_XOR: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(registers[rs1].unsafeAsInt() ^ registers[rs2].unsafeAsInt());
            } else {
                registers[rd] = registers[rs1] ^ registers[rs2];
            }
            break;
        }
        case OpCode::SHL: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                int64_t bv = registers[rs2].unsafeAsInt();
                if (bv >= 0 && bv < kInt64BitWidth) {
                    registers[rd] = Value(registers[rs1].unsafeAsInt() << bv);
                    break;
                }
            }
            registers[rd] = registers[rs1] << registers[rs2];
            break;
        }
        case OpCode::SHR: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs1 = readByte(*curBytecode, ip);
            uint8_t rs2 = readByte(*curBytecode, ip);
            if (registers[rs1].getType() == Value::Type::INTEGER && registers[rs2].getType() == Value::Type::INTEGER) {
                int64_t bv = registers[rs2].unsafeAsInt();
                if (bv >= 0 && bv < kInt64BitWidth) {
                    registers[rd] = Value(registers[rs1].unsafeAsInt() >> bv);
                    break;
                }
            }
            registers[rd] = registers[rs1] >> registers[rs2];
            break;
        }
        case OpCode::BIT_NOT: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs = readByte(*curBytecode, ip);
            if (registers[rs].getType() == Value::Type::INTEGER) {
                registers[rd] = Value(~registers[rs].unsafeAsInt());
            } else {
                registers[rd] = ~registers[rs];
            }
            break;
        }
        case OpCode::LOAD_VAR: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            std::string name = readString(*curBytecode, ip);
            registers[rd] = getGlobal(name);
            break;
        }
        case OpCode::STORE_VAR: {
            uint8_t rs = readByte(*curBytecode, ip);
            std::string name = readString(*curBytecode, ip);
            setGlobal(name, registers[rs]);
            break;
        }
        case OpCode::LOAD_LOCAL: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t index = readByte(*curBytecode, ip);
            if (index >= locals.size()) {
                throw std::runtime_error("Local variable index out of range: " + std::to_string(index) + " at ip " +
                                         std::to_string(ip - 1));
            }
            registers[rd] = locals[index];
            break;
        }
        case OpCode::STORE_LOCAL: {
            uint8_t index = readByte(*curBytecode, ip);
            uint8_t rs = readByte(*curBytecode, ip);
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
            uint16_t offset = readShort(*curBytecode, ip);
            if (offset >= curBytecode->size()) {
                throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
            }
            ip = offset;
            break;
        }
        case OpCode::JUMP_IF_FALSE: {
            uint8_t rs = readByte(*curBytecode, ip);
            uint16_t offset = readShort(*curBytecode, ip);
            bool isFalse;
            if (registers[rs].getType() == Value::Type::INTEGER) {
                isFalse = registers[rs].unsafeAsInt() == 0;
            } else {
                isFalse = !registers[rs].isTruthy();
            }
            if (isFalse) {
                if (offset >= curBytecode->size()) {
                    throw std::runtime_error("Jump offset out of bounds at ip " + std::to_string(ip - 2));
                }
                ip = offset;
            }
            break;
        }
        case OpCode::RETURN: {
            uint8_t rs = readByte(*curBytecode, ip);
            lastReturn = registers[rs];
            // If there's a caller frame, restore it and continue iteratively.
            if (callStack.size() > baseCallDepth) {
                CallFrame& top = callStack.back();
                locals = std::move(top.savedLocals);
                // Restore only the registers that were actually saved (partial restore).
                std::copy(top.savedRegisters.begin(), top.savedRegisters.end(), registers);
                maxRegUsed_ = top.savedMaxReg;
                uint8_t retReg = top.returnReg;
                ip = top.returnIp;
                curBytecode = top.returnBytecode;
                callStack.pop_back();
                registers[retReg] = lastReturn;
                TRACK_REG(retReg);
                break;
            }
            return;
        }
        case OpCode::PRINT: {
            uint8_t rs = readByte(*curBytecode, ip);
            std::cout << registers[rs].toString() << std::endl;
            break;
        }
        case OpCode::DUP:
            break;
        case OpCode::HALT:
            lastReturn = Value();
            return;
        case OpCode::MOV: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            uint8_t rs = readByte(*curBytecode, ip);
            registers[rd] = registers[rs];
            break;
        }
        case OpCode::CALL: {
            uint8_t rd = readByte(*curBytecode, ip);
            TRACK_REG(rd);
            std::string funcName = readString(*curBytecode, ip);
            uint8_t argCount = readByte(*curBytecode, ip);
            uint8_t argRegs[256];
            for (uint8_t i = 0; i < argCount; i++) {
                argRegs[i] = readByte(*curBytecode, ip);
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
            frame.returnBytecode = curBytecode;
            frame.savedLocals = std::move(locals);
            // Save only the registers actually used (high-water mark), not all 256.
            {
                size_t saveCount = static_cast<size_t>(maxRegUsed_) + 1;
                frame.savedRegisters.assign(registers, registers + saveCount);
            }
            frame.returnReg = rd;
            frame.savedMaxReg = maxRegUsed_;
            callStack.push_back(std::move(frame));

            locals.clear();
            locals.resize(argCount);
            for (uint8_t i = 0; i < argCount; i++) {
                locals[i] = callStack.back().savedRegisters[argRegs[i]];
            }

            // Switch to callee's bytecode iteratively (no recursive execute()).
            curBytecode = &func.bytecode;
            ip = 0;
            maxRegUsed_ = 0;
            break;
        }
        default:
            throw std::runtime_error("Unknown opcode " + std::to_string(static_cast<uint8_t>(op)) + " at ip " +
                                     std::to_string(ip - 1));
        }
    }
#endif // USE_COMPUTED_GOTO
#undef TRACK_REG
}

} // namespace omscript
