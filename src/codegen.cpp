#include "codegen.h"
#include <llvm/IR/Verifier.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Passes/PassBuilder.h>
#include <stdexcept>
#include <iostream>
#include <optional>

namespace {

using omscript::ASTNodeType;
using omscript::BlockStmt;
using omscript::BinaryExpr;
using omscript::Expression;
using omscript::LiteralExpr;
using omscript::UnaryExpr;
using omscript::AssignExpr;
using omscript::CallExpr;
using omscript::ArrayExpr;
using omscript::IndexExpr;
using omscript::PostfixExpr;
using omscript::ExprStmt;
using omscript::VarDecl;
using omscript::ReturnStmt;
using omscript::IfStmt;
using omscript::WhileStmt;
using omscript::ForStmt;
using omscript::Statement;

std::unique_ptr<Expression> optimizeOptMaxExpression(std::unique_ptr<Expression> expr);

std::unique_ptr<Expression> optimizeOptMaxUnary(const std::string& op, std::unique_ptr<Expression> operand) {
    operand = optimizeOptMaxExpression(std::move(operand));
    auto* literal = dynamic_cast<LiteralExpr*>(operand.get());
    if (!literal) {
        return std::make_unique<UnaryExpr>(op, std::move(operand));
    }
    
    if (literal->literalType == LiteralExpr::LiteralType::INTEGER) {
        long long value = literal->intValue;
        if (op == "-") {
            return std::make_unique<LiteralExpr>(-value);
        }
        if (op == "!") {
            return std::make_unique<LiteralExpr>(static_cast<long long>(value == 0));
        }
    } else if (literal->literalType == LiteralExpr::LiteralType::FLOAT) {
        double value = literal->floatValue;
        if (op == "-") {
            return std::make_unique<LiteralExpr>(-value);
        }
        if (op == "!") {
            return std::make_unique<LiteralExpr>(static_cast<long long>(value == 0.0));
        }
    }
    
    return std::make_unique<UnaryExpr>(op, std::move(operand));
}

std::unique_ptr<Expression> optimizeOptMaxBinary(const std::string& op,
                                                 std::unique_ptr<Expression> left,
                                                 std::unique_ptr<Expression> right) {
    left = optimizeOptMaxExpression(std::move(left));
    right = optimizeOptMaxExpression(std::move(right));
    auto* leftLiteral = dynamic_cast<LiteralExpr*>(left.get());
    auto* rightLiteral = dynamic_cast<LiteralExpr*>(right.get());
    if (!leftLiteral || !rightLiteral) {
        return std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    if (leftLiteral->literalType == LiteralExpr::LiteralType::INTEGER &&
        rightLiteral->literalType == LiteralExpr::LiteralType::INTEGER) {
        long long lval = leftLiteral->intValue;
        long long rval = rightLiteral->intValue;
        if (op == "+") return std::make_unique<LiteralExpr>(lval + rval);
        if (op == "-") return std::make_unique<LiteralExpr>(lval - rval);
        if (op == "*") return std::make_unique<LiteralExpr>(lval * rval);
        if (op == "/" && rval != 0) return std::make_unique<LiteralExpr>(lval / rval);
        if (op == "%" && rval != 0) return std::make_unique<LiteralExpr>(lval % rval);
        if (op == "==") return std::make_unique<LiteralExpr>(static_cast<long long>(lval == rval));
        if (op == "!=") return std::make_unique<LiteralExpr>(static_cast<long long>(lval != rval));
        if (op == "<") return std::make_unique<LiteralExpr>(static_cast<long long>(lval < rval));
        if (op == "<=") return std::make_unique<LiteralExpr>(static_cast<long long>(lval <= rval));
        if (op == ">") return std::make_unique<LiteralExpr>(static_cast<long long>(lval > rval));
        if (op == ">=") return std::make_unique<LiteralExpr>(static_cast<long long>(lval >= rval));
        if (op == "&&") return std::make_unique<LiteralExpr>(static_cast<long long>((lval != 0) && (rval != 0)));
        if (op == "||") return std::make_unique<LiteralExpr>(static_cast<long long>((lval != 0) || (rval != 0)));
    } else if (leftLiteral->literalType == LiteralExpr::LiteralType::FLOAT &&
               rightLiteral->literalType == LiteralExpr::LiteralType::FLOAT) {
        double lval = leftLiteral->floatValue;
        double rval = rightLiteral->floatValue;
        if (op == "+") return std::make_unique<LiteralExpr>(lval + rval);
        if (op == "-") return std::make_unique<LiteralExpr>(lval - rval);
        if (op == "*") return std::make_unique<LiteralExpr>(lval * rval);
        if (op == "/" && rval != 0.0) return std::make_unique<LiteralExpr>(lval / rval);
        if (op == "==") return std::make_unique<LiteralExpr>(static_cast<long long>(lval == rval));
        if (op == "!=") return std::make_unique<LiteralExpr>(static_cast<long long>(lval != rval));
        if (op == "<") return std::make_unique<LiteralExpr>(static_cast<long long>(lval < rval));
        if (op == "<=") return std::make_unique<LiteralExpr>(static_cast<long long>(lval <= rval));
        if (op == ">") return std::make_unique<LiteralExpr>(static_cast<long long>(lval > rval));
        if (op == ">=") return std::make_unique<LiteralExpr>(static_cast<long long>(lval >= rval));
        if (op == "&&") return std::make_unique<LiteralExpr>(static_cast<long long>((lval != 0.0) && (rval != 0.0)));
        if (op == "||") return std::make_unique<LiteralExpr>(static_cast<long long>((lval != 0.0) || (rval != 0.0)));
    }
    
    return std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
}

std::unique_ptr<Expression> optimizeOptMaxExpression(std::unique_ptr<Expression> expr) {
    if (!expr) {
        return nullptr;
    }
    
    switch (expr->type) {
        case ASTNodeType::LITERAL_EXPR:
        case ASTNodeType::IDENTIFIER_EXPR:
            return expr;
        case ASTNodeType::UNARY_EXPR: {
            auto* unary = static_cast<UnaryExpr*>(expr.get());
            return optimizeOptMaxUnary(unary->op, std::move(unary->operand));
        }
        case ASTNodeType::BINARY_EXPR: {
            auto* binary = static_cast<BinaryExpr*>(expr.get());
            return optimizeOptMaxBinary(binary->op, std::move(binary->left), std::move(binary->right));
        }
        case ASTNodeType::ASSIGN_EXPR: {
            auto* assign = static_cast<AssignExpr*>(expr.get());
            assign->value = optimizeOptMaxExpression(std::move(assign->value));
            return expr;
        }
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<CallExpr*>(expr.get());
            for (auto& arg : call->arguments) {
                arg = optimizeOptMaxExpression(std::move(arg));
            }
            return expr;
        }
        case ASTNodeType::ARRAY_EXPR: {
            auto* arrayExpr = static_cast<ArrayExpr*>(expr.get());
            for (auto& element : arrayExpr->elements) {
                element = optimizeOptMaxExpression(std::move(element));
            }
            return expr;
        }
        case ASTNodeType::INDEX_EXPR: {
            auto* indexExpr = static_cast<IndexExpr*>(expr.get());
            indexExpr->array = optimizeOptMaxExpression(std::move(indexExpr->array));
            indexExpr->index = optimizeOptMaxExpression(std::move(indexExpr->index));
            return expr;
        }
        case ASTNodeType::POSTFIX_EXPR: {
            auto* postfix = static_cast<PostfixExpr*>(expr.get());
            postfix->operand = optimizeOptMaxExpression(std::move(postfix->operand));
            return expr;
        }
        default:
            return expr;
    }
}

void optimizeOptMaxStatement(Statement* stmt);

void optimizeOptMaxBlock(BlockStmt* block) {
    for (auto& statement : block->statements) {
        optimizeOptMaxStatement(statement.get());
    }
}

void optimizeOptMaxStatement(Statement* stmt) {
    switch (stmt->type) {
        case ASTNodeType::BLOCK:
            optimizeOptMaxBlock(static_cast<BlockStmt*>(stmt));
            break;
        case ASTNodeType::VAR_DECL: {
            auto* varDecl = static_cast<VarDecl*>(stmt);
            if (varDecl->initializer) {
                varDecl->initializer = optimizeOptMaxExpression(std::move(varDecl->initializer));
            }
            break;
        }
        case ASTNodeType::RETURN_STMT: {
            auto* retStmt = static_cast<ReturnStmt*>(stmt);
            if (retStmt->value) {
                retStmt->value = optimizeOptMaxExpression(std::move(retStmt->value));
            }
            break;
        }
        case ASTNodeType::EXPR_STMT: {
            auto* exprStmt = static_cast<ExprStmt*>(stmt);
            exprStmt->expression = optimizeOptMaxExpression(std::move(exprStmt->expression));
            break;
        }
        case ASTNodeType::IF_STMT: {
            auto* ifStmt = static_cast<IfStmt*>(stmt);
            ifStmt->condition = optimizeOptMaxExpression(std::move(ifStmt->condition));
            optimizeOptMaxStatement(ifStmt->thenBranch.get());
            if (ifStmt->elseBranch) {
                optimizeOptMaxStatement(ifStmt->elseBranch.get());
            }
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            auto* whileStmt = static_cast<WhileStmt*>(stmt);
            whileStmt->condition = optimizeOptMaxExpression(std::move(whileStmt->condition));
            optimizeOptMaxStatement(whileStmt->body.get());
            break;
        }
        case ASTNodeType::FOR_STMT: {
            auto* forStmt = static_cast<ForStmt*>(stmt);
            forStmt->start = optimizeOptMaxExpression(std::move(forStmt->start));
            forStmt->end = optimizeOptMaxExpression(std::move(forStmt->end));
            if (forStmt->step) {
                forStmt->step = optimizeOptMaxExpression(std::move(forStmt->step));
            }
            optimizeOptMaxStatement(forStmt->body.get());
            break;
        }
        default:
            break;
    }
}

} // namespace

