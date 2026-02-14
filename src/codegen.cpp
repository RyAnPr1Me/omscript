#include "codegen.h"
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Intrinsics.h>
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
#include <cmath>
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
using omscript::PrefixExpr;
using omscript::TernaryExpr;
using omscript::ExprStmt;
using omscript::VarDecl;
using omscript::ReturnStmt;
using omscript::IfStmt;
using omscript::WhileStmt;
using omscript::DoWhileStmt;
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
        if (op == "~") {
            return std::make_unique<LiteralExpr>(~value);
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

    // Algebraic identity optimizations (one side is a literal)
    if (leftLiteral && leftLiteral->literalType == LiteralExpr::LiteralType::INTEGER) {
        long long lval = leftLiteral->intValue;
        if (lval == 0 && op == "+") return right;            // 0 + x → x
        if (lval == 0 && op == "*") return std::make_unique<LiteralExpr>(0LL); // 0 * x → 0
        if (lval == 1 && op == "*") return right;            // 1 * x → x
        if (lval == 0 && op == "/") return std::make_unique<LiteralExpr>(0LL); // 0 / x → 0
    }
    if (rightLiteral && rightLiteral->literalType == LiteralExpr::LiteralType::INTEGER) {
        long long rval = rightLiteral->intValue;
        if (rval == 0 && op == "+") return left;             // x + 0 → x
        if (rval == 0 && op == "-") return left;             // x - 0 → x
        if (rval == 1 && op == "*") return left;             // x * 1 → x
        if (rval == 0 && op == "*") return std::make_unique<LiteralExpr>(0LL); // x * 0 → 0
        if (rval == 1 && op == "/") return left;             // x / 1 → x
    }

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
        if (op == "&") return std::make_unique<LiteralExpr>(lval & rval);
        if (op == "|") return std::make_unique<LiteralExpr>(lval | rval);
        if (op == "^") return std::make_unique<LiteralExpr>(lval ^ rval);
        if (op == "<<" && rval >= 0 && rval < 64) return std::make_unique<LiteralExpr>(lval << rval);
        if (op == ">>" && rval >= 0 && rval < 64) return std::make_unique<LiteralExpr>(lval >> rval);
    } else if (leftLiteral->literalType == LiteralExpr::LiteralType::FLOAT &&
               rightLiteral->literalType == LiteralExpr::LiteralType::FLOAT) {
        double lval = leftLiteral->floatValue;
        double rval = rightLiteral->floatValue;
        if (op == "+") return std::make_unique<LiteralExpr>(lval + rval);
        if (op == "-") return std::make_unique<LiteralExpr>(lval - rval);
        if (op == "*") return std::make_unique<LiteralExpr>(lval * rval);
        if (op == "/" && rval != 0.0) return std::make_unique<LiteralExpr>(lval / rval);
        if (op == "%" && rval != 0.0) return std::make_unique<LiteralExpr>(std::fmod(lval, rval));
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
        case ASTNodeType::PREFIX_EXPR: {
            auto* prefix = static_cast<PrefixExpr*>(expr.get());
            prefix->operand = optimizeOptMaxExpression(std::move(prefix->operand));
            return expr;
        }
        case ASTNodeType::TERNARY_EXPR: {
            auto* ternary = static_cast<TernaryExpr*>(expr.get());
            ternary->condition = optimizeOptMaxExpression(std::move(ternary->condition));
            ternary->thenExpr = optimizeOptMaxExpression(std::move(ternary->thenExpr));
            ternary->elseExpr = optimizeOptMaxExpression(std::move(ternary->elseExpr));
            // Fold ternary when condition is a compile-time constant
            auto* condLiteral = dynamic_cast<LiteralExpr*>(ternary->condition.get());
            if (condLiteral && condLiteral->literalType == LiteralExpr::LiteralType::INTEGER) {
                return condLiteral->intValue != 0
                    ? std::move(ternary->thenExpr)
                    : std::move(ternary->elseExpr);
            }
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
        case ASTNodeType::DO_WHILE_STMT: {
            auto* doWhileStmt = static_cast<DoWhileStmt*>(stmt);
            optimizeOptMaxStatement(doWhileStmt->body.get());
            doWhileStmt->condition = optimizeOptMaxExpression(std::move(doWhileStmt->condition));
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

// Canonical set of all stdlib built-in function names.
// These functions are always compiled to native machine code via LLVM IR,
// never through the bytecode/dynamic compilation path.
static const std::unordered_set<std::string> stdlibFunctions = {
    "print", "abs", "len", "min", "max", "sign", "clamp", "pow",
    "print_char", "input", "sqrt", "is_even", "is_odd", "sum",
    "swap", "reverse", "to_char", "is_alpha", "is_digit"
};

bool isStdlibFunction(const std::string& name) {
    return stdlibFunctions.count(name) > 0;
}

CodeGenerator::CodeGenerator(OptimizationLevel optLevel) 
    : inOptMaxFunction(false),
      hasOptMaxFunctions(false),
      useDynamicCompilation(false),
      optimizationLevel(optLevel) {
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

void CodeGenerator::codegenError(const std::string& message, const ASTNode* node) {
    if (node && node->line > 0) {
        throw std::runtime_error("Error at line " + std::to_string(node->line) +
                                 ", column " + std::to_string(node->column) + ": " + message);
    }
    throw std::runtime_error(message);
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
        case ASTNodeType::DO_WHILE_STMT:
            generateDoWhile(static_cast<DoWhileStmt*>(stmt));
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
        case ASTNodeType::PREFIX_EXPR:
            return generatePrefix(static_cast<PrefixExpr*>(expr));
        case ASTNodeType::TERNARY_EXPR:
            return generateTernary(static_cast<TernaryExpr*>(expr));
        case ASTNodeType::ARRAY_EXPR:
            return generateArray(static_cast<ArrayExpr*>(expr));
        case ASTNodeType::INDEX_EXPR:
            return generateIndex(static_cast<IndexExpr*>(expr));
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
        // String literal - store the address as an i64 for compatibility with the i64 type system.
        // When passed directly to print(), the pointer form is used instead (see generateCall).
        llvm::Value* strPtr = builder->CreateGlobalString(expr->stringValue, "str");
        return builder->CreatePtrToInt(strPtr, getDefaultType(), "strint");
    }
}

llvm::Value* CodeGenerator::generateIdentifier(IdentifierExpr* expr) {
    auto it = namedValues.find(expr->name);
    if (it == namedValues.end() || !it->second) {
        codegenError("Unknown variable: " + expr->name, expr);
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
        } else if (expr->op == "&") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval & rval));
        } else if (expr->op == "|") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval | rval));
        } else if (expr->op == "^") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval ^ rval));
        } else if (expr->op == "<<") {
            if (rval >= 0 && rval < 64)
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval << rval));
        } else if (expr->op == ">>") {
            if (rval >= 0 && rval < 64)
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval >> rval));
        }
    }
    
    // Regular code generation for non-constant expressions
    if (expr->op == "+") {
        return builder->CreateAdd(left, right, "addtmp");
    } else if (expr->op == "-") {
        return builder->CreateSub(left, right, "subtmp");
    } else if (expr->op == "*") {
        return builder->CreateMul(left, right, "multmp");
    } else if (expr->op == "/" || expr->op == "%") {
        bool isDivision = expr->op == "/";
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* isZero = builder->CreateICmpEQ(right, zero, "divzero");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        const char* zeroName = isDivision ? "div.zero" : "mod.zero";
        const char* opName = isDivision ? "div.op" : "mod.op";
        llvm::BasicBlock* zeroBB = llvm::BasicBlock::Create(*context, zeroName, function);
        llvm::BasicBlock* opBB = llvm::BasicBlock::Create(*context, opName, function);
        builder->CreateCondBr(isZero, zeroBB, opBB);

        builder->SetInsertPoint(zeroBB);
        const char* messageText = isDivision ? "Runtime error: division by zero\n"
                                             : "Runtime error: modulo by zero\n";
        const char* messageName = isDivision ? "divzero_msg" : "modzero_msg";
        llvm::Value* message = builder->CreateGlobalString(messageText, messageName);
        builder->CreateCall(getPrintfFunction(), {message});
        llvm::Function* exitFunction = module->getFunction("exit");
        if (!exitFunction) {
            auto exitType = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                                                    {llvm::Type::getInt32Ty(*context)},
                                                    false);
            exitFunction = llvm::Function::Create(exitType,
                                                  llvm::Function::ExternalLinkage,
                                                  "exit",
                                                  module.get());
        }
        builder->CreateCall(exitFunction, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1)});
        builder->CreateUnreachable();

        builder->SetInsertPoint(opBB);
        return isDivision ? builder->CreateSDiv(left, right, "divtmp")
                          : builder->CreateSRem(left, right, "modtmp");
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
    } else if (expr->op == "&") {
        return builder->CreateAnd(left, right, "andtmp");
    } else if (expr->op == "|") {
        return builder->CreateOr(left, right, "ortmp");
    } else if (expr->op == "^") {
        return builder->CreateXor(left, right, "xortmp");
    } else if (expr->op == "<<") {
        return builder->CreateShl(left, right, "shltmp");
    } else if (expr->op == ">>") {
        return builder->CreateAShr(left, right, "ashrtmp");
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
    } else if (expr->op == "~") {
        return builder->CreateNot(operand, "bitnottmp");
    }
    
    throw std::runtime_error("Unknown unary operator: " + expr->op);
}

