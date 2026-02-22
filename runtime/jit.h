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

/// Observed type specialization for a function's arguments.
enum class JITSpecialization {
    Unknown,   // Not enough data yet
    IntOnly,   // All observed calls used integer arguments
    FloatOnly, // All observed calls used float arguments
    Mixed      // Arguments include a mix of int and float (not specializable)
};

/// Runtime type profile for a function.  The VM records argument types
/// at each call site and the JIT uses this profile to select the best
/// specialization when compiling or recompiling.
struct TypeProfile {
    size_t intCalls = 0;   // Calls where all args were INTEGER
    size_t floatCalls = 0; // Calls where all args were FLOAT
    size_t mixedCalls = 0; // Calls with mixed or other types

    /// Determine the best specialization from observed data.
    JITSpecialization bestSpecialization() const {
        if (intCalls == 0 && floatCalls == 0 && mixedCalls == 0)
            return JITSpecialization::Unknown;
        if (mixedCalls > 0)
            return JITSpecialization::Mixed;
        if (floatCalls > 0 && intCalls == 0)
            return JITSpecialization::FloatOnly;
        if (intCalls > 0 && floatCalls == 0)
            return JITSpecialization::IntOnly;
        return JITSpecialization::Mixed;
    }
};

/// Lightweight JIT compiler for bytecode functions.
///
/// Translates bytecode to LLVM IR and compiles to native machine code
/// via LLVM MCJIT.  Supports two specializations:
///
///  - **Integer**: all arithmetic uses i64 (original path)
///  - **Float**: all arithmetic uses double (enabled by type profiling)
///
/// Functions that use strings, globals, CALL, or PRINT remain interpreted.
///
/// The VM feeds call-count and type-profile data; once the JIT threshold
/// is reached, compile() selects a specialization based on the profile.
/// After additional calls, recompile() can switch to a better specialization
/// if the profile has changed (e.g., initially int, later mostly float).
class BytecodeJIT {
  public:
    /// Integer-specialized native function signature.
    using JITFnPtr = int64_t (*)(int64_t*, int);

    /// Float-specialized native function signature.
    using JITFloatFnPtr = double (*)(double*, int);

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

    /// Try to JIT-compile @p func with the given specialization.
    /// If @p spec is Unknown, defaults to IntOnly.  Returns true on success.
    bool compile(const BytecodeFunction& func, JITSpecialization spec = JITSpecialization::IntOnly);

    /// Recompile a previously-compiled function with the specialization
    /// determined by its current type profile.  Returns true on success.
    bool recompile(const BytecodeFunction& func);

    /// Return true if @p name has been successfully JIT-compiled.
    bool isCompiled(const std::string& name) const;

    /// Get the integer-specialized native function pointer.
    JITFnPtr getCompiled(const std::string& name) const;

    /// Get the float-specialized native function pointer (nullptr if N/A).
    JITFloatFnPtr getCompiledFloat(const std::string& name) const;

    /// Return the specialization used for a compiled function.
    JITSpecialization getSpecialization(const std::string& name) const;

    /// Increment the call counter for @p name.
    /// Returns true exactly once — when the counter first reaches kJITThreshold.
    bool recordCall(const std::string& name);

    /// Increment the post-JIT call counter for @p name.
    /// Returns true exactly once — when the counter first reaches
    /// kRecompileThreshold, indicating a recompilation opportunity.
    bool recordPostJITCall(const std::string& name);

    /// Return the total number of calls recorded for @p name.
    size_t getCallCount(const std::string& name) const;

    /// Record argument types for a call to @p name.
    void recordTypes(const std::string& name, bool allInt, bool allFloat);

    /// Get the type profile for @p name.
    const TypeProfile& getTypeProfile(const std::string& name) const;

  private:
    /// Keep LLVM execution engines alive so compiled code remains valid.
    struct JITModule {
        std::unique_ptr<llvm::LLVMContext> context;
        // ExecutionEngine owns the Module and compiled code.
        std::unique_ptr<llvm::ExecutionEngine> engine;
    };

    std::unordered_map<std::string, JITFnPtr> compiled_;
    std::unordered_map<std::string, JITFloatFnPtr> compiledFloat_;
    std::unordered_map<std::string, JITSpecialization> specializations_;
    std::unordered_map<std::string, TypeProfile> typeProfiles_;
    std::unordered_map<std::string, size_t> callCounts_;
    std::unordered_map<std::string, size_t> postJITCallCounts_;
    std::unordered_set<std::string> failedCompilations_;
    std::unordered_set<std::string> recompiled_;
    std::vector<JITModule> modules_;

    static TypeProfile emptyProfile_;

    bool initialized_ = false;
    void ensureInitialized();

    /// Internal: compile with a specific specialization.
    bool compileInt(const BytecodeFunction& func);
    bool compileFloat(const BytecodeFunction& func);
};

} // namespace omscript

#endif // JIT_H