namespace omscript {

CodeGenerator::CodeGenerator(OptimizationLevel optLevel) 
    : useDynamicCompilation(false),
      optimizationLevel(optLevel),
      inOptMaxFunction(false),
      hasOptMaxFunctions(false) {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("omscript", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);
    
    setupPrintfDeclaration();
}

CodeGenerator::~CodeGenerator() {}

void CodeGenerator::setupPrintfDeclaration() {
    // Declare printf function for output
    std::vector<llvm::Type*> printfArgs;
    printfArgs.push_back(llvm::PointerType::getUnqual(*context));
    
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

void CodeGenerator::beginScope() {
    validateScopeStacksMatch(__func__);
    scopeStack.emplace_back();
    constScopeStack.emplace_back();
}

void CodeGenerator::endScope() {
    validateScopeStacksMatch(__func__);
    if (scopeStack.empty()) {
        return;
    }
    
    auto& scope = scopeStack.back();
    auto& constScope = constScopeStack.back();
    for (const auto& entry : scope) {
        if (entry.second) {
            namedValues[entry.first] = entry.second;
        } else {
            namedValues.erase(entry.first);
        }
    }
    for (const auto& entry : constScope) {
        if (entry.second.wasPreviouslyDefined) {
            constValues[entry.first] = entry.second.previousIsConst;
        } else {
            constValues.erase(entry.first);
        }
    }
    scopeStack.pop_back();
    constScopeStack.pop_back();
}

void CodeGenerator::bindVariable(const std::string& name, llvm::Value* value, bool isConst) {
    if (!scopeStack.empty()) {
        auto& scope = scopeStack.back();
        if (scope.find(name) == scope.end()) {
            auto existing = namedValues.find(name);
            scope[name] = existing == namedValues.end() ? nullptr : existing->second;
        }
    }
    if (!constScopeStack.empty()) {
        auto& constScope = constScopeStack.back();
        if (constScope.find(name) == constScope.end()) {
            auto existingConst = constValues.find(name);
            if (existingConst == constValues.end()) {
                constScope[name] = {false, false};
            } else {
                constScope[name] = {true, existingConst->second};
            }
        }
    }
    namedValues[name] = value;
    constValues[name] = isConst;
}

void CodeGenerator::checkConstModification(const std::string& name, const std::string& action) {
    auto constIt = constValues.find(name);
    if (constIt != constValues.end() && constIt->second) {
        throw std::runtime_error("Cannot " + action + " const variable: " + name);
    }
}

void CodeGenerator::validateScopeStacksMatch(const char* location) {
    if (scopeStack.size() != constScopeStack.size()) {
        throw std::runtime_error("Scope tracking mismatch in codegen (" + std::string(location) + "): values=" +
                                 std::to_string(scopeStack.size()) + ", consts=" +
                                 std::to_string(constScopeStack.size()));
    }
}

llvm::AllocaInst* CodeGenerator::createEntryBlockAlloca(llvm::Function* function, const std::string& name) {
    llvm::IRBuilder<> entryBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
    return entryBuilder.CreateAlloca(getDefaultType(), nullptr, name);
}

void CodeGenerator::generate(Program* program) {
    hasOptMaxFunctions = false;
    optMaxFunctions.clear();
    for (auto& func : program->functions) {
        if (func->isOptMax) {
            optMaxFunctions.insert(func->name);
        }
    }
    
    // Generate all functions
    for (auto& func : program->functions) {
        generateFunction(func.get());
    }

    // Run optimization passes
    if (optimizationLevel != OptimizationLevel::O0) {
        runOptimizationPasses();
    }
    
    if (hasOptMaxFunctions) {
        optimizeOptMaxFunctions();
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
    inOptMaxFunction = func->isOptMax;
    hasOptMaxFunctions = hasOptMaxFunctions || func->isOptMax;
    if (func->isOptMax) {
        optimizeOptMaxBlock(func->body.get());
    }

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
    scopeStack.clear();
    loopStack.clear();
    constValues.clear();
    constScopeStack.clear();
    auto argIt = function->arg_begin();
    for (auto& param : func->parameters) {
        argIt->setName(param.name);
        
        llvm::AllocaInst* alloca = createEntryBlockAlloca(function, param.name);
        builder->CreateStore(&(*argIt), alloca);
        bindVariable(param.name, alloca);
        
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
    
    inOptMaxFunction = false;
    
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
        case ASTNodeType::FOR_STMT:
            generateFor(static_cast<ForStmt*>(stmt));
            break;
        case ASTNodeType::BREAK_STMT:
            if (loopStack.empty()) {
                throw std::runtime_error("break used outside of a loop");
            }
            builder->CreateBr(loopStack.back().breakTarget);
            break;
        case ASTNodeType::CONTINUE_STMT:
            if (loopStack.empty()) {
                throw std::runtime_error("continue used outside of a loop");
            }
            builder->CreateBr(loopStack.back().continueTarget);
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
        case ASTNodeType::POSTFIX_EXPR:
            return generatePostfix(static_cast<PostfixExpr*>(expr));
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
    auto it = namedValues.find(expr->name);
    if (it == namedValues.end() || !it->second) {
        throw std::runtime_error("Unknown variable: " + expr->name);
    }
    return builder->CreateLoad(getDefaultType(), it->second, expr->name.c_str());
}

llvm::Value* CodeGenerator::generateBinary(BinaryExpr* expr) {
    llvm::Value* left = generateExpression(expr->left.get());
    if (expr->op == "&&" || expr->op == "||") {
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* leftBool = builder->CreateICmpNE(left, zero, "leftbool");
        llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(*context, "logic.rhs", function);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "logic.cont", function);
        if (expr->op == "&&") {
            builder->CreateCondBr(leftBool, rhsBB, mergeBB);
        } else {
            builder->CreateCondBr(leftBool, mergeBB, rhsBB);
        }
        llvm::BasicBlock* leftBB = builder->GetInsertBlock();
        builder->SetInsertPoint(rhsBB);
        llvm::Value* right = generateExpression(expr->right.get());
        llvm::Value* rightBool = builder->CreateICmpNE(right, zero, "rightbool");
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* rightBB = builder->GetInsertBlock();
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(llvm::Type::getInt1Ty(*context), 2, "bool_result");
        if (expr->op == "&&") {
            phi->addIncoming(llvm::ConstantInt::getFalse(*context), leftBB);
            phi->addIncoming(rightBool, rightBB);
        } else {
            phi->addIncoming(llvm::ConstantInt::getTrue(*context), leftBB);
            phi->addIncoming(rightBool, rightBB);
        }
        return builder->CreateZExt(phi, getDefaultType(), "booltmp");
    }
    
    llvm::Value* right = generateExpression(expr->right.get());
    
    // Constant folding optimization - if both operands are constants, compute at compile time
    if (llvm::isa<llvm::ConstantInt>(left) && llvm::isa<llvm::ConstantInt>(right)) {
        auto leftConst = llvm::dyn_cast<llvm::ConstantInt>(left);
        auto rightConst = llvm::dyn_cast<llvm::ConstantInt>(right);
        int64_t lval = leftConst->getSExtValue();
        int64_t rval = rightConst->getSExtValue();
        
        if (expr->op == "+") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval + rval));
        } else if (expr->op == "-") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval - rval));
        } else if (expr->op == "*") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval * rval));
        } else if (expr->op == "/") {
            if (rval != 0) {
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval / rval));
            }
        } else if (expr->op == "%") {
            if (rval != 0) {
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval % rval));
            }
        }
    }
    
    // Regular code generation for non-constant expressions
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
    }
    
    throw std::runtime_error("Unknown binary operator: " + expr->op);
}