llvm::Value* CodeGenerator::generateCall(CallExpr* expr) {
    // All stdlib built-in functions are compiled to native machine code below.
    // They never use dynamic variables or the bytecode path.
    if (expr->callee == "print") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'print' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        // Check if the argument is a string literal to print it with %s
        Expression* argExpr = expr->arguments[0].get();
        if (argExpr->type == ASTNodeType::LITERAL_EXPR) {
            auto* litExpr = static_cast<LiteralExpr*>(argExpr);
            if (litExpr->literalType == LiteralExpr::LiteralType::STRING) {
                llvm::Value* strPtr = builder->CreateGlobalString(litExpr->stringValue, "printstr");
                llvm::GlobalVariable* strFmt = module->getGlobalVariable("print_str_fmt", true);
                if (!strFmt) {
                    strFmt = builder->CreateGlobalString("%s\n", "print_str_fmt");
                }
                builder->CreateCall(getPrintfFunction(), {strFmt, strPtr});
                return llvm::ConstantInt::get(getDefaultType(), 0);
            }
        }
        llvm::Value* arg = generateExpression(argExpr);
        llvm::GlobalVariable* formatStr = module->getGlobalVariable("print_fmt", true);
        if (!formatStr) {
            formatStr = builder->CreateGlobalString("%lld\n", "print_fmt");
        }
        builder->CreateCall(getPrintfFunction(), {formatStr, arg});
        return arg;
    }

    if (expr->callee == "abs") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'abs' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // abs(x) = x >= 0 ? x : -x
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* isNeg = builder->CreateICmpSLT(arg, zero, "isneg");
        llvm::Value* negVal = builder->CreateNeg(arg, "negval");
        return builder->CreateSelect(isNeg, negVal, arg, "absval");
    }

    if (expr->callee == "len") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'len' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // Array is stored as an i64 holding a pointer to [length, elem0, elem1, ...]
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg,
            llvm::PointerType::getUnqual(*context), "arrptr");
        return builder->CreateLoad(getDefaultType(), arrPtr, "arrlen");
    }

    if (expr->callee == "min") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'min' expects 2 arguments, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        llvm::Value* cmp = builder->CreateICmpSLT(a, b, "mincmp");
        return builder->CreateSelect(cmp, a, b, "minval");
    }

    if (expr->callee == "max") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'max' expects 2 arguments, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        llvm::Value* cmp = builder->CreateICmpSGT(a, b, "maxcmp");
        return builder->CreateSelect(cmp, a, b, "maxval");
    }

    if (expr->callee == "sign") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sign' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* pos = llvm::ConstantInt::get(getDefaultType(), 1, true);
        llvm::Value* neg = llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(-1), true);
        llvm::Value* isNeg = builder->CreateICmpSLT(x, zero, "signneg");
        llvm::Value* negOrZero = builder->CreateSelect(isNeg, neg, zero, "signNZ");
        llvm::Value* isPos = builder->CreateICmpSGT(x, zero, "signpos");
        return builder->CreateSelect(isPos, pos, negOrZero, "signval");
    }

    if (expr->callee == "clamp") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'clamp' expects 3 arguments, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        llvm::Value* lo = generateExpression(expr->arguments[1].get());
        llvm::Value* hi = generateExpression(expr->arguments[2].get());
        // clamp(val, lo, hi) = max(lo, min(val, hi))
        llvm::Value* cmpHi = builder->CreateICmpSLT(val, hi, "clamphi");
        llvm::Value* minVH = builder->CreateSelect(cmpHi, val, hi, "clampmin");
        llvm::Value* cmpLo = builder->CreateICmpSGT(minVH, lo, "clamplo");
        return builder->CreateSelect(cmpLo, minVH, lo, "clampval");
    }

    if (expr->callee == "pow") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'pow' expects 2 arguments, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* base = generateExpression(expr->arguments[0].get());
        llvm::Value* exp = generateExpression(expr->arguments[1].get());

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "pow.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "pow.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "pow.done", function);

        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "pow.result");
        llvm::PHINode* counter = builder->CreatePHI(getDefaultType(), 2, "pow.counter");
        result->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), entryBB);
        counter->addIncoming(exp, entryBB);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* done = builder->CreateICmpSLE(counter, zero, "pow.done.cmp");
        builder->CreateCondBr(done, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* newResult = builder->CreateMul(result, base, "pow.mul");
        llvm::Value* newCounter = builder->CreateSub(counter,
            llvm::ConstantInt::get(getDefaultType(), 1), "pow.dec");
        result->addIncoming(newResult, bodyBB);
        counter->addIncoming(newCounter, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return result;
    }

    if (expr->callee == "print_char") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'print_char' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Function* putcharFn = module->getFunction("putchar");
        if (!putcharFn) {
            auto putcharType = llvm::FunctionType::get(
                llvm::Type::getInt32Ty(*context),
                {llvm::Type::getInt32Ty(*context)},
                false);
            putcharFn = llvm::Function::Create(putcharType,
                llvm::Function::ExternalLinkage, "putchar", module.get());
        }
        llvm::Value* truncated = builder->CreateTrunc(arg, llvm::Type::getInt32Ty(*context), "charval");
        builder->CreateCall(putcharFn, {truncated});
        return arg;
    }

    if (expr->callee == "input") {
        if (!expr->arguments.empty()) {
            codegenError("Built-in function 'input' expects 0 arguments, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Function* scanfFn = module->getFunction("scanf");
        if (!scanfFn) {
            auto scanfType = llvm::FunctionType::get(
                llvm::Type::getInt32Ty(*context),
                {llvm::PointerType::getUnqual(*context)},
                true);
            scanfFn = llvm::Function::Create(scanfType,
                llvm::Function::ExternalLinkage, "scanf", module.get());
        }
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::AllocaInst* inputAlloca = createEntryBlockAlloca(function, "input_val");
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), inputAlloca);
        llvm::GlobalVariable* scanfFmt = module->getGlobalVariable("scanf_fmt", true);
        if (!scanfFmt) {
            scanfFmt = builder->CreateGlobalString("%lld", "scanf_fmt");
        }
        builder->CreateCall(scanfFn, {scanfFmt, inputAlloca});
        return builder->CreateLoad(getDefaultType(), inputAlloca, "input_read");
    }

    if (expr->callee == "sqrt") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sqrt' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Integer square root via Newton's method: guess = x, while (guess*guess > x) guess = (guess + x/guess) / 2
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "sqrt.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "sqrt.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "sqrt.done", function);

        // Handle x <= 0: return 0
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* isNonPositive = builder->CreateICmpSLE(x, zero, "sqrt.nonpos");
        llvm::BasicBlock* positiveBB = llvm::BasicBlock::Create(*context, "sqrt.positive", function);
        builder->CreateCondBr(isNonPositive, doneBB, positiveBB);

        builder->SetInsertPoint(positiveBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* guess = builder->CreatePHI(getDefaultType(), 2, "sqrt.guess");
        guess->addIncoming(x, positiveBB);

        llvm::Value* sq = builder->CreateMul(guess, guess, "sqrt.sq");
        llvm::Value* tooBig = builder->CreateICmpSGT(sq, x, "sqrt.toobig");
        builder->CreateCondBr(tooBig, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* div = builder->CreateSDiv(x, guess, "sqrt.div");
        llvm::Value* sum = builder->CreateAdd(guess, div, "sqrt.sum");
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);
        llvm::Value* newGuess = builder->CreateSDiv(sum, two, "sqrt.newguess");
        // Ensure progress: if newGuess >= guess, force guess - 1
        llvm::Value* noProgress = builder->CreateICmpSGE(newGuess, guess, "sqrt.noprogress");
        llvm::Value* forcedGuess = builder->CreateSub(guess, llvm::ConstantInt::get(getDefaultType(), 1), "sqrt.forced");
        llvm::Value* nextGuess = builder->CreateSelect(noProgress, forcedGuess, newGuess, "sqrt.next");
        guess->addIncoming(nextGuess, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "sqrt.result");
        result->addIncoming(zero, entryBB);
        result->addIncoming(guess, loopBB);
        return result;
    }

    if (expr->callee == "is_even") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_even' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* bit = builder->CreateAnd(x, one, "evenbit");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isEven = builder->CreateICmpEQ(bit, zero, "iseven");
        return builder->CreateZExt(isEven, getDefaultType(), "evenval");
    }

    if (expr->callee == "is_odd") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_odd' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        return builder->CreateAnd(x, one, "oddval");
    }

    if (expr->callee == "sum") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sum' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // Array layout: [length, elem0, elem1, ...]
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg,
            llvm::PointerType::getUnqual(*context), "sum.arrptr");
        llvm::Value* length = builder->CreateLoad(getDefaultType(), arrPtr, "sum.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "sum.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "sum.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "sum.done", function);

        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::PHINode* acc = builder->CreatePHI(getDefaultType(), 2, "sum.acc");
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "sum.idx");
        acc->addIncoming(zero, entryBB);
        idx->addIncoming(zero, entryBB);

        llvm::Value* done = builder->CreateICmpSGE(idx, length, "sum.done");
        builder->CreateCondBr(done, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        // Element is at offset (idx + 1) from array base
        llvm::Value* offset = builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 1), "sum.offset");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, offset, "sum.elemptr");
        llvm::Value* elem = builder->CreateLoad(getDefaultType(), elemPtr, "sum.elem");
        llvm::Value* newAcc = builder->CreateAdd(acc, elem, "sum.newacc");
        llvm::Value* newIdx = builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 1), "sum.newidx");
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(newIdx, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return acc;
    }

    if (expr->callee == "swap") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'swap' expects 3 arguments (array, i, j), but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* i = generateExpression(expr->arguments[1].get());
        llvm::Value* j = generateExpression(expr->arguments[2].get());

        llvm::Value* arrPtr = builder->CreateIntToPtr(arg,
            llvm::PointerType::getUnqual(*context), "swap.arrptr");
        // Elements are at offset (index + 1)
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* offI = builder->CreateAdd(i, one, "swap.offi");
        llvm::Value* offJ = builder->CreateAdd(j, one, "swap.offj");
        llvm::Value* ptrI = builder->CreateGEP(getDefaultType(), arrPtr, offI, "swap.ptri");
        llvm::Value* ptrJ = builder->CreateGEP(getDefaultType(), arrPtr, offJ, "swap.ptrj");
        llvm::Value* valI = builder->CreateLoad(getDefaultType(), ptrI, "swap.vali");
        llvm::Value* valJ = builder->CreateLoad(getDefaultType(), ptrJ, "swap.valj");
        builder->CreateStore(valJ, ptrI);
        builder->CreateStore(valI, ptrJ);
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (expr->callee == "reverse") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'reverse' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg,
            llvm::PointerType::getUnqual(*context), "rev.arrptr");
        llvm::Value* length = builder->CreateLoad(getDefaultType(), arrPtr, "rev.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "rev.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "rev.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "rev.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* lastIdx = builder->CreateSub(length, one, "rev.last");
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* lo = builder->CreatePHI(getDefaultType(), 2, "rev.lo");
        llvm::PHINode* hi = builder->CreatePHI(getDefaultType(), 2, "rev.hi");
        lo->addIncoming(zero, entryBB);
        hi->addIncoming(lastIdx, entryBB);

        llvm::Value* done = builder->CreateICmpSGE(lo, hi, "rev.done");
        builder->CreateCondBr(done, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* offLo = builder->CreateAdd(lo, one, "rev.offlo");
        llvm::Value* offHi = builder->CreateAdd(hi, one, "rev.offhi");
        llvm::Value* ptrLo = builder->CreateGEP(getDefaultType(), arrPtr, offLo, "rev.ptrlo");
        llvm::Value* ptrHi = builder->CreateGEP(getDefaultType(), arrPtr, offHi, "rev.ptrhi");
        llvm::Value* valLo = builder->CreateLoad(getDefaultType(), ptrLo, "rev.vallo");
        llvm::Value* valHi = builder->CreateLoad(getDefaultType(), ptrHi, "rev.valhi");
        builder->CreateStore(valHi, ptrLo);
        builder->CreateStore(valLo, ptrHi);
        llvm::Value* newLo = builder->CreateAdd(lo, one, "rev.newlo");
        llvm::Value* newHi = builder->CreateSub(hi, one, "rev.newhi");
        lo->addIncoming(newLo, bodyBB);
        hi->addIncoming(newHi, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return arg;
    }

    if (expr->callee == "to_char") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'to_char' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        // Returns the value itself - the integer IS the character code
        return generateExpression(expr->arguments[0].get());
    }

    if (expr->callee == "is_alpha") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_alpha' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // is_alpha: (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z')
        llvm::Value* geA = builder->CreateICmpSGE(x, llvm::ConstantInt::get(getDefaultType(), 65), "ge.A");
        llvm::Value* leZ = builder->CreateICmpSLE(x, llvm::ConstantInt::get(getDefaultType(), 90), "le.Z");
        llvm::Value* upper = builder->CreateAnd(geA, leZ, "isupper");
        llvm::Value* gea = builder->CreateICmpSGE(x, llvm::ConstantInt::get(getDefaultType(), 97), "ge.a");
        llvm::Value* lez = builder->CreateICmpSLE(x, llvm::ConstantInt::get(getDefaultType(), 122), "le.z");
        llvm::Value* lower = builder->CreateAnd(gea, lez, "islower");
        llvm::Value* isAlpha = builder->CreateOr(upper, lower, "isalpha");
        return builder->CreateZExt(isAlpha, getDefaultType(), "alphaval");
    }

    if (expr->callee == "is_digit") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_digit' expects 1 argument, but " +
                         std::to_string(expr->arguments.size()) + " provided", expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // is_digit: x >= '0' && x <= '9'
        llvm::Value* ge0 = builder->CreateICmpSGE(x, llvm::ConstantInt::get(getDefaultType(), 48), "ge.0");
        llvm::Value* le9 = builder->CreateICmpSLE(x, llvm::ConstantInt::get(getDefaultType(), 57), "le.9");
        llvm::Value* isDigit = builder->CreateAnd(ge0, le9, "isdigit");
        return builder->CreateZExt(isDigit, getDefaultType(), "digitval");
    }

    if (inOptMaxFunction) {
        // Stdlib functions are always native machine code, so they're safe to call from OPTMAX
        if (!isStdlibFunction(expr->callee) &&
            optMaxFunctions.find(expr->callee) == optMaxFunctions.end()) {
            std::string currentFunction = "<unknown>";
            if (builder->GetInsertBlock() && builder->GetInsertBlock()->getParent()) {
                currentFunction = std::string(builder->GetInsertBlock()->getParent()->getName());
            }
            throw std::runtime_error("OPTMAX function \"" + currentFunction +
                                     "\" cannot invoke non-OPTMAX function \"" +
                                     expr->callee + "\"");
        }
    }
    auto calleeIt = functions.find(expr->callee);
    if (calleeIt == functions.end() || !calleeIt->second) {
        codegenError("Unknown function: " + expr->callee, expr);
    }
    llvm::Function* callee = calleeIt->second;
    
    if (callee->arg_size() != expr->arguments.size()) {
        codegenError("Function '" + expr->callee + "' expects " +
                     std::to_string(callee->arg_size()) + " argument(s), but " +
                     std::to_string(expr->arguments.size()) + " provided", expr);
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
        codegenError("Unknown variable: " + expr->name, expr);
    }
    checkConstModification(expr->name, "modify");
    
    builder->CreateStore(value, it->second);
    return value;
}

