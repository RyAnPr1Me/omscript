#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "bytecode.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <unordered_map>
#include <memory>

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
    
    // Statement generators
    void generateVarDecl(VarDecl* stmt);
    void generateReturn(ReturnStmt* stmt);
    void generateIf(IfStmt* stmt);
    void generateWhile(WhileStmt* stmt);
    void generateFor(ForStmt* stmt);
    void generateBlock(BlockStmt* stmt);
    void generateExprStmt(ExprStmt* stmt);
    
    // Helper methods
    llvm::Type* getDefaultType();
    void setupPrintfDeclaration();
    llvm::Function* getPrintfFunction();
    
    // Optimization methods
    void runOptimizationPasses();
    void optimizeFunction(llvm::Function* func);
};

} // namespace omscript

#endif // CODEGEN_H
