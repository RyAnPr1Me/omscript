#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "bytecode.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declaration avoids including the full TargetMachine header,
// reducing compilation dependencies for translation units that only
// need the CodeGenerator interface (e.g. compiler.cpp, main.cpp).
namespace llvm {
class TargetMachine;
} // namespace llvm

namespace omscript {

// Returns true if the given name is a stdlib built-in function.
// Stdlib functions are always compiled to native machine code.
bool isStdlibFunction(const std::string& name);

enum class OptimizationLevel {
    O0, // No optimization
    O1, // Basic optimization
    O2, // Moderate optimization
    O3  // Aggressive optimization
};

/// Execution tier assigned to each function during compilation.
///
///  - **AOT**: The function has full type annotations (or is OPTMAX / main /
///    stdlib) and can be compiled to native machine code ahead of time via
///    LLVM IR.
///  - **Interpreted**: The function lacks type annotations or uses dynamic
///    features.  It is compiled to bytecode and run by the VM interpreter.
///  - **JIT**: An interpreted function that has been identified as hot by
///    the VM profiler and was successfully JIT-compiled to native code.
///    This tier also covers precompiled bytecode that is later recompiled
///    with type-specialized native code when profiling data is available.
enum class ExecutionTier {
    AOT,         // Compiled to native code via LLVM IR
    Interpreted, // Compiled to bytecode, run by the VM
    JIT          // Hot bytecode function JIT-compiled to native code
};

/// Return a human-readable label for an ExecutionTier value.
inline const char* executionTierName(ExecutionTier tier) {
    switch (tier) {
    case ExecutionTier::AOT:
        return "AOT";
    case ExecutionTier::Interpreted:
        return "Interpreted";
    case ExecutionTier::JIT:
        return "JIT";
    }
    return "Unknown";
}

class CodeGenerator {
  public:
    CodeGenerator(OptimizationLevel optLevel = OptimizationLevel::O2);
    ~CodeGenerator();

    void generate(Program* program);
    void writeObjectFile(const std::string& filename);
    llvm::Module* getModule() {
        return module.get();
    }
    void setOptimizationLevel(OptimizationLevel level) {
        optimizationLevel = level;
    }

    /// Set the target CPU architecture for instruction selection.
    /// Use "native" or "" for host auto-detection (default).
    void setMarch(const std::string& cpu) {
        marchCpu_ = cpu;
    }

    /// Set the CPU model for scheduling/micro-architecture tuning.
    /// Defaults to the same value as -march when empty.
    void setMtune(const std::string& cpu) {
        mtuneCpu_ = cpu;
    }

    /// Enable or disable position-independent code generation (default: true).
    void setPIC(bool enable) {
        usePIC_ = enable;
    }

    /// Enable or disable unsafe floating-point optimizations (default: false).
    void setFastMath(bool enable) {
        useFastMath_ = enable;
    }

    /// Enable or disable OPTMAX block optimization (default: true).
    void setOptMax(bool enable) {
        enableOptMax_ = enable;
    }

  private:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::Module> module;

    std::unordered_map<std::string, llvm::Value*> namedValues;
    std::vector<std::unordered_map<std::string, llvm::Value*>> scopeStack;

    struct LoopContext {
        llvm::BasicBlock* breakTarget;
        llvm::BasicBlock* continueTarget;
    };
    std::vector<LoopContext> loopStack;
    bool inOptMaxFunction;
    bool hasOptMaxFunctions;
    std::unordered_set<std::string> optMaxFunctions;

    struct ConstBinding {
        bool wasPreviouslyDefined;
        bool previousIsConst;
    };
    std::unordered_map<std::string, bool> constValues;
    std::vector<std::unordered_map<std::string, ConstBinding>> constScopeStack;
    std::unordered_map<std::string, llvm::Function*> functions;

    // Enum constant values (name → integer value), populated from enum declarations.
    std::unordered_map<std::string, long long> enumConstants_;