llvm::Value* CodeGenerator::generatePostfix(PostfixExpr* expr) {
    auto* identifier = dynamic_cast<IdentifierExpr*>(expr->operand.get());
    if (!identifier) {
        codegenError("Postfix operators require an identifier", expr);
    }
    
    auto it = namedValues.find(identifier->name);
    if (it == namedValues.end() || !it->second) {
        codegenError("Unknown variable: " + identifier->name, expr);
    }
    checkConstModification(identifier->name, "modify");
    
    llvm::Value* current = builder->CreateLoad(getDefaultType(), it->second, identifier->name.c_str());
    llvm::Value* delta = llvm::ConstantInt::get(getDefaultType(), 1, true);
    if (expr->op != "++" && expr->op != "--") {
        codegenError("Unknown postfix operator: " + expr->op, expr);
    }
    llvm::Value* updated = (expr->op == "++")
        ? builder->CreateAdd(current, delta, "postinc")
        : builder->CreateSub(current, delta, "postdec");
    
    builder->CreateStore(updated, it->second);
    return current;
}

llvm::Value* CodeGenerator::generatePrefix(PrefixExpr* expr) {
    auto* identifier = dynamic_cast<IdentifierExpr*>(expr->operand.get());
    if (!identifier) {
        codegenError("Prefix operators require an identifier", expr);
    }
    
    auto it = namedValues.find(identifier->name);
    if (it == namedValues.end() || !it->second) {
        codegenError("Unknown variable: " + identifier->name, expr);
    }
    checkConstModification(identifier->name, "modify");
    
    llvm::Value* current = builder->CreateLoad(getDefaultType(), it->second, identifier->name.c_str());
    llvm::Value* delta = llvm::ConstantInt::get(getDefaultType(), 1, true);
    if (expr->op != "++" && expr->op != "--") {
        codegenError("Unknown prefix operator: " + expr->op, expr);
    }
    llvm::Value* updated = (expr->op == "++")
        ? builder->CreateAdd(current, delta, "preinc")
        : builder->CreateSub(current, delta, "predec");
    
    builder->CreateStore(updated, it->second);
    return updated;
}

