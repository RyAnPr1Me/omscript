#include "codegen.h"
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <stdexcept>
#include <iostream>

namespace omscript {

CodeGenerator::CodeGenerator() : useDynamicCompilation(false) {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("omscript", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);
    
    setupPrintfDeclaration();
}

CodeGenerator::~CodeGenerator() {}

void CodeGenerator::setupPrintfDeclaration() {
    // Declare printf function for output
    std::vector<llvm::Type*> printfArgs;
    printfArgs.push_back(llvm::Type::getInt8PtrTy(*context));
    
    llvm::FunctionType* printfType = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(*context),
        printfArgs,
        true
    );
    
    llvm::Function::Create(
        printfType,
        llvm::Function::ExternalLinkage,
        "printf",
        module.get()
    );
}

llvm::Function* CodeGenerator::getPrintfFunction() {
    return module->getFunction("printf");
}

llvm::Type* CodeGenerator::getDefaultType() {
    // Default to 64-bit integer for dynamic typing
    return llvm::Type::getInt64Ty(*context);
}

void CodeGenerator::generate(Program* program) {
    // Generate all functions
    for (auto& func : program->functions) {
        generateFunction(func.get());
    }
    
    // Verify the module
    std::string errorStr;
    llvm::raw_string_ostream errorStream(errorStr);
    if (llvm::verifyModule(*module, &errorStream)) {
        std::cerr << "Module verification failed:\n" << errorStr << std::endl;
        throw std::runtime_error("Module verification failed");
    }
}

llvm::Function* CodeGenerator::generateFunction(FunctionDecl* func) {
    // Create function type
    std::vector<llvm::Type*> paramTypes;
    for (size_t i = 0; i < func->parameters.size(); i++) {
        paramTypes.push_back(getDefaultType());
    }
    
    llvm::FunctionType* funcType = llvm::FunctionType::get(
        getDefaultType(),
        paramTypes,
        false
    );
    
    // Create function
    llvm::Function* function = llvm::Function::Create(
        funcType,
        llvm::Function::ExternalLinkage,
        func->name,
        module.get()
    );
    
    functions[func->name] = function;
    
    // Create entry basic block
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", function);
    builder->SetInsertPoint(entry);
    
    // Set parameter names and create allocas
    namedValues.clear();
    auto argIt = function->arg_begin();
    for (auto& param : func->parameters) {
        argIt->setName(param.name);
        
        llvm::AllocaInst* alloca = builder->CreateAlloca(getDefaultType(), nullptr, param.name);
        builder->CreateStore(&(*argIt), alloca);
        namedValues[param.name] = alloca;
        
        ++argIt;
    }
    
    // Generate function body
    generateBlock(func->body.get());
    
    // Add default return if needed
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateRet(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)));
    }
    
    // Verify function
    std::string errorStr;
    llvm::raw_string_ostream errorStream(errorStr);
    if (llvm::verifyFunction(*function, &errorStream)) {
        std::cerr << "Function verification failed:\n" << errorStr << std::endl;
        function->print(llvm::errs());
        throw std::runtime_error("Function verification failed");
    }
    
    return function;
}

void CodeGenerator::generateStatement(Statement* stmt) {
    switch (stmt->type) {
        case ASTNodeType::VAR_DECL:
            generateVarDecl(static_cast<VarDecl*>(stmt));
            break;
        case ASTNodeType::RETURN_STMT:
            generateReturn(static_cast<ReturnStmt*>(stmt));
            break;
        case ASTNodeType::IF_STMT:
            generateIf(static_cast<IfStmt*>(stmt));
            break;
        case ASTNodeType::WHILE_STMT:
            generateWhile(static_cast<WhileStmt*>(stmt));
            break;
        case ASTNodeType::BLOCK:
            generateBlock(static_cast<BlockStmt*>(stmt));
            break;
        case ASTNodeType::EXPR_STMT:
            generateExprStmt(static_cast<ExprStmt*>(stmt));
            break;
        default:
            throw std::runtime_error("Unknown statement type");
    }
}

llvm::Value* CodeGenerator::generateExpression(Expression* expr) {
    switch (expr->type) {
        case ASTNodeType::LITERAL_EXPR:
            return generateLiteral(static_cast<LiteralExpr*>(expr));
        case ASTNodeType::IDENTIFIER_EXPR:
            return generateIdentifier(static_cast<IdentifierExpr*>(expr));
        case ASTNodeType::BINARY_EXPR:
            return generateBinary(static_cast<BinaryExpr*>(expr));
        case ASTNodeType::UNARY_EXPR:
            return generateUnary(static_cast<UnaryExpr*>(expr));
        case ASTNodeType::CALL_EXPR:
            return generateCall(static_cast<CallExpr*>(expr));
        case ASTNodeType::ASSIGN_EXPR:
            return generateAssign(static_cast<AssignExpr*>(expr));
        default:
            throw std::runtime_error("Unknown expression type");
    }
}

