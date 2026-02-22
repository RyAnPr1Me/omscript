#ifndef VM_H
#define VM_H

#include "value.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace omscript {

class BytecodeJIT; // forward declaration (defined in jit.h)

// Describes a bytecode function registered with the VM.
struct BytecodeFunction {
    std::string name;
    uint8_t arity;                 // number of parameters
    std::vector<uint8_t> bytecode; // function body bytecode
};

// A single activation record on the call stack.  Each CALL pushes a frame
// and each RETURN pops one, restoring the caller's local variables and
// instruction pointer.
struct CallFrame {
    const BytecodeFunction* function;           // function being executed
    size_t returnIp;                            // instruction pointer to resume in caller
    const std::vector<uint8_t>* returnBytecode; // caller's bytecode
    std::vector<Value> savedLocals;             // caller's locals snapshot
    std::vector<Value> savedRegisters;          // caller's register file snapshot
    uint8_t returnReg;                          // destination register for return value
};

class VM {
  public:
    // Maximum number of values allowed on the operand stack (kept for compat).
    static constexpr size_t kMaxStackSize = 65536;
    // Maximum call depth to prevent runaway recursion.
    static constexpr size_t kMaxCallDepth = 1024;
    // Maximum number of local variables per function scope.
    static constexpr size_t kMaxLocals = 256;
    // Register file size.
    static constexpr size_t kMaxRegisters = 256;

    VM();
    ~VM(); // defined in vm.cpp for proper BytecodeJIT cleanup

    void execute(const std::vector<uint8_t>& bytecode);
    void setGlobal(const std::string& name, const Value& value);
    Value getGlobal(const std::string& name);
    Value getLastReturn() const {
        return lastReturn;
    }

    // Register a bytecode function that can be invoked via the CALL opcode.
    void registerFunction(const BytecodeFunction& func);

    /// Return true if the named function has been JIT-compiled.
    bool isJITCompiled(const std::string& name) const;

  private:
    Value registers[kMaxRegisters];
    std::unordered_map<std::string, Value> globals;
    std::vector<Value> locals;
    Value lastReturn;

    // Registered bytecode functions keyed by name.
    std::unordered_map<std::string, BytecodeFunction> functions;

    // Call-frame stack for nested function calls.
    std::vector<CallFrame> callStack;

    // JIT compiler for hot bytecode functions.
    std::unique_ptr<BytecodeJIT> jit_;

    // Cached JIT native pointers — avoids repeated hash lookups into
    // BytecodeJIT::compiled_ on every call.
    using JITFnPtr = int64_t (*)(int64_t*, int);
    using JITFloatFnPtr = double (*)(double*, int);
    std::unordered_map<std::string, JITFnPtr> jitCache_;
    std::unordered_map<std::string, JITFloatFnPtr> jitFloatCache_;

    // Maximum number of JIT call arguments that can be passed via
    // stack-allocated buffer (avoids heap allocation for common cases).
    static constexpr size_t kMaxStackArgs = 8;

    // Invoke a JIT-compiled function, reading args from VM registers
    // and putting the result in registers[rd].  Returns true if successful.
    bool invokeJIT(JITFnPtr fn, uint8_t argCount, const uint8_t* argRegs, uint8_t rd);
    bool invokeJITFloat(JITFloatFnPtr fn, uint8_t argCount, const uint8_t* argRegs, uint8_t rd);

    // Classify argument types from registers for JIT type profiling.
    void classifyArgTypes(uint8_t argCount, const uint8_t* argRegs, bool& allInt, bool& allFloat) const;


    // Hot-path bytecode readers — force-inlined into the dispatch loop.
    inline void ensureReadable(const std::vector<uint8_t>& code, size_t ip, size_t count)
        __attribute__((always_inline));
    inline uint8_t readByte(const std::vector<uint8_t>& code, size_t& ip) __attribute__((always_inline));
    inline uint16_t readShort(const std::vector<uint8_t>& code, size_t& ip) __attribute__((always_inline));
    inline int64_t readInt(const std::vector<uint8_t>& code, size_t& ip) __attribute__((always_inline));
    inline double readFloat(const std::vector<uint8_t>& code, size_t& ip) __attribute__((always_inline));
    std::string readString(const std::vector<uint8_t>& code, size_t& ip);
};

} // namespace omscript

#endif // VM_H