llvm::Value* CodeGenerator::generateTernary(TernaryExpr* expr) {
    llvm::Value* condition = generateExpression(expr->condition.get());
    llvm::Value* condBool = builder->CreateICmpNE(
        condition,
        llvm::ConstantInt::get(getDefaultType(), 0, true),
        "terncond"
    );
    
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(*context, "tern.then", function);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(*context, "tern.else", function);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "tern.cont", function);
    
    builder->CreateCondBr(condBool, thenBB, elseBB);
    
    builder->SetInsertPoint(thenBB);
    llvm::Value* thenVal = generateExpression(expr->thenExpr.get());
    builder->CreateBr(mergeBB);
    thenBB = builder->GetInsertBlock();
    
    builder->SetInsertPoint(elseBB);
    llvm::Value* elseVal = generateExpression(expr->elseExpr.get());
    builder->CreateBr(mergeBB);
    elseBB = builder->GetInsertBlock();
    
    builder->SetInsertPoint(mergeBB);
    llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "ternval");
    phi->addIncoming(thenVal, thenBB);
    phi->addIncoming(elseVal, elseBB);
    
    return phi;
}

llvm::Value* CodeGenerator::generateArray(ArrayExpr* expr) {
    size_t numElements = expr->elements.size();
    // Allocate space for (1 + numElements) i64 slots: [length, elem0, elem1, ...]
    size_t totalSlots = 1 + numElements;
    llvm::Value* allocSize = llvm::ConstantInt::get(getDefaultType(), totalSlots);
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
    llvm::AllocaInst* arrAlloca = entryBuilder.CreateAlloca(getDefaultType(), allocSize, "arr");

    // Store the length in slot 0 (arrAlloca points to slot 0)
    builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), numElements), arrAlloca);

    // Store each element in slots 1..N
    for (size_t i = 0; i < numElements; i++) {
        llvm::Value* elemVal = generateExpression(expr->elements[i].get());
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrAlloca,
            llvm::ConstantInt::get(getDefaultType(), i + 1), "arr.elem.ptr");
        builder->CreateStore(elemVal, elemPtr);
    }

    // Return the array pointer as an i64 for the dynamic type system
    return builder->CreatePtrToInt(arrAlloca, getDefaultType(), "arr.int");
}