llvm::Value* CodeGenerator::generateUnary(UnaryExpr* expr) {
    llvm::Value* operand = generateExpression(expr->operand.get());
    
    if (expr->op == "-") {
        return builder->CreateNeg(operand, "negtmp");
    } else if (expr->op == "!") {
        llvm::Value* cmp = builder->CreateICmpEQ(
            operand,
            llvm::ConstantInt::get(getDefaultType(), 0, true),
            "nottmp"
        );
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    }
    
    throw std::runtime_error("Unknown unary operator: " + expr->op);
}

llvm::Value* CodeGenerator::generateCall(CallExpr* expr) {
    if (inOptMaxFunction) {
        if (optMaxFunctions.find(expr->callee) == optMaxFunctions.end()) {
            std::string currentFunction = "<unknown>";
            if (builder->GetInsertBlock() && builder->GetInsertBlock()->getParent()) {
                currentFunction = std::string(builder->GetInsertBlock()->getParent()->getName());
            }
            throw std::runtime_error("OPTMAX function \"" + currentFunction +
                                     "\" cannot invoke non-OPTMAX function \"" +
                                     expr->callee + "\"");
        }
    }
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
    auto it = namedValues.find(expr->name);
    if (it == namedValues.end() || !it->second) {
        throw std::runtime_error("Unknown variable: " + expr->name);
    }
    checkConstModification(expr->name, "modify");
    
    builder->CreateStore(value, it->second);
    return value;
}

