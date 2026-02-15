#ifndef VM_H
#define VM_H

#include "value.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>

namespace omscript {

// Describes a bytecode function registered with the VM.
struct BytecodeFunction {
    std::string name;
    uint8_t arity;                  // number of parameters
    std::vector<uint8_t> bytecode;  // function body bytecode
};

// A single activation record on the call stack.  Each CALL pushes a frame
// and each RETURN pops one, restoring the caller's local variables and
// instruction pointer.
struct CallFrame {
    const BytecodeFunction* function;  // function being executed
    size_t returnIp;                   // instruction pointer to resume in caller
    const std::vector<uint8_t>* returnBytecode;  // caller's bytecode
    std::vector<Value> savedLocals;    // caller's locals snapshot
};

class VM {
public:
    VM();
    
    void execute(const std::vector<uint8_t>& bytecode);
    void setGlobal(const std::string& name, const Value& value);
    Value getGlobal(const std::string& name);
    Value getLastReturn() const { return lastReturn; }
    
    // Register a bytecode function that can be invoked via the CALL opcode.
    void registerFunction(const BytecodeFunction& func);
    
private:
    std::vector<Value> stack;
    std::unordered_map<std::string, Value> globals;
    std::vector<Value> locals;
    Value lastReturn;
    
    // Registered bytecode functions keyed by name.
    std::unordered_map<std::string, BytecodeFunction> functions;
    
    // Call-frame stack for nested function calls.
    std::vector<CallFrame> callStack;
    
    void push(const Value& value);
    Value pop();
    Value peek(int offset = 0);
    
    void ensureReadable(const std::vector<uint8_t>& code, size_t ip, size_t count);
    uint8_t readByte(const std::vector<uint8_t>& code, size_t& ip);
    uint16_t readShort(const std::vector<uint8_t>& code, size_t& ip);
    int64_t readInt(const std::vector<uint8_t>& code, size_t& ip);
    double readFloat(const std::vector<uint8_t>& code, size_t& ip);
    std::string readString(const std::vector<uint8_t>& code, size_t& ip);
};

} // namespace omscript

#endif // VM_H