    // Bytecode emitter for dynamic/interpreted code (user-defined functions only;
    // stdlib built-ins are always compiled to native machine code via LLVM IR).
    BytecodeEmitter bytecodeEmitter;
    bool useDynamicCompilation;
    OptimizationLevel optimizationLevel;

    // Per-function execution tier decided during code generation.
    std::unordered_map<std::string, ExecutionTier> functionTiers;

    // String type tracking across function boundaries.
    // stringVars_: names of variables/parameters that hold string values in the
    //   current function scope (pointer-typed values stored as i64).
    // stringReturningFunctions_: functions known to return a string value.
    // funcParamStringTypes_: maps function name to the set of parameter indices
    //   that are expected to receive string arguments.
    std::unordered_set<std::string> stringVars_;
    std::unordered_set<std::string> stringReturningFunctions_;
    std::unordered_map<std::string, std::unordered_set<size_t>> funcParamStringTypes_;

    /// Compiled bytecode functions for Interpreted-tier functions.
    /// Populated by generateHybrid() when the hybrid execution model is active.
    struct CompiledBytecodeFunc {
        std::string name;
        uint8_t arity;
        std::vector<uint8_t> bytecode;
    };
    std::vector<CompiledBytecodeFunc> bytecodeFunctions_;

    /// Local variable name → index mapping for per-function bytecode emission.
    /// Parameters are bound to indices 0..arity-1; additional locals are
    /// allocated on demand during bytecode statement emission.
    std::unordered_map<std::string, uint8_t> bytecodeLocals_;
    uint8_t bytecodeNextLocal_ = 0;

    /// Register allocator state for register-based bytecode.
    uint8_t bytecodeNextReg_ = 0;
    uint8_t bytecodeLocalBase_ = 0;

    uint8_t allocReg() {
        if (bytecodeNextReg_ == 255)
            throw std::runtime_error("Register file overflow (max 255 registers)");
        return bytecodeNextReg_++;
    }

    void resetTempRegs() {
        bytecodeNextReg_ = bytecodeLocalBase_;
    }

    /// Return true when bytecode is being emitted for a function body
    /// (as opposed to top-level / main code).
    bool isInBytecodeFunctionContext() const {
        return !bytecodeLocals_.empty() || bytecodeNextLocal_ > 0;
    }

    /// Classify a function into its execution tier based on type annotations,
    /// OPTMAX status, and whether it is a special function (main/stdlib).
    ExecutionTier classifyFunction(const FunctionDecl* func) const;

    /// Emit bytecode for a single function body (used by hybrid compilation).
    void emitBytecodeForFunction(FunctionDecl* func);

    // Code generation methods
    llvm::Function* generateFunction(FunctionDecl* func);
    void generateStatement(Statement* stmt);
    llvm::Value* generateExpression(Expression* expr);

    // Expression generators
    llvm::Value* generateLiteral(LiteralExpr* expr);
    llvm::Value* generateIdentifier(IdentifierExpr* expr);
    llvm::Value* generateBinary(BinaryExpr* expr);
    llvm::Value* generateUnary(UnaryExpr* expr);
    llvm::Value* generateCall(CallExpr* expr);
    llvm::Value* generateAssign(AssignExpr* expr);
    llvm::Value* generatePostfix(PostfixExpr* expr);
    llvm::Value* generatePrefix(PrefixExpr* expr);
    llvm::Value* generateTernary(TernaryExpr* expr);
    llvm::Value* generateArray(ArrayExpr* expr);
    llvm::Value* generateIndex(IndexExpr* expr);
    llvm::Value* generateIndexAssign(IndexAssignExpr* expr);

    // Statement generators
    void generateVarDecl(VarDecl* stmt);
    void generateReturn(ReturnStmt* stmt);
    void generateIf(IfStmt* stmt);
    void generateWhile(WhileStmt* stmt);
    void generateDoWhile(DoWhileStmt* stmt);
    void generateFor(ForStmt* stmt);
    void generateForEach(ForEachStmt* stmt);
    void generateBlock(BlockStmt* stmt);
    void generateExprStmt(ExprStmt* stmt);
    void generateSwitch(SwitchStmt* stmt);
    void generateTryCatch(TryCatchStmt* stmt);
    void generateThrow(ThrowStmt* stmt);

