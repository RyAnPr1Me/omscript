#ifndef BYTECODE_H
#define BYTECODE_H

#include <vector>
#include <cstdint>
#include <string>

namespace omscript {

enum class OpCode : uint8_t {
    // Stack operations
    PUSH_INT,
    PUSH_FLOAT,
    PUSH_STRING,
    POP,
    
    // Arithmetic
    ADD,
    SUB,
    MUL,
    DIV,
    MOD,
    NEG,
    
    // Comparison
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,
    
    // Logical
    AND,
    OR,
    NOT,
    
    // Bitwise
    BIT_AND,
    BIT_OR,
    BIT_XOR,
    BIT_NOT,
    SHL,
    SHR,
    
    // Variables
    LOAD_VAR,
    STORE_VAR,
    LOAD_LOCAL,   // Load a local variable by index
    STORE_LOCAL,  // Store a local variable by index
    
    // Control flow
    JUMP,
    JUMP_IF_FALSE,
    CALL,
    RETURN,
    
    // Special
    PRINT,
    DUP,
    HALT
};

class BytecodeEmitter {
public:
    void emit(OpCode op);
    void emitByte(uint8_t byte);
    // Bytecode is encoded in little-endian order for cross-platform stability.
    void emitInt(int64_t value);
    void emitFloat(double value);
    void emitString(const std::string& str);
    void emitShort(uint16_t value);
    
    size_t currentOffset() const;
    void patchJump(size_t offset, uint16_t jump);
    
    const std::vector<uint8_t>& getCode() const { return code; }
    
private:
    std::vector<uint8_t> code;
};

} // namespace omscript

#endif // BYTECODE_H