llvm::Value* CodeGenerator::generateLiteral(LiteralExpr* expr) {
    if (expr->literalType == LiteralExpr::LiteralType::INTEGER) {
        return llvm::ConstantInt::get(*context, llvm::APInt(64, expr->intValue));
    } else if (expr->literalType == LiteralExpr::LiteralType::FLOAT) {
        // For simplicity, cast float to int64 in this basic implementation
        return llvm::ConstantInt::get(*context, llvm::APInt(64, static_cast<int64_t>(expr->floatValue)));
    } else {
        // String literal - not fully supported in this basic implementation
        return llvm::ConstantInt::get(*context, llvm::APInt(64, 0));
    }
}

llvm::Value* CodeGenerator::generateIdentifier(IdentifierExpr* expr) {
    llvm::Value* value = namedValues[expr->name];
    if (!value) {
        throw std::runtime_error("Unknown variable: " + expr->name);
    }
    return builder->CreateLoad(getDefaultType(), value, expr->name.c_str());
}

llvm::Value* CodeGenerator::generateBinary(BinaryExpr* expr) {
    llvm::Value* left = generateExpression(expr->left.get());
    llvm::Value* right = generateExpression(expr->right.get());
    
    if (expr->op == "+") {
        return builder->CreateAdd(left, right, "addtmp");
    } else if (expr->op == "-") {
        return builder->CreateSub(left, right, "subtmp");
    } else if (expr->op == "*") {
        return builder->CreateMul(left, right, "multmp");
    } else if (expr->op == "/") {
        return builder->CreateSDiv(left, right, "divtmp");
    } else if (expr->op == "%") {
        return builder->CreateSRem(left, right, "modtmp");
    } else if (expr->op == "==") {
        llvm::Value* cmp = builder->CreateICmpEQ(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == "!=") {
        llvm::Value* cmp = builder->CreateICmpNE(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == "<") {
        llvm::Value* cmp = builder->CreateICmpSLT(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == "<=") {
        llvm::Value* cmp = builder->CreateICmpSLE(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == ">") {
        llvm::Value* cmp = builder->CreateICmpSGT(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == ">=") {
        llvm::Value* cmp = builder->CreateICmpSGE(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == "&&") {
        llvm::Value* leftBool = builder->CreateICmpNE(left, llvm::ConstantInt::get(*context, llvm::APInt(64, 0)), "leftbool");
        llvm::Value* rightBool = builder->CreateICmpNE(right, llvm::ConstantInt::get(*context, llvm::APInt(64, 0)), "rightbool");
        llvm::Value* result = builder->CreateAnd(leftBool, rightBool, "andtmp");
        return builder->CreateZExt(result, getDefaultType(), "booltmp");
    } else if (expr->op == "||") {
        llvm::Value* leftBool = builder->CreateICmpNE(left, llvm::ConstantInt::get(*context, llvm::APInt(64, 0)), "leftbool");
        llvm::Value* rightBool = builder->CreateICmpNE(right, llvm::ConstantInt::get(*context, llvm::APInt(64, 0)), "rightbool");
        llvm::Value* result = builder->CreateOr(leftBool, rightBool, "ortmp");
        return builder->CreateZExt(result, getDefaultType(), "booltmp");
    }
    
    throw std::runtime_error("Unknown binary operator: " + expr->op);
}

llvm::Value* CodeGenerator::generateUnary(UnaryExpr* expr) {
    llvm::Value* operand = generateExpression(expr->operand.get());
    
    if (expr->op == "-") {
        return builder->CreateNeg(operand, "negtmp");
    } else if (expr->op == "!") {
        llvm::Value* cmp = builder->CreateICmpEQ(operand, llvm::ConstantInt::get(*context, llvm::APInt(64, 0)), "nottmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    }
    
    throw std::runtime_error("Unknown unary operator: " + expr->op);
}

llvm::Value* CodeGenerator::generateCall(CallExpr* expr) {
    llvm::Function* callee = functions[expr->callee];
    if (!callee) {
        throw std::runtime_error("Unknown function: " + expr->callee);
    }
    
    if (callee->arg_size() != expr->arguments.size()) {
        throw std::runtime_error("Incorrect number of arguments");
    }
    
    std::vector<llvm::Value*> args;
    for (auto& arg : expr->arguments) {
        args.push_back(generateExpression(arg.get()));
    }
    
    return builder->CreateCall(callee, args, "calltmp");
}

llvm::Value* CodeGenerator::generateAssign(AssignExpr* expr) {
    llvm::Value* value = generateExpression(expr->value.get());
    llvm::Value* variable = namedValues[expr->name];
    
    if (!variable) {
        throw std::runtime_error("Unknown variable: " + expr->name);
    }
    
    builder->CreateStore(value, variable);
    return value;
}

void CodeGenerator::generateVarDecl(VarDecl* stmt) {
    llvm::AllocaInst* alloca = builder->CreateAlloca(getDefaultType(), nullptr, stmt->name);
    namedValues[stmt->name] = alloca;
    
    if (stmt->initializer) {
        llvm::Value* initValue = generateExpression(stmt->initializer.get());
        builder->CreateStore(initValue, alloca);
    } else {
        builder->CreateStore(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)), alloca);
    }
}

void CodeGenerator::generateReturn(ReturnStmt* stmt) {
    if (stmt->value) {
        llvm::Value* retValue = generateExpression(stmt->value.get());
        builder->CreateRet(retValue);
    } else {
        builder->CreateRet(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)));
    }
}

void CodeGenerator::generateIf(IfStmt* stmt) {
    llvm::Value* condition = generateExpression(stmt->condition.get());
    llvm::Value* condBool = builder->CreateICmpNE(
        condition,
        llvm::ConstantInt::get(*context, llvm::APInt(64, 0)),
        "ifcond"
    );
    
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    
    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(*context, "then", function);
    llvm::BasicBlock* elseBB = stmt->elseBranch ? llvm::BasicBlock::Create(*context, "else") : nullptr;
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "ifcont");
    
    if (elseBB) {
        builder->CreateCondBr(condBool, thenBB, elseBB);
    } else {
        builder->CreateCondBr(condBool, thenBB, mergeBB);
    }
    
    // Then block
    builder->SetInsertPoint(thenBB);
    generateStatement(stmt->thenBranch.get());
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(mergeBB);
    }
    
    // Else block
    if (elseBB) {
        function->getBasicBlockList().push_back(elseBB);
        builder->SetInsertPoint(elseBB);
        generateStatement(stmt->elseBranch.get());
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(mergeBB);
        }
    }
    
    // Merge block
    function->getBasicBlockList().push_back(mergeBB);
    builder->SetInsertPoint(mergeBB);
}

void CodeGenerator::generateWhile(WhileStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "whilecond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "whilebody");
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "whileend");
    
    builder->CreateBr(condBB);
    
    // Condition block
    builder->SetInsertPoint(condBB);
    llvm::Value* condition = generateExpression(stmt->condition.get());
    llvm::Value* condBool = builder->CreateICmpNE(
        condition,
        llvm::ConstantInt::get(*context, llvm::APInt(64, 0)),
        "whilecond"
    );
    builder->CreateCondBr(condBool, bodyBB, endBB);
    
    // Body block
    function->getBasicBlockList().push_back(bodyBB);
    builder->SetInsertPoint(bodyBB);
    generateStatement(stmt->body.get());
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(condBB);
    }
    
    // End block
    function->getBasicBlockList().push_back(endBB);
    builder->SetInsertPoint(endBB);
}

