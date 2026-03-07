#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "diagnostic.h"
#include <llvm/IR/DIBuilder.h>
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
/// All user-defined functions compile to native LLVM IR and are executed via
/// the adaptive two-tier AOT pipeline:
///
///  - **AOT (Tier 1)**: Every function is compiled to native machine code
///    ahead-of-time by LLVM MCJIT at O2.  The adaptive runtime injects
///    call-counting dispatch prologs so hot functions can be promoted.
///
///  - **Tier 2**: When a function's call count reaches kRecompileThreshold
///    the runtime re-optimises it at O3 with a PGO entry-count annotation
///    and hot-patches the function pointer so subsequent calls bypass the
///    dispatch prolog entirely.
///
/// There is no bytecode or interpreter tier — every function produces
/// native machine code from the first call.
enum class ExecutionTier {
    AOT,         // Compiled to native code via LLVM IR (Tier 1); hot functions promoted to O3+PGO (Tier 2)
    Interpreted, // Executed via tree-walking interpreter (no LLVM compilation)
    JIT          // Just-in-time compiled at runtime
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
    /// Write the module as LLVM bitcode for full link-time optimization (FLTO).
    /// The linker (gcc/clang with -flto) reads bitcode and performs whole-program
    /// optimization across translation units at link time.
    void writeBitcodeFile(const std::string& filename);
    [[nodiscard]] llvm::Module* getModule() noexcept {
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

    /// Enable or disable explicit loop vectorization hints (default: true at O2+).
    void setVectorize(bool enable) {
        enableVectorize_ = enable;
    }

    /// Enable or disable loop unrolling hints (default: true at O2+).
    void setUnrollLoops(bool enable) {
        enableUnrollLoops_ = enable;
    }

    /// Enable or disable polyhedral-style loop optimizations (default: true at O3).
    void setLoopOptimize(bool enable) {
        enableLoopOptimize_ = enable;
    }

    /// Enable PGO instrumentation generation mode.
    /// When set, the AOT-compiled binary will write a raw profile (.profraw)
    /// to @p profilePath at program exit, capturing branch and call counts.
    void setPGOGen(const std::string& profilePath) {
        pgoGenPath_ = profilePath;
    }

    /// Enable PGO profile-guided optimization use mode.
    /// When set, the optimizer reads the .profdata file at @p profilePath
    /// and uses its branch/call counts to improve inlining, branch layout,
    /// and hot-path specialization decisions.
    void setPGOUse(const std::string& profilePath) {
        pgoUsePath_ = profilePath;
    }

    /// Check whether dynamic (JIT) compilation mode is enabled.
    [[nodiscard]] bool isDynamicCompilation() const noexcept {
        return dynamicCompilation_;
    }

    /// Enable or disable dynamic (JIT) compilation mode.
    void setDynamicCompilation(bool enable) {
        dynamicCompilation_ = enable;
    }

    /// Enable LTO pre-link optimization pipeline.
    /// When true, runOptimizationPasses() uses buildLTOPreLinkDefaultPipeline()
    /// instead of buildPerModuleDefaultPipeline(), deferring heavy IPO to the
    /// linker so that the bitcode is not double-optimized.
    void setLTO(bool enable) {
        lto_ = enable;
    }

    /// Enable or disable DWARF debug info generation.
    /// When true, the CodeGenerator emits debug metadata (compile unit,
    /// subprograms) so that compiled binaries can be debugged with GDB/LLDB.
    void setDebugMode(bool enable) {
        debugMode_ = enable;
    }

    /// Set the source filename for debug info metadata.
    void setSourceFilename(const std::string& filename) {
        sourceFilename_ = filename;
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

    // Stack of innermost catch-entry basic blocks, pushed/popped by
    // generateTryCatch(). generateThrow() branches directly to the top of this
    // stack when a throw occurs inside a try block, ensuring that control flow
    // reaches the catch handler immediately (rather than relying on a post-loop
    // flag check that could allow dangerous code to execute after the throw).
    std::vector<llvm::BasicBlock*> tryCatchStack_;
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

    // Store AST function declarations for default parameter lookup at call sites.
    std::unordered_map<std::string, const FunctionDecl*> functionDecls_;

    // Enum constant values (name → integer value), populated from enum declarations.
    std::unordered_map<std::string, long long> enumConstants_;

    // Struct type definitions: struct name → ordered list of field names.
    std::unordered_map<std::string, std::vector<std::string>> structDefs_;
    // Variables known to hold struct values, maps var name → struct type name.
    std::unordered_map<std::string, std::string> structVars_;

    OptimizationLevel optimizationLevel;

    // Per-function execution tier decided during code generation.
    std::unordered_map<std::string, ExecutionTier> functionTiers;

    // String type tracking across function boundaries.
    // stringVars_: names of variables/parameters that hold string values in the
    //   current function scope (pointer-typed values stored as i64).
    // stringReturningFunctions_: functions known to return a string value.
    // funcParamStringTypes_: maps function name to the set of parameter indices
    //   that are expected to receive string arguments.
    // stringArrayVars_: names of variables that hold arrays whose elements are
    //   string pointers (e.g. declared with ["a","b"] or assigned from str_split).
    //   Used by isStringExpr(IndexExpr) and generateForEach to propagate string
    //   type information through array element accesses.
    std::unordered_set<std::string> stringVars_;
    std::unordered_set<std::string> stringReturningFunctions_;
    std::unordered_map<std::string, std::unordered_set<size_t>> funcParamStringTypes_;
    std::unordered_set<std::string> stringArrayVars_;

    /// Classify a function into its execution tier based on type annotations,
    /// OPTMAX status, and whether it is a special function (main/stdlib).
    ExecutionTier classifyFunction(const FunctionDecl* func) const;

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
    llvm::Value* generateStructLiteral(StructLiteralExpr* expr);
    llvm::Value* generateFieldAccess(FieldAccessExpr* expr);
    llvm::Value* generateFieldAssign(FieldAssignExpr* expr);

    // Struct type resolution helpers.
    std::string resolveStructType(Expression* objExpr) const;
    size_t resolveFieldIndex(const std::string& structType, const std::string& fieldName,
                             const ASTNode* errorNode);

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

    /// RAII guard that calls beginScope() on construction and endScope()
    /// on destruction, ensuring scope stacks are always balanced even
    /// when exceptions interrupt code generation.
    class ScopeGuard {
      public:
        explicit ScopeGuard(CodeGenerator& cg) : cg_(cg) {
            cg_.beginScope();
        }
        ~ScopeGuard() noexcept {
            cg_.endScope();
        }
        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;

      private:
        CodeGenerator& cg_;
    };

    // String type inference helpers.
    // isStringExpr: returns true if the given AST expression is known to
    //   produce a string value at the current codegen point (uses namedValues
    //   and stringVars_ for identifier lookups).
    bool isStringExpr(Expression* expr) const;
    // isStringArrayExpr: returns true if the expression is known to be an array
    //   whose elements are string pointers (uses stringArrayVars_ lookup).
    bool isStringArrayExpr(Expression* expr) const;
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
    std::string marchCpu_;            // -march: CPU arch for instruction selection ("" = native)
    std::string mtuneCpu_;            // -mtune: CPU for scheduling tuning ("" = same as march)
    bool usePIC_ = true;              // -fpic / -fno-pic
    bool useFastMath_ = false;        // -ffast-math / -fno-fast-math
    bool enableOptMax_ = true;        // -foptmax / -fno-optmax
    bool enableVectorize_ = true;     // -fvectorize / -fno-vectorize
    bool enableUnrollLoops_ = true;   // -funroll-loops / -fno-unroll-loops
    bool enableLoopOptimize_ = true;  // -floop-optimize / -fno-loop-optimize
    std::string pgoGenPath_;          // --pgo-gen=<path>: emit raw profile to this file
    std::string pgoUsePath_;          // --pgo-use=<path>: read profile data from this file
    bool dynamicCompilation_ = false; // Dynamic (JIT) compilation mode
    bool lto_ = false;                // LTO mode: use pre-link pipeline

    // DWARF debug info infrastructure
    bool debugMode_ = false;                       // -g: emit debug metadata
    std::string sourceFilename_;                   // Source file for debug CU
    std::unique_ptr<llvm::DIBuilder> debugBuilder_; // Debug info builder (null if !debugMode_)
    llvm::DICompileUnit* debugCU_ = nullptr;       // DWARF compile unit
    llvm::DIFile* debugFile_ = nullptr;            // DWARF file descriptor
    llvm::DIScope* debugScope_ = nullptr;          // Current debug scope (CU or subprogram)

    /// Compile-time resource budget — limits to prevent DoS via oversized inputs.
    /// Checked during code generation to abort compilation if the program
    /// exceeds reasonable complexity bounds.
    /// Note: not atomic — CodeGenerator instances are not shared across threads.
    static constexpr size_t kMaxFunctions = 10000;
    static constexpr size_t kMaxIRInstructions = 1000000;
    size_t irInstructionCount_ = 0;

    /// Increment the IR instruction counter and abort if the budget is exceeded.
    void checkIRBudget() {
        if (++irInstructionCount_ > kMaxIRInstructions) {
            throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error,
                                             {"", 0, 0},
                                             "Compilation aborted: IR instruction limit exceeded (" +
                                                 std::to_string(kMaxIRInstructions) +
                                                 "). Input program is too large or complex."});
        }
    }

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
    llvm::Function* getOrDeclareFgets();
    llvm::Function* getOrDeclareFopen();
    llvm::Function* getOrDeclareFclose();
    llvm::Function* getOrDeclareFread();
    llvm::Function* getOrDeclareFseek();
    llvm::Function* getOrDeclareFtell();
    llvm::Function* getOrDeclareAccess();
    llvm::Function* getOrDeclarePthreadCreate();
    llvm::Function* getOrDeclarePthreadJoin();
    llvm::Function* getOrDeclarePthreadMutexInit();
    llvm::Function* getOrDeclarePthreadMutexLock();
    llvm::Function* getOrDeclarePthreadMutexUnlock();
    llvm::Function* getOrDeclarePthreadMutexDestroy();