llvm::Value* CodeGenerator::generateIndex(IndexExpr* expr) {
    llvm::Value* arrVal = generateExpression(expr->array.get());
    llvm::Value* idxVal = generateExpression(expr->index.get());

    // Convert the i64 back to a pointer
    llvm::Value* arrPtr = builder->CreateIntToPtr(arrVal,
        llvm::PointerType::getUnqual(*context), "idx.arrptr");

    // Bounds check: load length from slot 0, verify 0 <= index < length
    llvm::Value* lenVal = builder->CreateLoad(getDefaultType(), arrPtr, "idx.len");
    llvm::Value* inBounds = builder->CreateICmpSLT(idxVal, lenVal, "idx.inbounds");
    llvm::Value* notNeg = builder->CreateICmpSGE(idxVal,
        llvm::ConstantInt::get(getDefaultType(), 0), "idx.notneg");
    llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "idx.valid");

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "idx.ok", function);
    llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "idx.fail", function);

    builder->CreateCondBr(valid, okBB, failBB);

    // Out-of-bounds path: print error and abort
    builder->SetInsertPoint(failBB);
    llvm::Value* errMsg = builder->CreateGlobalString(
        "Runtime error: array index out of bounds\n", "idx_oob_msg");
    builder->CreateCall(getPrintfFunction(), {errMsg});
    llvm::Function* trapFn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::trap);
    builder->CreateCall(trapFn, {});
    builder->CreateUnreachable();

    // Success path: load element at offset (index + 1)
    builder->SetInsertPoint(okBB);
    llvm::Value* offset = builder->CreateAdd(idxVal,
        llvm::ConstantInt::get(getDefaultType(), 1), "idx.offset");
    llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, offset, "idx.elem.ptr");
    return builder->CreateLoad(getDefaultType(), elemPtr, "idx.elem");
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
    
    beginScope();
    
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
    
    endScope();
}