void CodeGenerator::generateBlock(BlockStmt* stmt) {
    for (auto& statement : stmt->statements) {
        if (builder->GetInsertBlock()->getTerminator()) {
            break;  // Don't generate unreachable code
        }
        generateStatement(statement.get());
    }
}

void CodeGenerator::generateExprStmt(ExprStmt* stmt) {
    generateExpression(stmt->expression.get());
}

void CodeGenerator::writeObjectFile(const std::string& filename) {
    // Initialize target
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
    
    auto targetTriple = llvm::sys::getDefaultTargetTriple();
    module->setTargetTriple(targetTriple);
    
    std::string error;
    auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
    
    if (!target) {
        throw std::runtime_error("Failed to lookup target: " + error);
    }
    
    auto CPU = "generic";
    auto features = "";
    
    llvm::TargetOptions opt;
    auto RM = llvm::Optional<llvm::Reloc::Model>();
    auto targetMachine = target->createTargetMachine(targetTriple, CPU, features, opt, RM);
    
    module->setDataLayout(targetMachine->createDataLayout());
    
    std::error_code EC;
    llvm::raw_fd_ostream dest(filename, EC, llvm::sys::fs::OF_None);
    
    if (EC) {
        throw std::runtime_error("Could not open file: " + EC.message());
    }
    
    llvm::legacy::PassManager pass;
    auto fileType = llvm::CGFT_ObjectFile;
    
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        throw std::runtime_error("TargetMachine can't emit a file of this type");
    }
    
    pass.run(*module);
    dest.flush();
}

} // namespace omscript