llvm::Value* CodeGenerator::generatePostfix(PostfixExpr* expr) {
    auto* identifier = dynamic_cast<IdentifierExpr*>(expr->operand.get());
    if (!identifier) {
        throw std::runtime_error("Postfix operators require an identifier");
    }
    
    auto it = namedValues.find(identifier->name);
    if (it == namedValues.end() || !it->second) {
        throw std::runtime_error("Unknown variable: " + identifier->name);
    }
    checkConstModification(identifier->name, "modify");
    
    llvm::Value* current = builder->CreateLoad(getDefaultType(), it->second, identifier->name.c_str());
    llvm::Value* delta = llvm::ConstantInt::get(getDefaultType(), 1, true);
    llvm::Value* updated = nullptr;
    if (expr->op == "++") {
        updated = builder->CreateAdd(current, delta, "postinc");
    } else if (expr->op == "--") {
        updated = builder->CreateSub(current, delta, "postdec");
    } else {
        throw std::runtime_error("Unknown postfix operator: " + expr->op);
    }
    
    builder->CreateStore(updated, it->second);
    return current;
}

void CodeGenerator::generateVarDecl(VarDecl* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        throw std::runtime_error("Variable declaration outside of function");
    }
    llvm::AllocaInst* alloca = createEntryBlockAlloca(function, stmt->name);
    bindVariable(stmt->name, alloca, stmt->isConst);
    
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
        llvm::ConstantInt::get(getDefaultType(), 0, true),
        "ifcond"
    );
    
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    
    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(*context, "then", function);
    llvm::BasicBlock* elseBB = stmt->elseBranch ? llvm::BasicBlock::Create(*context, "else", function) : nullptr;
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "ifcont", function);
    
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
        builder->SetInsertPoint(elseBB);
        generateStatement(stmt->elseBranch.get());
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(mergeBB);
        }
    }
    
    // Merge block
    builder->SetInsertPoint(mergeBB);
}