void CodeGenerator::generateDoWhile(DoWhileStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    
    beginScope();
    
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "dowhilebody", function);
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "dowhilecond", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "dowhileend", function);
    
    // Jump directly to body (execute at least once)
    builder->CreateBr(bodyBB);
    
    // Body block
    builder->SetInsertPoint(bodyBB);
    loopStack.push_back({endBB, condBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(condBB);
    }
    
    // Condition block
    builder->SetInsertPoint(condBB);
    llvm::Value* condition = generateExpression(stmt->condition.get());
    llvm::Value* condBool = builder->CreateICmpNE(
        condition,
        llvm::ConstantInt::get(getDefaultType(), 0, true),
        "dowhilecond"
    );
    builder->CreateCondBr(condBool, bodyBB, endBB);
    
    // End block
    builder->SetInsertPoint(endBB);
    
    endScope();
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
        stepVal = generateExpression(stmt->step.get());
    } else {
        stepVal = llvm::ConstantInt::get(*context, llvm::APInt(64, 1));
    }
    
    llvm::Value* zero = llvm::ConstantInt::get(stepVal->getType(), 0, true);
    
    // Create blocks
    llvm::BasicBlock* stepCheckBB = llvm::BasicBlock::Create(*context, "forstepcheck", function);
    llvm::BasicBlock* stepFailBB = llvm::BasicBlock::Create(*context, "forstepfail", function);
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "forcond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "forbody", function);
    llvm::BasicBlock* incBB = llvm::BasicBlock::Create(*context, "forinc", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "forend", function);
    
    builder->CreateBr(stepCheckBB);
    
    builder->SetInsertPoint(stepCheckBB);
    llvm::Value* stepNonZero = builder->CreateICmpNE(stepVal, zero, "stepnonzero");
    builder->CreateCondBr(stepNonZero, condBB, stepFailBB);
    
    builder->SetInsertPoint(stepFailBB);
    std::string errorMessage = "Runtime error: for-loop step cannot be zero for iterator '" +
                               stmt->iteratorVar + "'\n";
    llvm::GlobalVariable* messageVar = builder->CreateGlobalString(errorMessage, "forstepmsg");
    llvm::Constant* zeroIndex = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0);
    llvm::Constant* indices[] = {zeroIndex, zeroIndex};
    llvm::Constant* message = llvm::ConstantExpr::getInBoundsGetElementPtr(
        messageVar->getValueType(),
        messageVar,
        indices);
    builder->CreateCall(getPrintfFunction(), {message});
#if LLVM_VERSION_MAJOR >= 19
    llvm::FunctionCallee trap = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::trap);
#else
    llvm::FunctionCallee trap = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::trap);
#endif
    builder->CreateCall(trap);
    builder->CreateUnreachable();
    
    // Condition block: check if iterator < end (forward) or > end (backward)
    builder->SetInsertPoint(condBB);
    llvm::Value* curVal = builder->CreateLoad(getDefaultType(), iterAlloca, stmt->iteratorVar.c_str());
    llvm::Value* stepPositive = builder->CreateICmpSGT(stepVal, zero, "steppositive");
    llvm::Value* forwardCond = builder->CreateICmpSLT(curVal, endVal, "forcond_lt");
    llvm::Value* backwardCond = builder->CreateICmpSGT(curVal, endVal, "forcond_gt");
    llvm::Value* continueCond = builder->CreateSelect(stepPositive, forwardCond, backwardCond, "forcond_range");
    builder->CreateCondBr(continueCond, bodyBB, endBB);
    
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
        // Use PIC to support default PIE linking on modern toolchains.
        std::optional<llvm::Reloc::Model> RM = llvm::Reloc::PIC_;
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
            fpm.add(llvm::createSROAPass());                     // Scalar replacement of aggregates (early)
            fpm.add(llvm::createEarlyCSEPass());                 // Early common subexpression elimination
            fpm.add(llvm::createPromoteMemoryToRegisterPass());
            fpm.add(llvm::createInstructionCombiningPass());
            fpm.add(llvm::createReassociatePass());
            fpm.add(llvm::createGVNPass());
            fpm.add(llvm::createCFGSimplificationPass());
            fpm.add(llvm::createDeadCodeEliminationPass());
            fpm.add(llvm::createLICMPass());                     // Loop invariant code motion
            fpm.add(llvm::createLoopRotatePass());               // Rotate loops for better optimization
            fpm.add(llvm::createLoopSimplifyPass());             // Canonicalize loops
            fpm.add(llvm::createLoopInstSimplifyPass());         // Simplify instructions in loops
            fpm.add(llvm::createLoopUnrollPass());               // Unroll loops
            fpm.add(llvm::createSinkingPass());                  // Sink instructions closer to use
            fpm.add(llvm::createMergedLoadStoreMotionPass());    // Merge load/store across diamonds
            fpm.add(llvm::createStraightLineStrengthReducePass()); // Strength reduce along straight lines
            fpm.add(llvm::createNaryReassociatePass());          // N-ary reassociation
            fpm.add(llvm::createTailCallEliminationPass());      // Tail call elimination
            // Second round of cleanup after loop and strength reduction passes
            fpm.add(llvm::createInstructionCombiningPass());
            fpm.add(llvm::createCFGSimplificationPass());
            fpm.add(llvm::createDeadCodeEliminationPass());
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
    
    // Phase 1: Early canonicalization
    fpm.add(llvm::createSROAPass());
    fpm.add(llvm::createEarlyCSEPass());
    fpm.add(llvm::createPromoteMemoryToRegisterPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createReassociatePass());
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.add(llvm::createDeadCodeEliminationPass());
    // Phase 2: Loop optimizations
    fpm.add(llvm::createLICMPass());
    fpm.add(llvm::createLoopRotatePass());
    fpm.add(llvm::createLoopSimplifyPass());
    fpm.add(llvm::createLoopInstSimplifyPass());
    fpm.add(llvm::createLoopStrengthReducePass());
    fpm.add(llvm::createLoopUnrollPass());
    // Phase 3: Post-loop optimizations
    fpm.add(llvm::createSinkingPass());
    fpm.add(llvm::createMergedLoadStoreMotionPass());
    fpm.add(llvm::createStraightLineStrengthReducePass());
    fpm.add(llvm::createNaryReassociatePass());
    fpm.add(llvm::createTailCallEliminationPass());
    fpm.add(llvm::createConstantHoistingPass());
    fpm.add(llvm::createFlattenCFGPass());
    // Phase 4: Final cleanup
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.add(llvm::createDeadCodeEliminationPass());
    
    fpm.doInitialization();
    for (auto& func : module->functions()) {
        if (!func.isDeclaration() && optMaxFunctions.count(std::string(func.getName()))) {
            // OPTMAX runs the aggressive pass stack three times to maximize optimization.
            // Each iteration can expose new patterns for subsequent passes to simplify.
            // Three iterations is the sweet spot: the first pass does heavy lifting,
            // the second catches patterns exposed by loop/strength-reduce transforms,
            // and the third cleans up residuals.  Beyond three, passes reach a fixed
            // point and additional iterations produce no further changes.
            constexpr int optMaxIterations = 3;
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
    // Use PIC to support default PIE linking on modern toolchains.
    std::optional<llvm::Reloc::Model> RM = llvm::Reloc::PIC_;
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
#if LLVM_VERSION_MAJOR >= 18
    auto fileType = llvm::CodeGenFileType::ObjectFile;
#else
    auto fileType = llvm::CGFT_ObjectFile;
#endif
    
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        throw std::runtime_error("TargetMachine can't emit a file of this type");
    }
    
    pass.run(*module);
    dest.flush();
}