    // Helper methods
    llvm::Type* getDefaultType();
    llvm::Type* getFloatType();
    llvm::Value* toBool(llvm::Value* v);
    llvm::Value* toDefaultType(llvm::Value* v);
    llvm::Value* ensureFloat(llvm::Value* v);
    void setupPrintfDeclaration();
    llvm::Function* getPrintfFunction();
    void beginScope();
    void endScope();
    void bindVariable(const std::string& name, llvm::Value* value, bool isConst = false);
    void checkConstModification(const std::string& name, const std::string& action);
    void validateScopeStacksMatch(const char* location);
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* function, const std::string& name,
                                             llvm::Type* type = nullptr);
    [[noreturn]] void codegenError(const std::string& message, const ASTNode* node);

    // String type inference helpers.
    // isStringExpr: returns true if the given AST expression is known to
    //   produce a string value at the current codegen point (uses namedValues
    //   and stringVars_ for identifier lookups).
    bool isStringExpr(Expression* expr) const;
    // preAnalyzeStringTypes: iterative pre-pass over the full program AST to
    //   populate stringReturningFunctions_ and funcParamStringTypes_ before
    //   any function body is generated.
    void preAnalyzeStringTypes(Program* program);
    // isPreAnalysisStringExpr: lightweight AST-only string check used by the
    //   pre-analysis (no access to namedValues; uses stringReturningFunctions_
    //   and paramStringIndices to track string parameters).
    bool isPreAnalysisStringExpr(Expression* expr, const std::unordered_set<size_t>& paramStringIndices,
                                 const FunctionDecl* func) const;
    // scanStmtForStringReturns: returns true if any return statement in the
    //   given statement subtree returns a string expression.
    bool scanStmtForStringReturns(Statement* stmt, const std::unordered_set<size_t>& paramStringIndices,
                                  const FunctionDecl* func) const;
    // scanStmtForStringCalls: walks a statement subtree and records which
    //   function parameters receive string arguments at call sites.
    void scanStmtForStringCalls(Statement* stmt);

    // Target CPU configuration for LLVM code generation.
    std::string marchCpu_;     // -march: CPU arch for instruction selection ("" = native)
    std::string mtuneCpu_;     // -mtune: CPU for scheduling tuning ("" = same as march)
    bool usePIC_ = true;       // -fpic / -fno-pic
    bool useFastMath_ = false; // -ffast-math / -fno-fast-math
    bool enableOptMax_ = true; // -foptmax / -fno-optmax

    /// Counter for generating unique bytecode switch temp-variable names.
    /// Member variable (not function-local static) for thread-safety and
    /// deterministic output across independent CodeGenerator instances.
    int bytecodeSwitchCounter_ = 0;

    /// Resolve the effective CPU name and feature string for LLVM target machine
    /// construction based on the current -march / -mtune settings.
    void resolveTargetCPU(std::string& cpu, std::string& features) const;

    /// Create a configured TargetMachine for the current target triple and
    /// CPU settings.  Shared by runOptimizationPasses() and writeObjectFile()
    /// to eliminate duplicated setup code.
    std::unique_ptr<llvm::TargetMachine> createTargetMachine() const;

