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
///
/// **Type-specialized recompilation**: After a function has been JIT-compiled
/// with integer-only specialization, the profiler continues to observe
/// argument types.  If the function is later called with consistent type
/// patterns (e.g. always int,int), it may be recompiled with a tighter
/// specialization.  The `recompile()` method supports this path — it is
/// called when profiling data suggests a better specialization is available.
class BytecodeJIT {
public:
    /// Native function signature: takes (args_array, num_args), returns int64_t.
    using JITFnPtr = int64_t (*)(int64_t*, int);

    /// Number of interpreted calls before a function is JIT-compiled.
    static constexpr size_t kJITThreshold = 5;

    /// After this many *additional* calls post-JIT, consider recompilation
    /// with updated type-specialization data.
    static constexpr size_t kRecompileThreshold = 50;

    /// Functions with fewer than this many opcodes are too small to benefit
    /// from JIT compilation — the compilation overhead exceeds the savings.
    static constexpr size_t kMinBytecodeSize = 4;

    BytecodeJIT();
    ~BytecodeJIT();

    /// Try to JIT-compile @p func.  Returns true on success.
    bool compile(const BytecodeFunction& func);

    /// Recompile a previously-compiled function with updated type
    /// specialization.  This replaces the old native code pointer.
    /// Returns true on success.
    bool recompile(const BytecodeFunction& func);

    /// Return true if @p name has been successfully JIT-compiled.
    bool isCompiled(const std::string& name) const;

    /// Get the native function pointer for a JIT-compiled function.
    JITFnPtr getCompiled(const std::string& name) const;

    /// Increment the call counter for @p name.
    /// Returns true exactly once — when the counter first reaches kJITThreshold.
    bool recordCall(const std::string& name);

    /// Increment the post-JIT call counter for @p name.
    /// Returns true exactly once — when the counter first reaches
    /// kRecompileThreshold, indicating a recompilation opportunity.
    bool recordPostJITCall(const std::string& name);

    /// Return the total number of calls recorded for @p name.
    size_t getCallCount(const std::string& name) const;

private:
    /// Keep LLVM execution engines alive so compiled code remains valid.
    struct JITModule {
        std::unique_ptr<llvm::LLVMContext> context;
        // ExecutionEngine owns the Module and compiled code.
        std::unique_ptr<llvm::ExecutionEngine> engine;
    };

    std::unordered_map<std::string, JITFnPtr> compiled_;
    std::unordered_map<std::string, size_t> callCounts_;
    std::unordered_map<std::string, size_t> postJITCallCounts_;
    std::unordered_set<std::string> failedCompilations_;
    std::unordered_set<std::string> recompiled_;
    std::vector<JITModule> modules_;

    bool initialized_ = false;
    void ensureInitialized();
};

} // namespace omscript

#endif // JIT_H