void CodeGenerator::generateBytecode(Program* program) {
    for (auto& func : program->functions) {
        if (func->name == "main") {
            emitBytecodeBlock(func->body.get());
            bytecodeEmitter.emit(OpCode::RETURN);
        }
    }
    bytecodeEmitter.emit(OpCode::HALT);
}

void CodeGenerator::emitBytecodeExpression(Expression* expr) {
    switch (expr->type) {
        case ASTNodeType::LITERAL_EXPR: {
            auto* lit = static_cast<LiteralExpr*>(expr);
            if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
                bytecodeEmitter.emit(OpCode::PUSH_INT);
                bytecodeEmitter.emitInt(lit->intValue);
            } else if (lit->literalType == LiteralExpr::LiteralType::FLOAT) {
                bytecodeEmitter.emit(OpCode::PUSH_FLOAT);
                bytecodeEmitter.emitFloat(lit->floatValue);
            } else if (lit->literalType == LiteralExpr::LiteralType::STRING) {
                bytecodeEmitter.emit(OpCode::PUSH_STRING);
                bytecodeEmitter.emitString(lit->stringValue);
            }
            break;
        }
        case ASTNodeType::IDENTIFIER_EXPR: {
            auto* id = static_cast<IdentifierExpr*>(expr);
            bytecodeEmitter.emit(OpCode::LOAD_VAR);
            bytecodeEmitter.emitString(id->name);
            break;
        }
        case ASTNodeType::BINARY_EXPR: {
            auto* bin = static_cast<BinaryExpr*>(expr);
            emitBytecodeExpression(bin->left.get());
            emitBytecodeExpression(bin->right.get());
            if (bin->op == "+") bytecodeEmitter.emit(OpCode::ADD);
            else if (bin->op == "-") bytecodeEmitter.emit(OpCode::SUB);
            else if (bin->op == "*") bytecodeEmitter.emit(OpCode::MUL);
            else if (bin->op == "/") bytecodeEmitter.emit(OpCode::DIV);
            else if (bin->op == "%") bytecodeEmitter.emit(OpCode::MOD);
            else if (bin->op == "==") bytecodeEmitter.emit(OpCode::EQ);
            else if (bin->op == "!=") bytecodeEmitter.emit(OpCode::NE);
            else if (bin->op == "<") bytecodeEmitter.emit(OpCode::LT);
            else if (bin->op == "<=") bytecodeEmitter.emit(OpCode::LE);
            else if (bin->op == ">") bytecodeEmitter.emit(OpCode::GT);
            else if (bin->op == ">=") bytecodeEmitter.emit(OpCode::GE);
            else if (bin->op == "&&") bytecodeEmitter.emit(OpCode::AND);
            else if (bin->op == "||") bytecodeEmitter.emit(OpCode::OR);
            else {
                throw std::runtime_error("Unsupported binary operator in bytecode: " + bin->op);
            }
            break;
        }
        case ASTNodeType::UNARY_EXPR: {
            auto* unary = static_cast<UnaryExpr*>(expr);
            emitBytecodeExpression(unary->operand.get());
            if (unary->op == "-") bytecodeEmitter.emit(OpCode::NEG);
            else if (unary->op == "!") bytecodeEmitter.emit(OpCode::NOT);
            else {
                throw std::runtime_error("Unsupported unary operator in bytecode: " + unary->op);
            }
            break;
        }
        case ASTNodeType::ASSIGN_EXPR: {
            auto* assign = static_cast<AssignExpr*>(expr);
            emitBytecodeExpression(assign->value.get());
            bytecodeEmitter.emit(OpCode::STORE_VAR);
            bytecodeEmitter.emitString(assign->name);
            break;
        }
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<CallExpr*>(expr);
            if (isStdlibFunction(call->callee)) {
                // Stdlib functions are compiled to native machine code only.
                // They are not available in the bytecode interpreter.
                throw std::runtime_error("Stdlib function '" + call->callee +
                    "' must be compiled to native code, not bytecode");
            }
            for (auto& arg : call->arguments) {
                emitBytecodeExpression(arg.get());
            }
            bytecodeEmitter.emit(OpCode::CALL);
            bytecodeEmitter.emitString(call->callee);
            bytecodeEmitter.emitByte(static_cast<uint8_t>(call->arguments.size()));
            break;
        }
        case ASTNodeType::POSTFIX_EXPR: {
            auto* postfix = static_cast<PostfixExpr*>(expr);
            auto* id = dynamic_cast<IdentifierExpr*>(postfix->operand.get());
            if (!id) {
                throw std::runtime_error("Postfix operator requires an identifier in bytecode");
            }
            // Push the current value (return value for postfix)
            bytecodeEmitter.emit(OpCode::LOAD_VAR);
            bytecodeEmitter.emitString(id->name);
            // Compute new value: load, push 1, add/sub, store
            bytecodeEmitter.emit(OpCode::LOAD_VAR);
            bytecodeEmitter.emitString(id->name);
            bytecodeEmitter.emit(OpCode::PUSH_INT);
            bytecodeEmitter.emitInt(1);
            if (postfix->op == "++") {
                bytecodeEmitter.emit(OpCode::ADD);
            } else {
                bytecodeEmitter.emit(OpCode::SUB);
            }
            bytecodeEmitter.emit(OpCode::STORE_VAR);
            bytecodeEmitter.emitString(id->name);
            bytecodeEmitter.emit(OpCode::POP); // STORE_VAR left the new value on top; discard it to expose the original value below
            break;
        }
        case ASTNodeType::PREFIX_EXPR: {
            auto* prefix = static_cast<PrefixExpr*>(expr);
            auto* id = dynamic_cast<IdentifierExpr*>(prefix->operand.get());
            if (!id) {
                throw std::runtime_error("Prefix operator requires an identifier in bytecode");
            }
            // Compute new value: load, push 1, add/sub, store
            bytecodeEmitter.emit(OpCode::LOAD_VAR);
            bytecodeEmitter.emitString(id->name);
            bytecodeEmitter.emit(OpCode::PUSH_INT);
            bytecodeEmitter.emitInt(1);
            if (prefix->op == "++") {
                bytecodeEmitter.emit(OpCode::ADD);
            } else {
                bytecodeEmitter.emit(OpCode::SUB);
            }
            bytecodeEmitter.emit(OpCode::STORE_VAR);
            bytecodeEmitter.emitString(id->name);
            // STORE_VAR leaves the new value on the stack (prefix returns new value)
            break;
        }
        case ASTNodeType::TERNARY_EXPR: {
            auto* ternary = static_cast<TernaryExpr*>(expr);
            emitBytecodeExpression(ternary->condition.get());
            bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
            size_t elsePatch = bytecodeEmitter.currentOffset();
            bytecodeEmitter.emitShort(0); // placeholder
            
            emitBytecodeExpression(ternary->thenExpr.get());
            bytecodeEmitter.emit(OpCode::JUMP);
            size_t endPatch = bytecodeEmitter.currentOffset();
            bytecodeEmitter.emitShort(0); // placeholder
            
            bytecodeEmitter.patchJump(elsePatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
            emitBytecodeExpression(ternary->elseExpr.get());
            bytecodeEmitter.patchJump(endPatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
            break;
        }
        default:
            throw std::runtime_error("Unsupported expression type in bytecode generation");
    }
}