void CodeGenerator::generateWhile(WhileStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "whilecond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "whilebody", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "whileend", function);
    
    builder->CreateBr(condBB);
    
    // Condition block
    builder->SetInsertPoint(condBB);
    llvm::Value* condition = generateExpression(stmt->condition.get());
    llvm::Value* condBool = builder->CreateICmpNE(
        condition,
        llvm::ConstantInt::get(getDefaultType(), 0, true),
        "whilecond"
    );
    builder->CreateCondBr(condBool, bodyBB, endBB);
    
    // Body block
    builder->SetInsertPoint(bodyBB);
    loopStack.push_back({endBB, condBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(condBB);
    }
    
    // End block
    builder->SetInsertPoint(endBB);
}

void CodeGenerator::generateFor(ForStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        throw std::runtime_error("For loop outside of function");
    }
    
    beginScope();
    
    // Allocate iterator variable
    llvm::AllocaInst* iterAlloca = createEntryBlockAlloca(function, stmt->iteratorVar);
    bindVariable(stmt->iteratorVar, iterAlloca);
    
    // Initialize iterator
    llvm::Value* startVal = generateExpression(stmt->start.get());
    builder->CreateStore(startVal, iterAlloca);
    
    // Get end value
    llvm::Value* endVal = generateExpression(stmt->end.get());
    
    // Get step value (default to 1 if not specified)
    llvm::Value* stepVal;
    if (stmt->step) {
        if (auto* literal = dynamic_cast<LiteralExpr*>(stmt->step.get())) {
            bool isZero = (literal->literalType == LiteralExpr::LiteralType::INTEGER && literal->intValue == 0) ||
                          (literal->literalType == LiteralExpr::LiteralType::FLOAT && literal->floatValue == 0.0);
            if (isZero) {
                throw std::runtime_error("For loop step cannot be zero");
            }
        }
        stepVal = generateExpression(stmt->step.get());
    } else {
        stepVal = llvm::ConstantInt::get(*context, llvm::APInt(64, 1));
    }
    
    // Create blocks
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "forcond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "forbody", function);
    llvm::BasicBlock* incBB = llvm::BasicBlock::Create(*context, "forinc", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "forend", function);
    
    builder->CreateBr(condBB);
    
    // Condition block: check if iterator < end (forward) or > end (backward)
    builder->SetInsertPoint(condBB);
    llvm::Value* curVal = builder->CreateLoad(getDefaultType(), iterAlloca, stmt->iteratorVar.c_str());
    llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
    llvm::Value* stepNonZero = builder->CreateICmpNE(stepVal, zero, "stepnonzero");
    llvm::Value* stepPositive = builder->CreateICmpSGT(stepVal, zero, "steppositive");
    llvm::Value* forwardCond = builder->CreateICmpSLT(curVal, endVal, "forcond_lt");
    llvm::Value* backwardCond = builder->CreateICmpSGT(curVal, endVal, "forcond_gt");
    llvm::Value* rangeCond = builder->CreateSelect(stepPositive, forwardCond, backwardCond, "forcond_range");
    llvm::Value* condBool = builder->CreateAnd(stepNonZero, rangeCond, "forcond");
    builder->CreateCondBr(condBool, bodyBB, endBB);
    
    // Body block
    builder->SetInsertPoint(bodyBB);
    loopStack.push_back({endBB, incBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(incBB);
    }
    
    // Increment block
    builder->SetInsertPoint(incBB);
    llvm::Value* nextVal = builder->CreateLoad(getDefaultType(), iterAlloca, stmt->iteratorVar.c_str());
    llvm::Value* incVal = builder->CreateAdd(nextVal, stepVal, "nextvar");
    builder->CreateStore(incVal, iterAlloca);
    builder->CreateBr(condBB);
    
    // End block
    builder->SetInsertPoint(endBB);
    
    endScope();
}

void CodeGenerator::generateBlock(BlockStmt* stmt) {
    beginScope();
    for (auto& statement : stmt->statements) {
        if (builder->GetInsertBlock()->getTerminator()) {
            break;  // Don't generate unreachable code
        }
        generateStatement(statement.get());
    }
    endScope();
}

void CodeGenerator::generateExprStmt(ExprStmt* stmt) {
    generateExpression(stmt->expression.get());
}

void CodeGenerator::runOptimizationPasses() {
    // Create a function pass manager
    llvm::legacy::FunctionPassManager fpm(module.get());
    llvm::legacy::PassManager mpm;
    
    // Add target-specific data layout
    std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
    module->setTargetTriple(targetTriple);
#else
    module->setTargetTriple(targetTripleStr);
#endif
    
    std::string error;
#if LLVM_VERSION_MAJOR >= 19
    auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
#else
    auto target = llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
#endif
    if (target) {
        llvm::TargetOptions opt;
        std::optional<llvm::Reloc::Model> RM;
#if LLVM_VERSION_MAJOR >= 19
        auto targetMachine = target->createTargetMachine(targetTriple, "generic", "", opt, RM);
#else
        auto targetMachine = target->createTargetMachine(targetTripleStr, "generic", "", opt, RM);
#endif
        if (targetMachine) {
            module->setDataLayout(targetMachine->createDataLayout());
        }
    }
    
    // Configure optimization based on level
    switch (optimizationLevel) {
        case OptimizationLevel::O1:
            // Basic optimizations
            fpm.add(llvm::createInstructionCombiningPass());
            fpm.add(llvm::createReassociatePass());
            fpm.add(llvm::createCFGSimplificationPass());
            break;
            
        case OptimizationLevel::O2:
            // Moderate optimizations
            fpm.add(llvm::createPromoteMemoryToRegisterPass());  // mem2reg
            fpm.add(llvm::createInstructionCombiningPass());     // Instruction combining
            fpm.add(llvm::createReassociatePass());              // Reassociate expressions
            fpm.add(llvm::createGVNPass());                      // Global value numbering
            fpm.add(llvm::createCFGSimplificationPass());        // Simplify CFG
            fpm.add(llvm::createDeadCodeEliminationPass());      // Dead code elimination
            break;
            
        case OptimizationLevel::O3:
            // Aggressive optimizations
            fpm.add(llvm::createPromoteMemoryToRegisterPass());
            fpm.add(llvm::createInstructionCombiningPass());
            fpm.add(llvm::createReassociatePass());
            fpm.add(llvm::createGVNPass());
            fpm.add(llvm::createCFGSimplificationPass());
            fpm.add(llvm::createDeadCodeEliminationPass());
            fpm.add(llvm::createLICMPass());                     // Loop invariant code motion
            fpm.add(llvm::createLoopSimplifyPass());             // Canonicalize loops
            fpm.add(llvm::createLoopUnrollPass());               // Unroll loops
            fpm.add(llvm::createTailCallEliminationPass());      // Tail call elimination
            fpm.add(llvm::createEarlyCSEPass());                 // Early common subexpression elimination
            fpm.add(llvm::createSROAPass());                     // Scalar replacement of aggregates
            break;
            
        case OptimizationLevel::O0:
            // No optimization
            return;
    }
    
    // Run function passes on all functions
    fpm.doInitialization();
    for (auto& func : module->functions()) {
        if (!func.isDeclaration()) {
            fpm.run(func);
        }
    }
    fpm.doFinalization();
    
    // Run module passes  
    mpm.run(*module);
}

void CodeGenerator::optimizeFunction(llvm::Function* func) {
    // Per-function optimization (if needed for specific cases)
    llvm::legacy::FunctionPassManager fpm(module.get());
    
    fpm.add(llvm::createPromoteMemoryToRegisterPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createReassociatePass());
    fpm.add(llvm::createCFGSimplificationPass());
    
    fpm.doInitialization();
    fpm.run(*func);
    fpm.doFinalization();
}

void CodeGenerator::optimizeOptMaxFunctions() {
    llvm::legacy::FunctionPassManager fpm(module.get());
    
    fpm.add(llvm::createPromoteMemoryToRegisterPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createReassociatePass());
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.add(llvm::createDeadCodeEliminationPass());
    fpm.add(llvm::createLICMPass());
    fpm.add(llvm::createLoopStrengthReducePass());
    fpm.add(llvm::createLoopSimplifyPass());
    fpm.add(llvm::createLoopUnrollPass());
    fpm.add(llvm::createTailCallEliminationPass());
    fpm.add(llvm::createEarlyCSEPass());
    fpm.add(llvm::createSROAPass());
    fpm.add(llvm::createConstantHoistingPass());
    fpm.add(llvm::createFlattenCFGPass());
    
    fpm.doInitialization();
    for (auto& func : module->functions()) {
        if (!func.isDeclaration() && optMaxFunctions.count(std::string(func.getName()))) {
            // OPTMAX runs the aggressive pass stack twice to prioritize maximal optimization.
            constexpr int optMaxIterations = 2;  // Second pass catches new foldable patterns after the first.
            for (int i = 0; i < optMaxIterations; ++i) {
                fpm.run(func);
            }
        }
    }
    fpm.doFinalization();
}

void CodeGenerator::writeObjectFile(const std::string& filename) {
    // Initialize only native target
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    
    std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
    module->setTargetTriple(targetTriple);
#else
    module->setTargetTriple(targetTripleStr);
#endif
    
    std::string error;
#if LLVM_VERSION_MAJOR >= 19
    auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
#else
    auto target = llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
#endif
    
    if (!target) {
        throw std::runtime_error("Failed to lookup target: " + error);
    }
    
    auto CPU = "generic";
    auto features = "";
    
    llvm::TargetOptions opt;
    std::optional<llvm::Reloc::Model> RM;
#if LLVM_VERSION_MAJOR >= 19
    auto targetMachine = target->createTargetMachine(targetTriple, CPU, features, opt, RM);
#else
    auto targetMachine = target->createTargetMachine(targetTripleStr, CPU, features, opt, RM);
#endif
    
    module->setDataLayout(targetMachine->createDataLayout());
    
    std::error_code EC;
    llvm::raw_fd_ostream dest(filename, EC, llvm::sys::fs::OF_None);
    
    if (EC) {
        throw std::runtime_error("Could not open file: " + EC.message());
    }
    
    llvm::legacy::PassManager pass;
    auto fileType = llvm::CodeGenFileType::ObjectFile;
    
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        throw std::runtime_error("TargetMachine can't emit a file of this type");
    }
    
    pass.run(*module);
    dest.flush();
}

} // namespace omscript
