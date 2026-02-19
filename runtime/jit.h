#ifndef JIT_H
#define JIT_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declarations — avoid pulling LLVM headers into every translation unit.
namespace llvm {
class ExecutionEngine;
class LLVMContext;
} // namespace llvm

namespace omscript {

// Forward-declare BytecodeFunction (defined in vm.h).
struct BytecodeFunction;

/// Lightweight JIT compiler for bytecode functions.
///
/// Translates integer-only bytecode to LLVM IR, then compiles it to native
/// machine code via LLVM MCJIT.  Functions that use floats, strings, globals,
/// or CALL/PRINT are not JIT-eligible and will continue to be interpreted.
///
/// The VM feeds call-count data into recordCall(); once the threshold is
/// reached the VM invokes compile().  Subsequent calls to the function go
/// through the native pointer returned by getCompiled().
class BytecodeJIT {
public:
    /// Native function signature: takes (args_array, num_args), returns int64_t.
    using JITFnPtr = int64_t (*)(int64_t*, int);

    /// Number of interpreted calls before a function is JIT-compiled.
    static constexpr size_t kJITThreshold = 10;

    BytecodeJIT();
    ~BytecodeJIT();

    /// Try to JIT-compile @p func.  Returns true on success.
    bool compile(const BytecodeFunction& func);

    /// Return true if @p name has been successfully JIT-compiled.
    bool isCompiled(const std::string& name) const;

    /// Get the native function pointer for a JIT-compiled function.
    JITFnPtr getCompiled(const std::string& name) const;

    /// Increment the call counter for @p name.
    /// Returns true exactly once — when the counter first reaches kJITThreshold.
    bool recordCall(const std::string& name);

private:
    /// Keep LLVM execution engines alive so compiled code remains valid.
    struct JITModule {
        std::unique_ptr<llvm::LLVMContext> context;
        // ExecutionEngine owns the Module and compiled code.
        std::unique_ptr<llvm::ExecutionEngine> engine;
    };

    std::unordered_map<std::string, JITFnPtr> compiled_;
    std::unordered_map<std::string, size_t> callCounts_;
    std::unordered_set<std::string> failedCompilations_;
    std::vector<JITModule> modules_;

    bool initialized_ = false;
    void ensureInitialized();
};

} // namespace omscript

#endif // JIT_H