    /// Shared implementation for prefix and postfix increment/decrement.
    /// Returns the *old* value for postfix (isPostfix=true) and the *new*
    /// value for prefix (isPostfix=false).
    llvm::Value* generateIncDec(Expression* operandExpr, const std::string& op, bool isPostfix,
                                const ASTNode* errorNode);

    // Optimization methods
    void runOptimizationPasses();
    void optimizeOptMaxFunctions();

    /// Apply intra-procedural optimization passes for the JIT baseline.
    /// Improves IR quality (mem2reg, instcombine, GVN, LICM, etc.) without
    /// IPO passes that would destroy function boundaries needed for hot-patching.
    void runJITBaselinePasses();

  public:
    // Per-function optimization for targeted optimization of individual functions
    void optimizeFunction(llvm::Function* func);

    /// Compile all functions to native LLVM IR.  Equivalent to generate()
    /// in the current single-tier model; retained as a separate entry point
    /// so the adaptive JIT runner can use a distinct call site from the
    /// traditional AOT compile path.
    void generateHybrid(Program* program);

    /// Return the execution tier assigned to a function, or AOT if not found.
    [[nodiscard]] ExecutionTier getFunctionTier(const std::string& name) const {
        auto it = functionTiers.find(name);
        return it != functionTiers.end() ? it->second : ExecutionTier::AOT;
    }

    /// Return a read-only view of all function tier assignments.
    [[nodiscard]] const std::unordered_map<std::string, ExecutionTier>& getFunctionTiers() const noexcept {
        return functionTiers;
    }
};

} // namespace omscript

#endif // CODEGEN_H
