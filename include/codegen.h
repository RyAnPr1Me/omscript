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

enum class OptimizationLevel {
    O0,  // No optimization
    O1,  // Basic optimization
    O2,  // Moderate optimization
    O3   // Aggressive optimization
};

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
    
    // Bytecode emitter for dynamic code
    BytecodeEmitter bytecodeEmitter;
    bool useDynamicCompilation;
    OptimizationLevel optimizationLevel;
    
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
    
    // Statement generators
    void generateVarDecl(VarDecl* stmt);
    void generateReturn(ReturnStmt* stmt);
    void generateIf(IfStmt* stmt);
    void generateWhile(WhileStmt* stmt);
    void generateDoWhile(DoWhileStmt* stmt);
    void generateFor(ForStmt* stmt);
    void generateBlock(BlockStmt* stmt);
    void generateExprStmt(ExprStmt* stmt);
    
    // Helper methods
    llvm::Type* getDefaultType();
    void setupPrintfDeclaration();
    llvm::Function* getPrintfFunction();
    void beginScope();
    void endScope();
    void bindVariable(const std::string& name, llvm::Value* value, bool isConst = false);
    void checkConstModification(const std::string& name, const std::string& action);
    void validateScopeStacksMatch(const char* location);
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* function, const std::string& name);
    [[noreturn]] void codegenError(const std::string& message, const ASTNode* node);
    
    // Optimization methods
    void runOptimizationPasses();
    void optimizeFunction(llvm::Function* func);
    void optimizeOptMaxFunctions();
};

} // namespace omscript

#endif // CODEGEN_H