void CodeGenerator::emitBytecodeStatement(Statement* stmt) {
    switch (stmt->type) {
        case ASTNodeType::EXPR_STMT: {
            auto* exprStmt = static_cast<ExprStmt*>(stmt);
            emitBytecodeExpression(exprStmt->expression.get());
            bytecodeEmitter.emit(OpCode::POP);
            break;
        }
        case ASTNodeType::VAR_DECL: {
            auto* varDecl = static_cast<VarDecl*>(stmt);
            if (varDecl->initializer) {
                emitBytecodeExpression(varDecl->initializer.get());
            } else {
                bytecodeEmitter.emit(OpCode::PUSH_INT);
                bytecodeEmitter.emitInt(0);
            }
            bytecodeEmitter.emit(OpCode::STORE_VAR);
            bytecodeEmitter.emitString(varDecl->name);
            // STORE_VAR leaves the value on the stack; pop it since
            // variable declarations are statements, not expressions.
            bytecodeEmitter.emit(OpCode::POP);
            break;
        }
        case ASTNodeType::RETURN_STMT: {
            auto* retStmt = static_cast<ReturnStmt*>(stmt);
            if (retStmt->value) {
                emitBytecodeExpression(retStmt->value.get());
            } else {
                bytecodeEmitter.emit(OpCode::PUSH_INT);
                bytecodeEmitter.emitInt(0);
            }
            bytecodeEmitter.emit(OpCode::RETURN);
            break;
        }
        case ASTNodeType::IF_STMT: {
            auto* ifStmt = static_cast<IfStmt*>(stmt);
            emitBytecodeExpression(ifStmt->condition.get());
            
            bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
            size_t elsePatch = bytecodeEmitter.currentOffset();
            bytecodeEmitter.emitShort(0); // placeholder
            
            emitBytecodeStatement(ifStmt->thenBranch.get());
            
            if (ifStmt->elseBranch) {
                bytecodeEmitter.emit(OpCode::JUMP);
                size_t endPatch = bytecodeEmitter.currentOffset();
                bytecodeEmitter.emitShort(0); // placeholder
                
                bytecodeEmitter.patchJump(elsePatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
                emitBytecodeStatement(ifStmt->elseBranch.get());
                bytecodeEmitter.patchJump(endPatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
            } else {
                bytecodeEmitter.patchJump(elsePatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
            }
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            auto* whileStmt = static_cast<WhileStmt*>(stmt);
            size_t loopStart = bytecodeEmitter.currentOffset();
            
            emitBytecodeExpression(whileStmt->condition.get());
            bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
            size_t exitPatch = bytecodeEmitter.currentOffset();
            bytecodeEmitter.emitShort(0); // placeholder
            
            emitBytecodeStatement(whileStmt->body.get());
            
            bytecodeEmitter.emit(OpCode::JUMP);
            bytecodeEmitter.emitShort(static_cast<uint16_t>(loopStart));
            
            bytecodeEmitter.patchJump(exitPatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
            break;
        }
        case ASTNodeType::BLOCK: {
            emitBytecodeBlock(static_cast<BlockStmt*>(stmt));
            break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            auto* doWhileStmt = static_cast<DoWhileStmt*>(stmt);
            size_t loopStart = bytecodeEmitter.currentOffset();
            
            emitBytecodeStatement(doWhileStmt->body.get());
            
            emitBytecodeExpression(doWhileStmt->condition.get());
            // Jump back to the start if condition is true (negate: jump if false to exit)
            bytecodeEmitter.emit(OpCode::NOT);
            bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
            bytecodeEmitter.emitShort(static_cast<uint16_t>(loopStart));
            break;
        }
        case ASTNodeType::FOR_STMT: {
            auto* forStmt = static_cast<ForStmt*>(stmt);
            // Initialize iterator variable
            emitBytecodeExpression(forStmt->start.get());
            bytecodeEmitter.emit(OpCode::STORE_VAR);
            bytecodeEmitter.emitString(forStmt->iteratorVar);
            bytecodeEmitter.emit(OpCode::POP);
            
            size_t loopStart = bytecodeEmitter.currentOffset();
            
            // Condition: iterator < end
            bytecodeEmitter.emit(OpCode::LOAD_VAR);
            bytecodeEmitter.emitString(forStmt->iteratorVar);
            emitBytecodeExpression(forStmt->end.get());
            bytecodeEmitter.emit(OpCode::LT);
            
            bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
            size_t exitPatch = bytecodeEmitter.currentOffset();
            bytecodeEmitter.emitShort(0); // placeholder
            
            // Body
            emitBytecodeStatement(forStmt->body.get());
            
            // Increment: iterator = iterator + step (default step = 1)
            bytecodeEmitter.emit(OpCode::LOAD_VAR);
            bytecodeEmitter.emitString(forStmt->iteratorVar);
            if (forStmt->step) {
                emitBytecodeExpression(forStmt->step.get());
            } else {
                bytecodeEmitter.emit(OpCode::PUSH_INT);
                bytecodeEmitter.emitInt(1);
            }
            bytecodeEmitter.emit(OpCode::ADD);
            bytecodeEmitter.emit(OpCode::STORE_VAR);
            bytecodeEmitter.emitString(forStmt->iteratorVar);
            bytecodeEmitter.emit(OpCode::POP);
            
            bytecodeEmitter.emit(OpCode::JUMP);
            bytecodeEmitter.emitShort(static_cast<uint16_t>(loopStart));
            
            bytecodeEmitter.patchJump(exitPatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
            break;
        }
        default:
            throw std::runtime_error("Unsupported statement type in bytecode generation");
    }
}

void CodeGenerator::emitBytecodeBlock(BlockStmt* stmt) {
    for (auto& statement : stmt->statements) {
        emitBytecodeStatement(statement.get());
    }
}

} // namespace omscript
