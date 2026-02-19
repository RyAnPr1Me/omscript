#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "bytecode.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>

namespace omscript {

// Returns true if the given name is a stdlib built-in function.
// Stdlib functions are always compiled to native machine code.
bool isStdlibFunction(const std::string& name);

enum class OptimizationLevel {
    O0,  // No optimization
    O1,  // Basic optimization
    O2,  // Moderate optimization
    O3   // Aggressive optimization
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
        case ExecutionTier::AOT:         return "AOT";
        case ExecutionTier::Interpreted: return "Interpreted";
        case ExecutionTier::JIT:         return "JIT";
    }
    return "Unknown";
}

class CodeGenerator {
public:
    CodeGenerator(OptimizationLevel optLevel = OptimizationLevel::O2);
    ~CodeGenerator();
    
    void generate(Program* program);
    void writeObjectFile(const std::string& filename);
    llvm::Module* getModule() { return module.get(); }
    void setOptimizationLevel(OptimizationLevel level) { optimizationLevel = level; }
    
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
    
    // Bytecode emitter for dynamic/interpreted code (user-defined functions only;
    // stdlib built-ins are always compiled to native machine code via LLVM IR).
    BytecodeEmitter bytecodeEmitter;
    bool useDynamicCompilation;
    OptimizationLevel optimizationLevel;

    // Per-function execution tier decided during code generation.
    std::unordered_map<std::string, ExecutionTier> functionTiers;

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
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* function, const std::string& name, llvm::Type* type = nullptr);
    [[noreturn]] void codegenError(const std::string& message, const ASTNode* node);
    
    // Optimization methods
    void runOptimizationPasses();
    void optimizeOptMaxFunctions();
    
    void emitBytecodeExpression(Expression* expr);
    void emitBytecodeStatement(Statement* stmt);
    void emitBytecodeBlock(BlockStmt* stmt);
    
public:
    // Per-function optimization for targeted optimization of individual functions
    void optimizeFunction(llvm::Function* func);
    
    // Bytecode generation (alternative backend)
    void generateBytecode(Program* program);

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
    const BytecodeEmitter& getBytecodeEmitter() const { return bytecodeEmitter; }
    bool isDynamicCompilation() const { return useDynamicCompilation; }
    void setDynamicCompilation(bool enable) { useDynamicCompilation = enable; }
};

} // namespace omscript

#endif // CODEGEN_H