    // Lazy-declaration helpers for C library functions.  Each method returns
    // the existing declaration if one has already been added to the module,
    // or creates a new external declaration on first use.  This removes
    // duplicated getFunction()/Create() blocks that were scattered across
    // multiple built-in handlers.
    llvm::Function* getOrDeclareStrlen();
    llvm::Function* getOrDeclareMalloc();
    llvm::Function* getOrDeclareStrcpy();
    llvm::Function* getOrDeclareStrcat();
    llvm::Function* getOrDeclareStrcmp();
    llvm::Function* getOrDeclarePutchar();
    llvm::Function* getOrDeclareScanf();
    llvm::Function* getOrDeclareExit();
    llvm::Function* getOrDeclareAbort();
    llvm::Function* getOrDeclareSnprintf();
    llvm::Function* getOrDeclareMemchr();
    llvm::Function* getOrDeclareFree();
    llvm::Function* getOrDeclareStrstr();
    llvm::Function* getOrDeclareMemcpy();
    llvm::Function* getOrDeclareMemmove();
    llvm::Function* getOrDeclareToupper();
    llvm::Function* getOrDeclareTolower();
    llvm::Function* getOrDeclareIsspace();
    llvm::Function* getOrDeclareStrtoll();
    llvm::Function* getOrDeclareStrtod();
    llvm::Function* getOrDeclareFloor();
    llvm::Function* getOrDeclareCeil();
    llvm::Function* getOrDeclareRound();
    llvm::Function* getOrDeclareQsort();
    llvm::Function* getOrDeclareRand();
    llvm::Function* getOrDeclareSrand();
    llvm::Function* getOrDeclareTimeFunc();
    llvm::Function* getOrDeclareUsleep();
    llvm::Function* getOrDeclareStrchr();
    llvm::Function* getOrDeclareStrndup();
    llvm::Function* getOrDeclareRealloc();
    llvm::Function* getOrDeclareAtoi();
    llvm::Function* getOrDeclareAtof();
    llvm::Function* getOrDeclareFwrite();
    llvm::Function* getOrDeclareFflush();

    /// Shared implementation for prefix and postfix increment/decrement.
    /// Returns the *old* value for postfix (isPostfix=true) and the *new*
    /// value for prefix (isPostfix=false).
    llvm::Value* generateIncDec(Expression* operandExpr, const std::string& op, bool isPostfix,
                                const ASTNode* errorNode);

    // Optimization methods
    void runOptimizationPasses();
    void optimizeOptMaxFunctions();

    uint8_t emitBytecodeExpression(Expression* expr);
    void emitBytecodeStatement(Statement* stmt);
    void emitBytecodeBlock(BlockStmt* stmt);
    uint8_t emitBytecodeLoad(const std::string& name);
    void emitBytecodeStore(const std::string& name, uint8_t rs);

  public:
    // Per-function optimization for targeted optimization of individual functions
    void optimizeFunction(llvm::Function* func);

    // Bytecode generation (alternative backend)
    void generateBytecode(Program* program);

    /// Hybrid code generation: generates LLVM IR for AOT-tier functions and
    /// bytecode for Interpreted-tier functions in a single pass.  AOT-tier
    /// functions that call Interpreted-tier functions get IR stubs that
    /// invoke the VM at runtime, enabling seamless cross-tier calls.
    void generateHybrid(Program* program);

    /// Return true if hybrid compilation produced any bytecode functions.
    bool hasHybridBytecodeFunctions() const {
        return !bytecodeFunctions_.empty();
    }

    /// Return the list of bytecode functions produced by hybrid compilation.
    const std::vector<CompiledBytecodeFunc>& getBytecodeFunctions() const {
        return bytecodeFunctions_;
    }

    /// Return the execution tier assigned to a function, or AOT if not found.
    ExecutionTier getFunctionTier(const std::string& name) const {
        auto it = functionTiers.find(name);
        return it != functionTiers.end() ? it->second : ExecutionTier::AOT;
    }

    /// Return a read-only view of all function tier assignments.
    const std::unordered_map<std::string, ExecutionTier>& getFunctionTiers() const {
        return functionTiers;
    }

    // Accessors for bytecode output
    const BytecodeEmitter& getBytecodeEmitter() const {
        return bytecodeEmitter;
    }
    bool isDynamicCompilation() const {
        return useDynamicCompilation;
    }
    void setDynamicCompilation(bool enable) {
        useDynamicCompilation = enable;
    }
};

} // namespace omscript

#endif // CODEGEN_H
