#include "codegen.h"
#include <climits>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Utils.h>
#include <optional>
#include <set>
#include <stdexcept>

namespace {

using omscript::ArrayExpr;
using omscript::AssignExpr;
using omscript::ASTNodeType;
using omscript::BinaryExpr;
using omscript::BlockStmt;
using omscript::CallExpr;
using omscript::DoWhileStmt;
using omscript::Expression;
using omscript::ExprStmt;
using omscript::ForEachStmt;
using omscript::ForStmt;
using omscript::IfStmt;
using omscript::IndexAssignExpr;
using omscript::IndexExpr;
using omscript::LiteralExpr;
using omscript::PostfixExpr;
using omscript::PrefixExpr;
using omscript::ReturnStmt;
using omscript::Statement;
using omscript::TernaryExpr;
using omscript::UnaryExpr;
using omscript::VarDecl;
using omscript::WhileStmt;

/// Returns the log2 of a positive power-of-two value, or -1 if the value is
/// not a power of two (or is non-positive).  Used by strength-reduction passes
/// to convert multiply/divide/modulo by a constant power of 2 into cheaper
/// shift or bitwise-AND operations.
inline int log2IfPowerOf2(int64_t val) {
    if (val <= 0 || (val & (val - 1)) != 0)
        return -1;
    int shift = 0;
    while (val > 1) {
        val >>= 1;
        shift++;
    }
    return shift;
}

/// Returns true if the expression is a simple value with no side effects
/// (literal or identifier).  Used by algebraic identity optimizations to
/// determine if an operand can be safely dropped.
inline bool isPureExpression(const Expression* expr) {
    return expr->type == ASTNodeType::LITERAL_EXPR || expr->type == ASTNodeType::IDENTIFIER_EXPR;
}

std::unique_ptr<Expression> optimizeOptMaxExpression(std::unique_ptr<Expression> expr);

std::unique_ptr<Expression> optimizeOptMaxUnary(const std::string& op, std::unique_ptr<Expression> operand) {
    operand = optimizeOptMaxExpression(std::move(operand));

    // Double-negation elimination: -(-x) → x, !(! x) → bool(x), ~(~x) → x
    auto* innerUnary = dynamic_cast<UnaryExpr*>(operand.get());
    if (innerUnary && innerUnary->op == op) {
        if (op == "-" || op == "~")
            return std::move(innerUnary->operand);
    }

    auto* literal = dynamic_cast<LiteralExpr*>(operand.get());
    if (!literal) {
        return std::make_unique<UnaryExpr>(op, std::move(operand));
    }

    if (literal->literalType == LiteralExpr::LiteralType::INTEGER) {
        long long value = literal->intValue;
        if (op == "-") {
            if (value == LLONG_MIN)
                return std::make_unique<UnaryExpr>(op, std::move(operand));
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

std::unique_ptr<Expression> optimizeOptMaxBinary(const std::string& op, std::unique_ptr<Expression> left,
                                                 std::unique_ptr<Expression> right) {
    left = optimizeOptMaxExpression(std::move(left));
    right = optimizeOptMaxExpression(std::move(right));
    auto* leftLiteral = dynamic_cast<LiteralExpr*>(left.get());
    auto* rightLiteral = dynamic_cast<LiteralExpr*>(right.get());

    // Algebraic identity optimizations (one side is a literal)
    // Note: when the result is a constant that drops the non-literal operand
    // (e.g. 0*x→0, x*0→0, 1**x→1, x**0→1), we only apply the optimization
    // if the dropped operand is pure (no side effects like function calls).
    if (leftLiteral && leftLiteral->literalType == LiteralExpr::LiteralType::INTEGER) {
        long long lval = leftLiteral->intValue;
        if (lval == 0 && op == "+")
            return right; // 0 + x → x
        if (lval == 0 && (op == "*" || op == "&") && isPureExpression(right.get()))
            return std::make_unique<LiteralExpr>(static_cast<long long>(0)); // 0 * x, 0 & x → 0
        if (lval == 0 && (op == "|" || op == "^"))
            return right; // 0 | x, 0 ^ x → x
        if (lval == 1 && op == "*")
            return right; // 1 * x → x
        if (lval == 1 && op == "**" && isPureExpression(right.get()))
            return std::make_unique<LiteralExpr>(static_cast<long long>(1)); // 1 ** x → 1
    }
    if (rightLiteral && rightLiteral->literalType == LiteralExpr::LiteralType::INTEGER) {
        long long rval = rightLiteral->intValue;
        if (rval == 0 && op == "+")
            return left; // x + 0 → x
        if (rval == 0 && op == "-")
            return left; // x - 0 → x
        if (rval == 0 && (op == "*" || op == "&") && isPureExpression(left.get()))
            return std::make_unique<LiteralExpr>(static_cast<long long>(0)); // x * 0, x & 0 → 0
        if (rval == 0 && (op == "|" || op == "^" || op == "<<" || op == ">>"))
            return left; // x | 0, x ^ 0, x << 0, x >> 0 → x
        if (rval == 1 && op == "*")
            return left; // x * 1 → x
        if (rval == 1 && op == "/")
            return left; // x / 1 → x
        if (rval == 1 && op == "**")
            return left; // x ** 1 → x
        if (rval == 0 && op == "**" && isPureExpression(left.get()))
            return std::make_unique<LiteralExpr>(static_cast<long long>(1)); // x ** 0 → 1
    }

    if (!leftLiteral || !rightLiteral) {
        return std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    if (leftLiteral->literalType == LiteralExpr::LiteralType::INTEGER &&
        rightLiteral->literalType == LiteralExpr::LiteralType::INTEGER) {
        long long lval = leftLiteral->intValue;
        long long rval = rightLiteral->intValue;
        if (op == "+")
            return std::make_unique<LiteralExpr>(lval + rval);
        if (op == "-")
            return std::make_unique<LiteralExpr>(lval - rval);
        if (op == "*")
            return std::make_unique<LiteralExpr>(lval * rval);
        if (op == "/" && rval != 0 && !(lval == LLONG_MIN && rval == -1))
            return std::make_unique<LiteralExpr>(lval / rval);
        if (op == "%" && rval != 0 && !(lval == LLONG_MIN && rval == -1))
            return std::make_unique<LiteralExpr>(lval % rval);
        if (op == "==")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval == rval));
        if (op == "!=")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval != rval));
        if (op == "<")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval < rval));
        if (op == "<=")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval <= rval));
        if (op == ">")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval > rval));
        if (op == ">=")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval >= rval));
        if (op == "&&")
            return std::make_unique<LiteralExpr>(static_cast<long long>((lval != 0) && (rval != 0)));
        if (op == "||")
            return std::make_unique<LiteralExpr>(static_cast<long long>((lval != 0) || (rval != 0)));
        if (op == "&")
            return std::make_unique<LiteralExpr>(lval & rval);
        if (op == "|")
            return std::make_unique<LiteralExpr>(lval | rval);
        if (op == "^")
            return std::make_unique<LiteralExpr>(lval ^ rval);
        if (op == "<<" && rval >= 0 && rval < 64)
            return std::make_unique<LiteralExpr>(lval << rval);
        if (op == ">>" && rval >= 0 && rval < 64)
            return std::make_unique<LiteralExpr>(lval >> rval);
        if (op == "**") {
            if (rval >= 0) {
                long long result = 1;
                for (long long i = 0; i < rval; i++)
                    result *= lval;
                return std::make_unique<LiteralExpr>(result);
            } else {
                return std::make_unique<LiteralExpr>(static_cast<long long>(0));
            }
        }
    } else if (leftLiteral->literalType == LiteralExpr::LiteralType::FLOAT &&
               rightLiteral->literalType == LiteralExpr::LiteralType::FLOAT) {
        double lval = leftLiteral->floatValue;
        double rval = rightLiteral->floatValue;
        if (op == "+")
            return std::make_unique<LiteralExpr>(lval + rval);
        if (op == "-")
            return std::make_unique<LiteralExpr>(lval - rval);
        if (op == "*")
            return std::make_unique<LiteralExpr>(lval * rval);
        if (op == "/" && rval != 0.0)
            return std::make_unique<LiteralExpr>(lval / rval);
        if (op == "%" && rval != 0.0)
            return std::make_unique<LiteralExpr>(std::fmod(lval, rval));
        if (op == "==")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval == rval));
        if (op == "!=")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval != rval));
        if (op == "<")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval < rval));
        if (op == "<=")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval <= rval));
        if (op == ">")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval > rval));
        if (op == ">=")
            return std::make_unique<LiteralExpr>(static_cast<long long>(lval >= rval));
        if (op == "&&")
            return std::make_unique<LiteralExpr>(static_cast<long long>((lval != 0.0) && (rval != 0.0)));
        if (op == "||")
            return std::make_unique<LiteralExpr>(static_cast<long long>((lval != 0.0) || (rval != 0.0)));
        if (op == "**")
            return std::make_unique<LiteralExpr>(std::pow(lval, rval));
    } else {
        // Mixed int/float constant folding: promote the integer operand to double
        double leftDouble = (leftLiteral->literalType == LiteralExpr::LiteralType::FLOAT)
                                ? leftLiteral->floatValue
                                : static_cast<double>(leftLiteral->intValue);
        double rightDouble = (rightLiteral->literalType == LiteralExpr::LiteralType::FLOAT)
                                 ? rightLiteral->floatValue
                                 : static_cast<double>(rightLiteral->intValue);
        if (op == "+")
            return std::make_unique<LiteralExpr>(leftDouble + rightDouble);
        if (op == "-")
            return std::make_unique<LiteralExpr>(leftDouble - rightDouble);
        if (op == "*")
            return std::make_unique<LiteralExpr>(leftDouble * rightDouble);
        if (op == "/" && rightDouble != 0.0)
            return std::make_unique<LiteralExpr>(leftDouble / rightDouble);
        if (op == "%" && rightDouble != 0.0)
            return std::make_unique<LiteralExpr>(std::fmod(leftDouble, rightDouble));
        if (op == "==")
            return std::make_unique<LiteralExpr>(static_cast<long long>(leftDouble == rightDouble));
        if (op == "!=")
            return std::make_unique<LiteralExpr>(static_cast<long long>(leftDouble != rightDouble));
        if (op == "<")
            return std::make_unique<LiteralExpr>(static_cast<long long>(leftDouble < rightDouble));
        if (op == "<=")
            return std::make_unique<LiteralExpr>(static_cast<long long>(leftDouble <= rightDouble));
        if (op == ">")
            return std::make_unique<LiteralExpr>(static_cast<long long>(leftDouble > rightDouble));
        if (op == ">=")
            return std::make_unique<LiteralExpr>(static_cast<long long>(leftDouble >= rightDouble));
        if (op == "**")
            return std::make_unique<LiteralExpr>(std::pow(leftDouble, rightDouble));
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
    case ASTNodeType::INDEX_ASSIGN_EXPR: {
        auto* ia = static_cast<IndexAssignExpr*>(expr.get());
        ia->array = optimizeOptMaxExpression(std::move(ia->array));
        ia->index = optimizeOptMaxExpression(std::move(ia->index));
        ia->value = optimizeOptMaxExpression(std::move(ia->value));
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
            return condLiteral->intValue != 0 ? std::move(ternary->thenExpr) : std::move(ternary->elseExpr);
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
    case ASTNodeType::FOR_EACH_STMT: {
        auto* forEach = static_cast<ForEachStmt*>(stmt);
        forEach->collection = optimizeOptMaxExpression(std::move(forEach->collection));
        optimizeOptMaxStatement(forEach->body.get());
        break;
    }
    case ASTNodeType::SWITCH_STMT: {
        auto* switchStmt = static_cast<omscript::SwitchStmt*>(stmt);
        switchStmt->condition = optimizeOptMaxExpression(std::move(switchStmt->condition));
        for (auto& sc : switchStmt->cases) {
            if (sc.value) {
                sc.value = optimizeOptMaxExpression(std::move(sc.value));
            }
            for (auto& s : sc.body) {
                optimizeOptMaxStatement(s.get());
            }
        }
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
    "abs",        "assert",    "char_at", "clamp",    "gcd",    "input", "is_alpha",   "is_digit",
    "is_even",    "is_odd",    "len",     "log2",     "max",    "min",   "pow",        "print",
    "print_char", "reverse",   "sign",    "sqrt",     "str_concat",      "str_eq",     "str_find",
    "str_len",    "sum",       "swap",    "to_char",  "to_string",       "typeof"};

bool isStdlibFunction(const std::string& name) {
    return stdlibFunctions.find(name) != stdlibFunctions.end();
}

CodeGenerator::CodeGenerator(OptimizationLevel optLevel)
    : inOptMaxFunction(false), hasOptMaxFunctions(false), useDynamicCompilation(false), optimizationLevel(optLevel) {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("omscript", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    setupPrintfDeclaration();
}

CodeGenerator::~CodeGenerator() = default;

// ---------------------------------------------------------------------------
// Execution-tier classification
// ---------------------------------------------------------------------------

ExecutionTier CodeGenerator::classifyFunction(const FunctionDecl* func) const {
    // main is always AOT-compiled — it is the program entry point.
    if (func->name == "main")
        return ExecutionTier::AOT;

    // OPTMAX functions are explicitly marked for aggressive AOT optimisation.
    if (func->isOptMax)
        return ExecutionTier::AOT;

    // Stdlib built-ins are always native (handled separately, but classify
    // them here for completeness).
    if (isStdlibFunction(func->name))
        return ExecutionTier::AOT;

    // If every parameter carries a type annotation the function is
    // statically typed and therefore eligible for AOT compilation.
    if (func->hasFullTypeAnnotations())
        return ExecutionTier::AOT;

    // Everything else starts life as interpreted bytecode.
    // The VM profiler may later promote it to JIT.
    return ExecutionTier::Interpreted;
}

void CodeGenerator::setupPrintfDeclaration() {
    // Declare printf function for output
    std::vector<llvm::Type*> printfArgs;
    printfArgs.push_back(llvm::PointerType::getUnqual(*context));

    llvm::FunctionType* printfType = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), printfArgs, true);

    llvm::Function::Create(printfType, llvm::Function::ExternalLinkage, "printf", module.get());
}

llvm::Function* CodeGenerator::getPrintfFunction() {
    return module->getFunction("printf");
}

llvm::Type* CodeGenerator::getDefaultType() {
    // Default to 64-bit integer for dynamic typing
    return llvm::Type::getInt64Ty(*context);
}

llvm::Type* CodeGenerator::getFloatType() {
    return llvm::Type::getDoubleTy(*context);
}

llvm::Value* CodeGenerator::toBool(llvm::Value* v) {
    if (v->getType()->isDoubleTy()) {
        return builder->CreateFCmpONE(v, llvm::ConstantFP::get(getFloatType(), 0.0), "tobool");
    } else if (v->getType()->isPointerTy()) {
        llvm::Value* intVal = builder->CreatePtrToInt(v, getDefaultType(), "ptrtoint");
        return builder->CreateICmpNE(intVal, llvm::ConstantInt::get(getDefaultType(), 0), "tobool");
    } else {
        return builder->CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0, true), "tobool");
    }
}

llvm::Value* CodeGenerator::toDefaultType(llvm::Value* v) {
    if (v->getType() == getDefaultType())
        return v;
    if (v->getType()->isDoubleTy()) {
        return builder->CreateFPToSI(v, getDefaultType(), "ftoi");
    }
    if (v->getType()->isPointerTy()) {
        return builder->CreatePtrToInt(v, getDefaultType(), "ptoi");
    }
    if (v->getType()->isIntegerTy() && v->getType() != getDefaultType()) {
        return builder->CreateZExt(v, getDefaultType(), "zext");
    }
    return v;
}

llvm::Value* CodeGenerator::ensureFloat(llvm::Value* v) {
    if (v->getType()->isDoubleTy())
        return v;
    if (v->getType()->isIntegerTy()) {
        return builder->CreateSIToFP(v, getFloatType(), "itof");
    }
    if (v->getType()->isPointerTy()) {
        // Convert pointer to int first, then to float
        llvm::Value* intVal = builder->CreatePtrToInt(v, getDefaultType(), "ptoi");
        return builder->CreateSIToFP(intVal, getFloatType(), "itof");
    }
    return v;
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
        throw std::runtime_error("Scope tracking mismatch in codegen (" + std::string(location) +
                                 "): values=" + std::to_string(scopeStack.size()) +
                                 ", consts=" + std::to_string(constScopeStack.size()));
    }
}

llvm::AllocaInst* CodeGenerator::createEntryBlockAlloca(llvm::Function* function, const std::string& name,
                                                        llvm::Type* type) {
    llvm::IRBuilder<> entryBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
    return entryBuilder.CreateAlloca(type ? type : getDefaultType(), nullptr, name);
}

void CodeGenerator::codegenError(const std::string& message, const ASTNode* node) {
    if (node && node->line > 0) {
        throw std::runtime_error("Error at line " + std::to_string(node->line) + ", column " +
                                 std::to_string(node->column) + ": " + message);
    }
    throw std::runtime_error(message);
}

// ---------------------------------------------------------------------------
// Lazy C library function declarations
// ---------------------------------------------------------------------------
// These helpers ensure each C library function is declared at most once in
// the LLVM module, eliminating duplicated getFunction()/Create() blocks
// that were previously scattered across multiple built-in handlers.

llvm::Function* CodeGenerator::getOrDeclareStrlen() {
    if (auto* fn = module->getFunction("strlen"))
        return fn;
    auto* ty = llvm::FunctionType::get(getDefaultType(), {llvm::PointerType::getUnqual(*context)}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "strlen", module.get());
}

llvm::Function* CodeGenerator::getOrDeclareMalloc() {
    if (auto* fn = module->getFunction("malloc"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::PointerType::getUnqual(*context), {getDefaultType()}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "malloc", module.get());
}

llvm::Function* CodeGenerator::getOrDeclareStrcpy() {
    if (auto* fn = module->getFunction("strcpy"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "strcpy", module.get());
}

llvm::Function* CodeGenerator::getOrDeclareStrcat() {
    if (auto* fn = module->getFunction("strcat"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "strcat", module.get());
}

llvm::Function* CodeGenerator::getOrDeclareStrcmp() {
    if (auto* fn = module->getFunction("strcmp"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(builder->getInt32Ty(), {ptrTy, ptrTy}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "strcmp", module.get());
}

llvm::Function* CodeGenerator::getOrDeclarePutchar() {
    if (auto* fn = module->getFunction("putchar"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "putchar", module.get());
}

llvm::Function* CodeGenerator::getOrDeclareScanf() {
    if (auto* fn = module->getFunction("scanf"))
        return fn;
    auto* ty =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::PointerType::getUnqual(*context)}, true);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "scanf", module.get());
}

llvm::Function* CodeGenerator::getOrDeclareExit() {
    if (auto* fn = module->getFunction("exit"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {llvm::Type::getInt32Ty(*context)}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "exit", module.get());
}

llvm::Function* CodeGenerator::getOrDeclareAbort() {
    if (auto* fn = module->getFunction("abort"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "abort", module.get());
}

llvm::Function* CodeGenerator::getOrDeclareSnprintf() {
    if (auto* fn = module->getFunction("snprintf"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, getDefaultType(), ptrTy}, true);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "snprintf", module.get());
}

llvm::Function* CodeGenerator::getOrDeclareMemchr() {
    if (auto* fn = module->getFunction("memchr"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, llvm::Type::getInt32Ty(*context), getDefaultType()}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "memchr", module.get());
}

// ---------------------------------------------------------------------------
// String type inference helpers
// ---------------------------------------------------------------------------

bool CodeGenerator::isPreAnalysisStringExpr(Expression* expr, const std::unordered_set<size_t>& paramStringIndices,
                                            const FunctionDecl* func) const {
    if (!expr)
        return false;
    if (auto* lit = dynamic_cast<LiteralExpr*>(expr))
        return lit->literalType == LiteralExpr::LiteralType::STRING;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        if (func) {
            for (size_t i = 0; i < func->parameters.size(); i++) {
                if (func->parameters[i].name == id->name && paramStringIndices.count(i))
                    return true;
            }
        }
        return false;
    }
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        return bin->op == "+" && (isPreAnalysisStringExpr(bin->left.get(), paramStringIndices, func) ||
                                  isPreAnalysisStringExpr(bin->right.get(), paramStringIndices, func));
    }
    if (auto* tern = dynamic_cast<TernaryExpr*>(expr)) {
        return isPreAnalysisStringExpr(tern->thenExpr.get(), paramStringIndices, func) ||
               isPreAnalysisStringExpr(tern->elseExpr.get(), paramStringIndices, func);
    }
    if (auto* call = dynamic_cast<CallExpr*>(expr))
        return stringReturningFunctions_.count(call->callee) > 0;
    return false;
}

bool CodeGenerator::scanStmtForStringReturns(Statement* stmt, const std::unordered_set<size_t>& paramStringIndices,
                                             const FunctionDecl* func) const {
    if (!stmt)
        return false;
    if (auto* ret = dynamic_cast<ReturnStmt*>(stmt))
        return ret->value && isPreAnalysisStringExpr(ret->value.get(), paramStringIndices, func);
    if (auto* block = dynamic_cast<BlockStmt*>(stmt)) {
        for (auto& s : block->statements)
            if (scanStmtForStringReturns(s.get(), paramStringIndices, func))
                return true;
    }
    if (auto* ifS = dynamic_cast<IfStmt*>(stmt)) {
        return scanStmtForStringReturns(ifS->thenBranch.get(), paramStringIndices, func) ||
               scanStmtForStringReturns(ifS->elseBranch.get(), paramStringIndices, func);
    }
    if (auto* whileS = dynamic_cast<WhileStmt*>(stmt))
        return scanStmtForStringReturns(whileS->body.get(), paramStringIndices, func);
    if (auto* doS = dynamic_cast<DoWhileStmt*>(stmt))
        return scanStmtForStringReturns(doS->body.get(), paramStringIndices, func);
    if (auto* forS = dynamic_cast<ForStmt*>(stmt))
        return scanStmtForStringReturns(forS->body.get(), paramStringIndices, func);
    if (auto* forEachS = dynamic_cast<ForEachStmt*>(stmt))
        return scanStmtForStringReturns(forEachS->body.get(), paramStringIndices, func);
    return false;
}

void CodeGenerator::scanStmtForStringCalls(Statement* stmt) {
    if (!stmt)
        return;
    auto scanExpr = [this](auto& self, Expression* expr) -> void {
        if (!expr)
            return;
        if (auto* call = dynamic_cast<CallExpr*>(expr)) {
            for (size_t i = 0; i < call->arguments.size(); i++) {
                if (isPreAnalysisStringExpr(call->arguments[i].get(), {}, nullptr))
                    funcParamStringTypes_[call->callee].insert(i);
            }
            for (auto& arg : call->arguments)
                self(self, arg.get());
        }
        if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
            self(self, bin->left.get());
            self(self, bin->right.get());
        }
        if (auto* assign = dynamic_cast<AssignExpr*>(expr))
            self(self, assign->value.get());
        if (auto* tern = dynamic_cast<TernaryExpr*>(expr)) {
            self(self, tern->condition.get());
            self(self, tern->thenExpr.get());
            self(self, tern->elseExpr.get());
        }
    };
    auto scanStmt = [&](auto& self, Statement* s) -> void {
        if (!s)
            return;
        if (auto* block = dynamic_cast<BlockStmt*>(s)) {
            for (auto& st : block->statements)
                self(self, st.get());
        } else if (auto* exprS = dynamic_cast<ExprStmt*>(s)) {
            scanExpr(scanExpr, exprS->expression.get());
        } else if (auto* varDecl = dynamic_cast<VarDecl*>(s)) {
            if (varDecl->initializer)
                scanExpr(scanExpr, varDecl->initializer.get());
        } else if (auto* ret = dynamic_cast<ReturnStmt*>(s)) {
            if (ret->value)
                scanExpr(scanExpr, ret->value.get());
        } else if (auto* ifS = dynamic_cast<IfStmt*>(s)) {
            scanExpr(scanExpr, ifS->condition.get());
            self(self, ifS->thenBranch.get());
            self(self, ifS->elseBranch.get());
        } else if (auto* whileS = dynamic_cast<WhileStmt*>(s)) {
            scanExpr(scanExpr, whileS->condition.get());
            self(self, whileS->body.get());
        } else if (auto* doS = dynamic_cast<DoWhileStmt*>(s)) {
            self(self, doS->body.get());
            scanExpr(scanExpr, doS->condition.get());
        } else if (auto* forS = dynamic_cast<ForStmt*>(s)) {
            if (forS->start)
                scanExpr(scanExpr, forS->start.get());
            if (forS->end)
                scanExpr(scanExpr, forS->end.get());
            if (forS->step)
                scanExpr(scanExpr, forS->step.get());
            self(self, forS->body.get());
        } else if (auto* forEachS = dynamic_cast<ForEachStmt*>(s)) {
            scanExpr(scanExpr, forEachS->collection.get());
            self(self, forEachS->body.get());
        }
    };
    scanStmt(scanStmt, stmt);
}

void CodeGenerator::preAnalyzeStringTypes(Program* program) {
    // Iteratively propagate string type information until no new facts are learned.
    // Each iteration may uncover new string-returning functions or string parameters,
    // which in turn enables further propagation in the next iteration.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& func : program->functions) {
            // Collect the set of parameter indices known to receive string arguments
            // for this function (from funcParamStringTypes_ populated so far).
            std::unordered_set<size_t> paramStrIdx;
            auto pit = funcParamStringTypes_.find(func->name);
            if (pit != funcParamStringTypes_.end())
                paramStrIdx = pit->second;

            // Check if this function returns a string value.
            if (!stringReturningFunctions_.count(func->name)) {
                if (scanStmtForStringReturns(func->body.get(), paramStrIdx, func.get())) {
                    stringReturningFunctions_.insert(func->name);
                    changed = true;
                }
            }

            // Scan all call sites in this function's body to find which parameters
            // of other functions receive string arguments.
            // Track change by counting total entries before/after.
            size_t prevTotal = 0;
            for (auto& kv : funcParamStringTypes_)
                prevTotal += kv.second.size();
            scanStmtForStringCalls(func->body.get());
            size_t newTotal = 0;
            for (auto& kv : funcParamStringTypes_)
                newTotal += kv.second.size();
            if (newTotal > prevTotal)
                changed = true;
        }
    }
}

bool CodeGenerator::isStringExpr(Expression* expr) const {
    if (!expr)
        return false;
    if (auto* lit = dynamic_cast<LiteralExpr*>(expr))
        return lit->literalType == LiteralExpr::LiteralType::STRING;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        // Check the LLVM alloca type (handles local vars initialized with string literals).
        auto it = namedValues.find(id->name);
        if (it != namedValues.end() && it->second) {
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);
            if (alloca && alloca->getAllocatedType()->isPointerTy())
                return true;
        }
        // Check the runtime string-variable tracker (handles params and vars
        // assigned from string function returns).
        return stringVars_.count(id->name) > 0;
    }
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        return bin->op == "+" && (isStringExpr(bin->left.get()) || isStringExpr(bin->right.get()));
    }
    if (auto* tern = dynamic_cast<TernaryExpr*>(expr)) {
        return isStringExpr(tern->thenExpr.get()) || isStringExpr(tern->elseExpr.get());
    }
    if (auto* call = dynamic_cast<CallExpr*>(expr))
        return stringReturningFunctions_.count(call->callee) > 0;
    return false;
}

void CodeGenerator::generate(Program* program) {
    hasOptMaxFunctions = false;
    optMaxFunctions.clear();

    // Validate: check for duplicate function names and duplicate parameters.
    bool hasMain = false;
    std::unordered_map<std::string, const FunctionDecl*> seenFunctions;
    for (auto& func : program->functions) {
        if (func->name == "main") {
            hasMain = true;
        }
        auto it = seenFunctions.find(func->name);
        if (it != seenFunctions.end()) {
            codegenError("Duplicate function definition: '" + func->name + "' (previously defined at line " +
                             std::to_string(it->second->line) + ")",
                         func.get());
        }
        seenFunctions[func->name] = func.get();

        // Check for duplicate parameter names within this function.
        std::unordered_set<std::string> seenParams;
        for (const auto& param : func->parameters) {
            if (!seenParams.insert(param.name).second) {
                codegenError("Duplicate parameter name '" + param.name + "' in function '" + func->name + "'",
                             func.get());
            }
        }

        if (func->isOptMax) {
            optMaxFunctions.insert(func->name);
        }

        // Classify the execution tier for this function.
        functionTiers[func->name] = classifyFunction(func.get());
    }
    if (!hasMain) {
        // Program-level error — no specific AST node to reference for location.
        throw std::runtime_error("No 'main' function defined");
    }

    // Forward-declare all functions so that any function can reference any
    // other regardless of source-file ordering (enables mutual recursion).
    for (auto& func : program->functions) {
        std::vector<llvm::Type*> paramTypes(func->parameters.size(), getDefaultType());
        llvm::FunctionType* funcType = llvm::FunctionType::get(getDefaultType(), paramTypes, false);
        llvm::Function* function =
            llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, func->name, module.get());
        functions[func->name] = function;
    }

    // Pre-analyze string types: determine which functions return strings and
    // which parameters receive string arguments, so that print/concat/etc.
    // work correctly when strings cross function boundaries.
    preAnalyzeStringTypes(program);

    // Generate all function bodies
    for (auto& func : program->functions) {
        generateFunction(func.get());
    }

    // Run optimization passes
    if (optimizationLevel != OptimizationLevel::O0) {
        runOptimizationPasses();
    }

    if (hasOptMaxFunctions && enableOptMax_) {
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

    // Retrieve the forward-declared function
    llvm::Function* function = functions[func->name];

    // Hint small helper functions for inlining at O2+.  OPTMAX functions
    // already get aggressive optimization; non-main functions with few
    // statements benefit from being inlined into their callers.
    // 8 statements covers most simple accessors and arithmetic helpers.
    static constexpr size_t kMaxInlineHintStatements = 8;
    if (func->name != "main" && optimizationLevel >= OptimizationLevel::O2 && func->body &&
        func->body->statements.size() <= kMaxInlineHintStatements) {
        function->addFnAttr(llvm::Attribute::InlineHint);
    }

    // Create entry basic block
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", function);
    builder->SetInsertPoint(entry);

    // Set parameter names and create allocas
    namedValues.clear();
    scopeStack.clear();
    loopStack.clear();
    constValues.clear();
    constScopeStack.clear();
    stringVars_.clear();

    // Pre-populate stringVars_ for parameters known to receive string arguments.
    auto paramStrIt = funcParamStringTypes_.find(func->name);
    auto argIt = function->arg_begin();
    for (size_t paramIdx = 0; paramIdx < func->parameters.size(); ++paramIdx) {
        auto& param = func->parameters[paramIdx];
        argIt->setName(param.name);

        llvm::AllocaInst* alloca = createEntryBlockAlloca(function, param.name);
        builder->CreateStore(&(*argIt), alloca);
        bindVariable(param.name, alloca);

        if (paramStrIt != funcParamStringTypes_.end() && paramStrIt->second.count(paramIdx))
            stringVars_.insert(param.name);

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
    case ASTNodeType::FOR_EACH_STMT:
        generateForEach(static_cast<ForEachStmt*>(stmt));
        break;
    case ASTNodeType::BREAK_STMT:
        if (loopStack.empty()) {
            codegenError("break used outside of a loop", stmt);
        }
        builder->CreateBr(loopStack.back().breakTarget);
        break;
    case ASTNodeType::CONTINUE_STMT: {
        // Search backwards through the loop stack for the nearest enclosing loop
        // (not switch) that has a non-null continueTarget.  Switch statements push
        // a context with nullptr continueTarget so that 'break' exits the switch,
        // but 'continue' must skip over switch contexts to reach the loop.
        llvm::BasicBlock* continueTarget = nullptr;
        for (auto it = loopStack.rbegin(); it != loopStack.rend(); ++it) {
            if (it->continueTarget != nullptr) {
                continueTarget = it->continueTarget;
                break;
            }
        }
        if (!continueTarget) {
            codegenError("continue used outside of a loop", stmt);
        }
        builder->CreateBr(continueTarget);
        break;
    }
    case ASTNodeType::BLOCK:
        generateBlock(static_cast<BlockStmt*>(stmt));
        break;
    case ASTNodeType::EXPR_STMT:
        generateExprStmt(static_cast<ExprStmt*>(stmt));
        break;
    case ASTNodeType::SWITCH_STMT:
        generateSwitch(static_cast<SwitchStmt*>(stmt));
        break;
    default:
        codegenError("Unknown statement type", stmt);
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
    case ASTNodeType::INDEX_ASSIGN_EXPR:
        return generateIndexAssign(static_cast<IndexAssignExpr*>(expr));
    default:
        codegenError("Unknown expression type", expr);
    }
}

llvm::Value* CodeGenerator::generateLiteral(LiteralExpr* expr) {
    if (expr->literalType == LiteralExpr::LiteralType::INTEGER) {
        return llvm::ConstantInt::get(*context, llvm::APInt(64, expr->intValue));
    } else if (expr->literalType == LiteralExpr::LiteralType::FLOAT) {
        return llvm::ConstantFP::get(getFloatType(), expr->floatValue);
    } else {
        // String literal - return as a pointer to the global string data.
        // When passed directly to print(), the pointer form is used with %s.
        return builder->CreateGlobalString(expr->stringValue, "str");
    }
}

llvm::Value* CodeGenerator::generateIdentifier(IdentifierExpr* expr) {
    auto it = namedValues.find(expr->name);
    if (it == namedValues.end() || !it->second) {
        codegenError("Unknown variable: " + expr->name, expr);
    }
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);
    llvm::Type* loadType = alloca ? alloca->getAllocatedType() : getDefaultType();
    return builder->CreateLoad(loadType, it->second, expr->name.c_str());
}

llvm::Value* CodeGenerator::generateBinary(BinaryExpr* expr) {
    llvm::Value* left = generateExpression(expr->left.get());
    if (expr->op == "&&" || expr->op == "||") {
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::Value* leftBool = toBool(left);
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
        llvm::Value* rightBool = toBool(right);
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

    bool leftIsFloat = left->getType()->isDoubleTy();
    bool rightIsFloat = right->getType()->isDoubleTy();

    // Float operations path
    if (leftIsFloat || rightIsFloat) {
        if (!leftIsFloat)
            left = ensureFloat(left);
        if (!rightIsFloat)
            right = ensureFloat(right);

        if (expr->op == "+")
            return builder->CreateFAdd(left, right, "faddtmp");
        if (expr->op == "-")
            return builder->CreateFSub(left, right, "fsubtmp");
        if (expr->op == "*")
            return builder->CreateFMul(left, right, "fmultmp");
        if (expr->op == "/")
            return builder->CreateFDiv(left, right, "fdivtmp");
        if (expr->op == "%")
            return builder->CreateFRem(left, right, "fremtmp");

        if (expr->op == "==") {
            auto cmp = builder->CreateFCmpOEQ(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == "!=") {
            auto cmp = builder->CreateFCmpONE(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == "<") {
            auto cmp = builder->CreateFCmpOLT(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == "<=") {
            auto cmp = builder->CreateFCmpOLE(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == ">") {
            auto cmp = builder->CreateFCmpOGT(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == ">=") {
            auto cmp = builder->CreateFCmpOGE(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == "**") {
            // Float exponentiation: use llvm.pow intrinsic
#if LLVM_VERSION_MAJOR >= 19
            llvm::Function* powFn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::pow,
                                                                            {llvm::Type::getDoubleTy(*context)});
#else
            llvm::Function* powFn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::pow,
                                                                    {llvm::Type::getDoubleTy(*context)});
#endif
            return builder->CreateCall(powFn, {left, right}, "fpowtmp");
        }

        codegenError("Invalid binary operator for float operands: " + expr->op, expr);
    }

    // String concatenation path: either operand is a string (ptr or i64-as-string).
    if (expr->op == "+") {
        bool leftIsStr = left->getType()->isPointerTy() || isStringExpr(expr->left.get());
        bool rightIsStr = right->getType()->isPointerTy() || isStringExpr(expr->right.get());
        if (leftIsStr && rightIsStr) {
            // Convert i64 string values back to ptr if necessary.
            if (!left->getType()->isPointerTy())
                left = builder->CreateIntToPtr(left, llvm::PointerType::getUnqual(*context), "str.l.ptr");
            if (!right->getType()->isPointerTy())
                right = builder->CreateIntToPtr(right, llvm::PointerType::getUnqual(*context), "str.r.ptr");

            llvm::Value* len1 = builder->CreateCall(getOrDeclareStrlen(), {left}, "len1");
            llvm::Value* len2 = builder->CreateCall(getOrDeclareStrlen(), {right}, "len2");
            llvm::Value* totalLen = builder->CreateAdd(len1, len2, "totallen");
            llvm::Value* allocSize =
                builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "allocsize");
            llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "strbuf");
            builder->CreateCall(getOrDeclareStrcpy(), {buf, left});
            builder->CreateCall(getOrDeclareStrcat(), {buf, right});
            return buf;
        }
    }

    // Convert pointer types to i64 for integer operations (fallback)
    if (left->getType()->isPointerTy()) {
        left = builder->CreatePtrToInt(left, getDefaultType(), "ptoi");
    }
    if (right->getType()->isPointerTy()) {
        right = builder->CreatePtrToInt(right, getDefaultType(), "ptoi");
    }

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
            if (rval != 0 && !(lval == INT64_MIN && rval == -1)) {
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval / rval));
            }
        } else if (expr->op == "%") {
            if (rval != 0 && !(lval == INT64_MIN && rval == -1)) {
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval % rval));
            }
        } else if (expr->op == "==") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval == rval ? 1 : 0));
        } else if (expr->op == "!=") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval != rval ? 1 : 0));
        } else if (expr->op == "<") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval < rval ? 1 : 0));
        } else if (expr->op == "<=") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval <= rval ? 1 : 0));
        } else if (expr->op == ">") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval > rval ? 1 : 0));
        } else if (expr->op == ">=") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval >= rval ? 1 : 0));
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
        } else if (expr->op == "**") {
            if (rval >= 0) {
                int64_t result = 1;
                for (int64_t i = 0; i < rval; i++)
                    result *= lval;
                return llvm::ConstantInt::get(*context, llvm::APInt(64, result));
            } else {
                return llvm::ConstantInt::get(*context, llvm::APInt(64, 0));
            }
        }
    }

    // Algebraic identity optimizations — when one operand is a known constant,
    // many operations can be simplified without emitting any instruction.
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
        int64_t rv = ci->getSExtValue();
        if (rv == 0) {
            if (expr->op == "+" || expr->op == "-" || expr->op == "|" || expr->op == "^" || expr->op == "<<" ||
                expr->op == ">>")
                return left; // x+0, x-0, x|0, x^0, x<<0, x>>0 → x
            if (expr->op == "*" || expr->op == "&")
                return llvm::ConstantInt::get(getDefaultType(), 0); // x*0, x&0 → 0
            if (expr->op == "**")
                return llvm::ConstantInt::get(getDefaultType(), 1); // x**0 → 1
        }
        if (rv == 1) {
            if (expr->op == "*" || expr->op == "/" || expr->op == "**")
                return left; // x*1, x/1, x**1 → x
        }
    }
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
        int64_t lv = ci->getSExtValue();
        if (lv == 0) {
            if (expr->op == "+" || expr->op == "|" || expr->op == "^")
                return right; // 0+x, 0|x, 0^x → x
            if (expr->op == "*" || expr->op == "&")
                return llvm::ConstantInt::get(getDefaultType(), 0); // 0*x, 0&x → 0
        }
        if (lv == 1 && expr->op == "*")
            return right; // 1*x → x
        if (lv == 1 && expr->op == "**")
            return llvm::ConstantInt::get(getDefaultType(), 1); // 1**x → 1
    }

    // Regular code generation for non-constant expressions
    if (expr->op == "+") {
        return builder->CreateNSWAdd(left, right, "addtmp");
    } else if (expr->op == "-") {
        return builder->CreateNSWSub(left, right, "subtmp");
    } else if (expr->op == "*") {
        // Strength reduction: multiply by power of 2 → left shift
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            int s = log2IfPowerOf2(ci->getSExtValue());
            if (s >= 0)
                return builder->CreateShl(left, llvm::ConstantInt::get(getDefaultType(), s), "shltmp");
        }
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
            int s = log2IfPowerOf2(ci->getSExtValue());
            if (s >= 0)
                return builder->CreateShl(right, llvm::ConstantInt::get(getDefaultType(), s), "shltmp");
        }
        return builder->CreateNSWMul(left, right, "multmp");
    } else if (expr->op == "/" || expr->op == "%") {
        bool isDivision = expr->op == "/";

        // Strength reduction for constant power-of-2 divisors.
        // Since the divisor is a known positive constant it is never zero,
        // so we can skip the division-by-zero guard entirely.
        //   n / 2^k  →  SDiv (truncation toward zero; AShr rounds toward -∞)
        //   n % 2^k  →  SRem with the zero-check elided (constant != 0)
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            int s = log2IfPowerOf2(ci->getSExtValue());
            if (s >= 0) {
                if (isDivision) {
                    return builder->CreateSDiv(left, right, "divtmp");
                }
                // For modulo we still use SRem to preserve correct signed
                // semantics (e.g. -7 % 4 == -3), but the divisor is a known
                // non-zero constant so the zero-check is unnecessary.
                return builder->CreateSRem(left, right, "modtmp");
            }
        }

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* isZero = builder->CreateICmpEQ(right, zero, "divzero");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        const char* zeroName = isDivision ? "div.zero" : "mod.zero";
        const char* opName = isDivision ? "div.op" : "mod.op";
        llvm::BasicBlock* zeroBB = llvm::BasicBlock::Create(*context, zeroName, function);
        llvm::BasicBlock* opBB = llvm::BasicBlock::Create(*context, opName, function);
        builder->CreateCondBr(isZero, zeroBB, opBB);

        builder->SetInsertPoint(zeroBB);
        const char* messageText = isDivision ? "Runtime error: division by zero\n" : "Runtime error: modulo by zero\n";
        const char* messageName = isDivision ? "divzero_msg" : "modzero_msg";
        llvm::Value* message = builder->CreateGlobalString(messageText, messageName);
        builder->CreateCall(getPrintfFunction(), {message});
        builder->CreateCall(getOrDeclareExit(), {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1)});
        builder->CreateUnreachable();

        builder->SetInsertPoint(opBB);
        return isDivision ? builder->CreateSDiv(left, right, "divtmp") : builder->CreateSRem(left, right, "modtmp");
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
        // Mask shift amount to [0, 63] to prevent undefined behavior
        llvm::Value* mask = llvm::ConstantInt::get(getDefaultType(), 63);
        llvm::Value* safeShift = builder->CreateAnd(right, mask, "shlmask");
        return builder->CreateShl(left, safeShift, "shltmp");
    } else if (expr->op == ">>") {
        // Mask shift amount to [0, 63] to prevent undefined behavior
        llvm::Value* mask = llvm::ConstantInt::get(getDefaultType(), 63);
        llvm::Value* safeShift = builder->CreateAnd(right, mask, "shrmask");
        return builder->CreateAShr(left, safeShift, "ashrtmp");
    } else if (expr->op == "**") {
        // Exponentiation operator: base ** exponent
        // Uses iterative multiplication loop (same algorithm as pow() stdlib).
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* negExpBB = llvm::BasicBlock::Create(*context, "pow.negexp", function);
        llvm::BasicBlock* posExpBB = llvm::BasicBlock::Create(*context, "pow.posexp", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "pow.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "pow.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "pow.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* isNegExp = builder->CreateICmpSLT(right, zero, "pow.isneg");
        builder->CreateCondBr(isNegExp, negExpBB, posExpBB);

        builder->SetInsertPoint(negExpBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(posExpBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "pow.result");
        llvm::PHINode* counter = builder->CreatePHI(getDefaultType(), 2, "pow.counter");
        result->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), posExpBB);
        counter->addIncoming(right, posExpBB);

        llvm::Value* done = builder->CreateICmpSLE(counter, zero, "pow.done.cmp");
        builder->CreateCondBr(done, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* newResult = builder->CreateMul(result, left, "pow.mul");
        llvm::Value* newCounter = builder->CreateSub(counter, llvm::ConstantInt::get(getDefaultType(), 1), "pow.dec");
        result->addIncoming(newResult, bodyBB);
        counter->addIncoming(newCounter, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* finalResult = builder->CreatePHI(getDefaultType(), 2, "pow.final");
        finalResult->addIncoming(zero, negExpBB);
        finalResult->addIncoming(result, loopBB);
        return finalResult;
    }

    codegenError("Unknown binary operator: " + expr->op, expr);
}

llvm::Value* CodeGenerator::generateUnary(UnaryExpr* expr) {
    llvm::Value* operand = generateExpression(expr->operand.get());

    // Constant folding for unary operations
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(operand)) {
        int64_t val = ci->getSExtValue();
        if (expr->op == "-") {
            if (val != INT64_MIN)
                return llvm::ConstantInt::get(*context, llvm::APInt(64, -val));
        } else if (expr->op == "!") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, val == 0 ? 1 : 0));
        } else if (expr->op == "~") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, ~val));
        }
    }
    if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(operand)) {
        if (expr->op == "-") {
            return llvm::ConstantFP::get(getFloatType(), -cf->getValueAPF().convertToDouble());
        }
    }

    if (expr->op == "-") {
        if (operand->getType()->isDoubleTy()) {
            return builder->CreateFNeg(operand, "fnegtmp");
        }
        return builder->CreateNeg(operand, "negtmp");
    } else if (expr->op == "!") {
        llvm::Value* boolVal = toBool(operand);
        llvm::Value* notVal = builder->CreateNot(boolVal, "nottmp");
        return builder->CreateZExt(notVal, getDefaultType(), "booltmp");
    } else if (expr->op == "~") {
        if (operand->getType()->isDoubleTy()) {
            operand = builder->CreateFPToSI(operand, getDefaultType(), "ftoi");
        }
        return builder->CreateNot(operand, "bitnottmp");
    }

    codegenError("Unknown unary operator: " + expr->op, expr);
}

llvm::Value* CodeGenerator::generateCall(CallExpr* expr) {
    // All stdlib built-in functions are compiled to native machine code below.
    // They never use dynamic variables or the bytecode path.
    if (expr->callee == "print") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'print' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        Expression* argExpr = expr->arguments[0].get();
        llvm::Value* arg = generateExpression(argExpr);
        if (arg->getType()->isDoubleTy()) {
            // Print float value
            llvm::GlobalVariable* floatFmt = module->getGlobalVariable("print_float_fmt", true);
            if (!floatFmt) {
                floatFmt = builder->CreateGlobalString("%g\n", "print_float_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {floatFmt, arg});
            return llvm::ConstantInt::get(getDefaultType(), 0);
        } else if (arg->getType()->isPointerTy() || isStringExpr(argExpr)) {
            // Print string (literal, local string variable, or i64 holding a
            // string pointer that crossed a function boundary via ptrtoint).
            if (!arg->getType()->isPointerTy()) {
                arg = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "print.str.ptr");
            }
            llvm::GlobalVariable* strFmt = module->getGlobalVariable("print_str_fmt", true);
            if (!strFmt) {
                strFmt = builder->CreateGlobalString("%s\n", "print_str_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {strFmt, arg});
            return llvm::ConstantInt::get(getDefaultType(), 0);
        } else {
            // Print integer
            llvm::GlobalVariable* formatStr = module->getGlobalVariable("print_fmt", true);
            if (!formatStr) {
                formatStr = builder->CreateGlobalString("%lld\n", "print_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {formatStr, arg});
            return llvm::ConstantInt::get(getDefaultType(), 0);
        }
    }

    if (expr->callee == "abs") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'abs' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        if (arg->getType()->isDoubleTy()) {
            llvm::Value* fzero = llvm::ConstantFP::get(getFloatType(), 0.0);
            llvm::Value* isNeg = builder->CreateFCmpOLT(arg, fzero, "fisneg");
            llvm::Value* negVal = builder->CreateFNeg(arg, "fnegval");
            return builder->CreateSelect(isNeg, negVal, arg, "fabsval");
        }
        // abs(x) = x >= 0 ? x : -x
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* isNeg = builder->CreateICmpSLT(arg, zero, "isneg");
        llvm::Value* negVal = builder->CreateNeg(arg, "negval");
        return builder->CreateSelect(isNeg, negVal, arg, "absval");
    }

    if (expr->callee == "len") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'len' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // Array is stored as an i64 holding a pointer to [length, elem0, elem1, ...]
        // Convert to integer first if needed (e.g. if stored in a float variable)
        arg = toDefaultType(arg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "arrptr");
        return builder->CreateLoad(getDefaultType(), arrPtr, "arrlen");
    }

    if (expr->callee == "min") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'min' expects 2 arguments, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        if (a->getType()->isDoubleTy() || b->getType()->isDoubleTy()) {
            if (!a->getType()->isDoubleTy())
                a = ensureFloat(a);
            if (!b->getType()->isDoubleTy())
                b = ensureFloat(b);
            llvm::Value* cmp = builder->CreateFCmpOLT(a, b, "fmincmp");
            return builder->CreateSelect(cmp, a, b, "fminval");
        }
        llvm::Value* cmp = builder->CreateICmpSLT(a, b, "mincmp");
        return builder->CreateSelect(cmp, a, b, "minval");
    }

    if (expr->callee == "max") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'max' expects 2 arguments, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        if (a->getType()->isDoubleTy() || b->getType()->isDoubleTy()) {
            if (!a->getType()->isDoubleTy())
                a = ensureFloat(a);
            if (!b->getType()->isDoubleTy())
                b = ensureFloat(b);
            llvm::Value* cmp = builder->CreateFCmpOGT(a, b, "fmaxcmp");
            return builder->CreateSelect(cmp, a, b, "fmaxval");
        }
        llvm::Value* cmp = builder->CreateICmpSGT(a, b, "maxcmp");
        return builder->CreateSelect(cmp, a, b, "maxval");
    }

    if (expr->callee == "sign") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sign' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        if (x->getType()->isDoubleTy()) {
            llvm::Value* fzero = llvm::ConstantFP::get(getFloatType(), 0.0);
            llvm::Value* pos = llvm::ConstantInt::get(getDefaultType(), 1, true);
            llvm::Value* neg = llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(-1), true);
            llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
            llvm::Value* isNeg = builder->CreateFCmpOLT(x, fzero, "fsignneg");
            llvm::Value* negOrZero = builder->CreateSelect(isNeg, neg, zero, "fsignNZ");
            llvm::Value* isPos = builder->CreateFCmpOGT(x, fzero, "fsignpos");
            return builder->CreateSelect(isPos, pos, negOrZero, "fsignval");
        }
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
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        llvm::Value* lo = generateExpression(expr->arguments[1].get());
        llvm::Value* hi = generateExpression(expr->arguments[2].get());
        // clamp(val, lo, hi) = max(lo, min(val, hi))
        if (val->getType()->isDoubleTy() || lo->getType()->isDoubleTy() || hi->getType()->isDoubleTy()) {
            if (!val->getType()->isDoubleTy())
                val = ensureFloat(val);
            if (!lo->getType()->isDoubleTy())
                lo = ensureFloat(lo);
            if (!hi->getType()->isDoubleTy())
                hi = ensureFloat(hi);
            llvm::Value* cmpHi = builder->CreateFCmpOLT(val, hi, "fclamphi");
            llvm::Value* minVH = builder->CreateSelect(cmpHi, val, hi, "fclampmin");
            llvm::Value* cmpLo = builder->CreateFCmpOGT(minVH, lo, "fclamplo");
            return builder->CreateSelect(cmpLo, minVH, lo, "fclampval");
        }
        llvm::Value* cmpHi = builder->CreateICmpSLT(val, hi, "clamphi");
        llvm::Value* minVH = builder->CreateSelect(cmpHi, val, hi, "clampmin");
        llvm::Value* cmpLo = builder->CreateICmpSGT(minVH, lo, "clamplo");
        return builder->CreateSelect(cmpLo, minVH, lo, "clampval");
    }

    if (expr->callee == "pow") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'pow' expects 2 arguments, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* base = generateExpression(expr->arguments[0].get());
        llvm::Value* exp = generateExpression(expr->arguments[1].get());
        // Convert float arguments to integer since pow() is an integer operation
        base = toDefaultType(base);
        exp = toDefaultType(exp);

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Handle negative exponents: pow(base, negative) = 0 for |base| > 1
        llvm::BasicBlock* negExpBB = llvm::BasicBlock::Create(*context, "pow.negexp", function);
        llvm::BasicBlock* posExpBB = llvm::BasicBlock::Create(*context, "pow.posexp", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "pow.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "pow.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "pow.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* isNegExp = builder->CreateICmpSLT(exp, zero, "pow.isneg");
        builder->CreateCondBr(isNegExp, negExpBB, posExpBB);

        // Negative exponent: return 0 (integer approximation of base^(-n))
        builder->SetInsertPoint(negExpBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(posExpBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "pow.result");
        llvm::PHINode* counter = builder->CreatePHI(getDefaultType(), 2, "pow.counter");
        result->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), posExpBB);
        counter->addIncoming(exp, posExpBB);

        llvm::Value* done = builder->CreateICmpSLE(counter, zero, "pow.done.cmp");
        builder->CreateCondBr(done, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* newResult = builder->CreateMul(result, base, "pow.mul");
        llvm::Value* newCounter = builder->CreateSub(counter, llvm::ConstantInt::get(getDefaultType(), 1), "pow.dec");
        result->addIncoming(newResult, bodyBB);
        counter->addIncoming(newCounter, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* finalResult = builder->CreatePHI(getDefaultType(), 2, "pow.final");
        finalResult->addIncoming(zero, negExpBB);
        finalResult->addIncoming(result, loopBB);
        return finalResult;
    }

    if (expr->callee == "print_char") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'print_char' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        if (arg->getType()->isDoubleTy()) {
            arg = builder->CreateFPToSI(arg, getDefaultType(), "ftoi");
        }
        llvm::Value* truncated = builder->CreateTrunc(arg, llvm::Type::getInt32Ty(*context), "charval");
        builder->CreateCall(getOrDeclarePutchar(), {truncated});
        return arg; // return the character code as documented
    }

    if (expr->callee == "input") {
        if (!expr->arguments.empty()) {
            codegenError("Built-in function 'input' expects 0 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::AllocaInst* inputAlloca = createEntryBlockAlloca(function, "input_val");
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), inputAlloca);
        llvm::GlobalVariable* scanfFmt = module->getGlobalVariable("scanf_fmt", true);
        if (!scanfFmt) {
            scanfFmt = builder->CreateGlobalString("%lld", "scanf_fmt");
        }
        builder->CreateCall(getOrDeclareScanf(), {scanfFmt, inputAlloca});
        return builder->CreateLoad(getDefaultType(), inputAlloca, "input_read");
    }

    if (expr->callee == "sqrt") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sqrt' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since sqrt() is an integer square root
        x = toDefaultType(x);
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
        llvm::Value* forcedGuess =
            builder->CreateSub(guess, llvm::ConstantInt::get(getDefaultType(), 1), "sqrt.forced");
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
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_even() is an integer operation
        x = toDefaultType(x);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* bit = builder->CreateAnd(x, one, "evenbit");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isEven = builder->CreateICmpEQ(bit, zero, "iseven");
        return builder->CreateZExt(isEven, getDefaultType(), "evenval");
    }

    if (expr->callee == "is_odd") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_odd' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_odd() is an integer operation
        x = toDefaultType(x);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        return builder->CreateAnd(x, one, "oddval");
    }

    if (expr->callee == "sum") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sum' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // Array layout: [length, elem0, elem1, ...]
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "sum.arrptr");
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
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* i = generateExpression(expr->arguments[1].get());
        llvm::Value* j = generateExpression(expr->arguments[2].get());

        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "swap.arrptr");
        llvm::Value* length = builder->CreateLoad(getDefaultType(), arrPtr, "swap.len");

        // Bounds check both indices: 0 <= i < length and 0 <= j < length
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* iInBounds = builder->CreateICmpSLT(i, length, "swap.i.inbounds");
        llvm::Value* iNotNeg = builder->CreateICmpSGE(i, zero, "swap.i.notneg");
        llvm::Value* iValid = builder->CreateAnd(iInBounds, iNotNeg, "swap.i.valid");
        llvm::Value* jInBounds = builder->CreateICmpSLT(j, length, "swap.j.inbounds");
        llvm::Value* jNotNeg = builder->CreateICmpSGE(j, zero, "swap.j.notneg");
        llvm::Value* jValid = builder->CreateAnd(jInBounds, jNotNeg, "swap.j.valid");
        llvm::Value* bothValid = builder->CreateAnd(iValid, jValid, "swap.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "swap.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "swap.fail", function);
        builder->CreateCondBr(bothValid, okBB, failBB);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: swap index out of bounds\n", "swap_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
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
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "rev.arrptr");
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
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        // Returns the value itself - the integer IS the character code
        return generateExpression(expr->arguments[0].get());
    }

    if (expr->callee == "is_alpha") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_alpha' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_alpha() is an integer operation
        x = toDefaultType(x);
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
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_digit() is an integer operation
        x = toDefaultType(x);
        // is_digit: x >= '0' && x <= '9'
        llvm::Value* ge0 = builder->CreateICmpSGE(x, llvm::ConstantInt::get(getDefaultType(), 48), "ge.0");
        llvm::Value* le9 = builder->CreateICmpSLE(x, llvm::ConstantInt::get(getDefaultType(), 57), "le.9");
        llvm::Value* isDigit = builder->CreateAnd(ge0, le9, "isdigit");
        return builder->CreateZExt(isDigit, getDefaultType(), "digitval");
    }

    // typeof(x) returns 1 for integer, 2 for float, 3 for string, 0 for none.
    // In the current LLVM compilation path, all values are represented as i64,
    // so typeof() always returns 1 (integer).  In the bytecode VM path the
    // runtime Value type would be inspected instead.
    if (expr->callee == "typeof") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'typeof' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        // Evaluate the argument for its side effects, then return type tag.
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        (void)arg;
        // In the LLVM native path all values are i64, so type is always 1 (integer).
        return llvm::ConstantInt::get(getDefaultType(), 1);
    }

    // assert(condition) — aborts with an error if the condition is falsy.
    if (expr->callee == "assert") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'assert' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* condVal = generateExpression(expr->arguments[0].get());
        condVal = toBool(condVal);

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "assert.fail", function);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "assert.ok", function);

        builder->CreateCondBr(condVal, okBB, failBB);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: assertion failed\n", "assert_fail_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        return llvm::ConstantInt::get(getDefaultType(), 1);
    }

    if (expr->callee == "str_len") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'str_len' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // String may be a raw pointer (from a literal/local) or an i64 holding a pointer.
        llvm::Value* strPtr = arg->getType()->isPointerTy()
                                  ? arg
                                  : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "strlen.ptr");
        return builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strlen.result");
    }

    if (expr->callee == "char_at") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'char_at' expects 2 arguments (string, index), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* idxArg = generateExpression(expr->arguments[1].get());
        idxArg = toDefaultType(idxArg);
        // String may be a raw pointer or an i64 holding a pointer.
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "charat.ptr");

        // Bounds check: 0 <= index < str_len(s)
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "charat.strlen");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* inBounds = builder->CreateICmpSLT(idxArg, strLen, "charat.inbounds");
        llvm::Value* notNeg = builder->CreateICmpSGE(idxArg, zero, "charat.notneg");
        llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "charat.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "charat.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "charat.fail", function);
        builder->CreateCondBr(valid, okBB, failBB);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg =
            builder->CreateGlobalString("Runtime error: char_at index out of bounds\n", "charat_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Load char via GEP
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, idxArg, "charat.gep");
        llvm::Value* charVal = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "charat.char");
        // Zero-extend to i64
        return builder->CreateZExt(charVal, getDefaultType(), "charat.ext");
    }

    if (expr->callee == "str_eq") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_eq' expects 2 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* lhsArg = generateExpression(expr->arguments[0].get());
        llvm::Value* rhsArg = generateExpression(expr->arguments[1].get());
        // Convert to pointers; the value may already be a pointer (literal/local).
        llvm::Value* lhsPtr =
            lhsArg->getType()->isPointerTy()
                ? lhsArg
                : builder->CreateIntToPtr(lhsArg, llvm::PointerType::getUnqual(*context), "streq.lhs");
        llvm::Value* rhsPtr =
            rhsArg->getType()->isPointerTy()
                ? rhsArg
                : builder->CreateIntToPtr(rhsArg, llvm::PointerType::getUnqual(*context), "streq.rhs");
        llvm::Value* cmpResult = builder->CreateCall(getOrDeclareStrcmp(), {lhsPtr, rhsPtr}, "streq.cmp");
        // strcmp returns 0 on equality; convert to boolean (1 if equal, 0 otherwise)
        llvm::Value* isEqual = builder->CreateICmpEQ(cmpResult, builder->getInt32(0), "streq.eq");
        return builder->CreateZExt(isEqual, getDefaultType(), "streq.result");
    }

    if (expr->callee == "str_concat") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_concat' expects 2 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* lhsArg = generateExpression(expr->arguments[0].get());
        llvm::Value* rhsArg = generateExpression(expr->arguments[1].get());
        llvm::Value* lhsPtr =
            lhsArg->getType()->isPointerTy()
                ? lhsArg
                : builder->CreateIntToPtr(lhsArg, llvm::PointerType::getUnqual(*context), "concat.lhs");
        llvm::Value* rhsPtr =
            rhsArg->getType()->isPointerTy()
                ? rhsArg
                : builder->CreateIntToPtr(rhsArg, llvm::PointerType::getUnqual(*context), "concat.rhs");
        llvm::Value* len1 = builder->CreateCall(getOrDeclareStrlen(), {lhsPtr}, "concat.len1");
        llvm::Value* len2 = builder->CreateCall(getOrDeclareStrlen(), {rhsPtr}, "concat.len2");
        llvm::Value* totalLen = builder->CreateAdd(len1, len2, "concat.totallen");
        llvm::Value* allocSize =
            builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "concat.allocsize");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "concat.buf");
        builder->CreateCall(getOrDeclareStrcpy(), {buf, lhsPtr});
        builder->CreateCall(getOrDeclareStrcat(), {buf, rhsPtr});
        // Mark return as string-returning so callers can track it
        stringReturningFunctions_.insert("str_concat");
        return buf;
    }

    if (expr->callee == "log2") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'log2' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* n = generateExpression(expr->arguments[0].get());
        n = toDefaultType(n);
        // Integer log2 via loop: count how many times we can right-shift before reaching 0.
        // Returns -1 for n <= 0.
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "log2.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "log2.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "log2.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);

        // n <= 0 → return -1
        llvm::Value* isNonPositive = builder->CreateICmpSLE(n, zero, "log2.nonpos");
        llvm::BasicBlock* startBB = llvm::BasicBlock::Create(*context, "log2.start", function);
        builder->CreateCondBr(isNonPositive, doneBB, startBB);

        builder->SetInsertPoint(startBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* val = builder->CreatePHI(getDefaultType(), 2, "log2.val");
        val->addIncoming(n, startBB);
        llvm::PHINode* count = builder->CreatePHI(getDefaultType(), 2, "log2.count");
        count->addIncoming(negOne, startBB);

        // while val > 0: val >>= 1, count++
        llvm::Value* stillPositive = builder->CreateICmpSGT(val, zero, "log2.pos");
        builder->CreateCondBr(stillPositive, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* newVal = builder->CreateLShr(val, one, "log2.shr");
        llvm::Value* newCount = builder->CreateAdd(count, one, "log2.inc");
        val->addIncoming(newVal, bodyBB);
        count->addIncoming(newCount, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "log2.result");
        result->addIncoming(negOne, entryBB);
        result->addIncoming(count, loopBB);
        return result;
    }

    if (expr->callee == "gcd") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'gcd' expects 2 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        // Use absolute values
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* aNeg = builder->CreateICmpSLT(a, zero, "gcd.aneg");
        llvm::Value* aNegVal = builder->CreateNeg(a, "gcd.anegval");
        a = builder->CreateSelect(aNeg, aNegVal, a, "gcd.aabs");
        llvm::Value* bNeg = builder->CreateICmpSLT(b, zero, "gcd.bneg");
        llvm::Value* bNegVal = builder->CreateNeg(b, "gcd.bnegval");
        b = builder->CreateSelect(bNeg, bNegVal, b, "gcd.babs");

        // Euclidean algorithm: while (b != 0) { temp = b; b = a % b; a = temp; }
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheaderBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "gcd.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "gcd.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "gcd.done", function);

        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* phiA = builder->CreatePHI(getDefaultType(), 2, "gcd.a");
        phiA->addIncoming(a, preheaderBB);
        llvm::PHINode* phiB = builder->CreatePHI(getDefaultType(), 2, "gcd.b");
        phiB->addIncoming(b, preheaderBB);

        llvm::Value* bIsZero = builder->CreateICmpEQ(phiB, zero, "gcd.bzero");
        builder->CreateCondBr(bIsZero, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* remainder = builder->CreateSRem(phiA, phiB, "gcd.rem");
        // Ensure non-negative remainder
        llvm::Value* remNeg = builder->CreateICmpSLT(remainder, zero, "gcd.remneg");
        llvm::Value* remNegVal = builder->CreateNeg(remainder, "gcd.remnegval");
        remainder = builder->CreateSelect(remNeg, remNegVal, remainder, "gcd.remabs");
        phiA->addIncoming(phiB, bodyBB);
        phiB->addIncoming(remainder, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return phiA;
    }

    if (expr->callee == "to_string") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'to_string' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        val = toDefaultType(val);
        // Allocate buffer (21 bytes is enough for any 64-bit signed integer)
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 21);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "tostr.buf");
        // snprintf(buf, 21, "%lld", val)
        llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_fmt", true);
        if (!fmtStr) {
            fmtStr = builder->CreateGlobalString("%lld", "tostr_fmt");
        }
        builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
        stringReturningFunctions_.insert("to_string");
        return buf;
    }

    if (expr->callee == "str_find") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_find' expects 2 arguments (string, char_code), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* chArg = generateExpression(expr->arguments[1].get());
        chArg = toDefaultType(chArg);
        // String may be a raw pointer or an i64 holding a pointer.
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strfind.ptr");
        // Get string length
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strfind.len");
        // memchr(strPtr, ch, strLen)
        llvm::Value* chTrunc = builder->CreateTrunc(chArg, llvm::Type::getInt32Ty(*context), "strfind.ch32");
        llvm::Value* found = builder->CreateCall(getOrDeclareMemchr(), {strPtr, chTrunc, strLen}, "strfind.found");
        // If memchr returns null, return -1; otherwise return (found - strPtr)
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNull = builder->CreateICmpEQ(found, nullPtr, "strfind.isnull");
        llvm::Value* foundInt = builder->CreatePtrToInt(found, getDefaultType(), "strfind.foundint");
        llvm::Value* baseInt = builder->CreatePtrToInt(strPtr, getDefaultType(), "strfind.baseint");
        llvm::Value* offset = builder->CreateSub(foundInt, baseInt, "strfind.offset");
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        return builder->CreateSelect(isNull, negOne, offset, "strfind.result");
    }

    if (inOptMaxFunction) {
        // Stdlib functions are always native machine code, so they're safe to call from OPTMAX
        if (!isStdlibFunction(expr->callee) && optMaxFunctions.find(expr->callee) == optMaxFunctions.end()) {
            std::string currentFunction = "<unknown>";
            if (builder->GetInsertBlock() && builder->GetInsertBlock()->getParent()) {
                currentFunction = std::string(builder->GetInsertBlock()->getParent()->getName());
            }
            codegenError("OPTMAX function \"" + currentFunction + "\" cannot invoke non-OPTMAX function \"" +
                             expr->callee + "\"",
                         expr);
        }
    }
    auto calleeIt = functions.find(expr->callee);
    if (calleeIt == functions.end() || !calleeIt->second) {
        codegenError("Unknown function: " + expr->callee, expr);
    }
    llvm::Function* callee = calleeIt->second;

    if (callee->arg_size() != expr->arguments.size()) {
        codegenError("Function '" + expr->callee + "' expects " + std::to_string(callee->arg_size()) +
                         " argument(s), but " + std::to_string(expr->arguments.size()) + " provided",
                     expr);
    }

    std::vector<llvm::Value*> args;
    for (auto& arg : expr->arguments) {
        llvm::Value* argVal = generateExpression(arg.get());
        // Function parameters are i64, convert if needed
        argVal = toDefaultType(argVal);
        args.push_back(argVal);
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

    // Type conversion if the alloca type and value type differ
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);
    if (alloca) {
        llvm::Type* allocaType = alloca->getAllocatedType();
        if (allocaType->isDoubleTy() && value->getType()->isIntegerTy()) {
            value = builder->CreateSIToFP(value, getFloatType(), "itof");
        } else if (allocaType->isIntegerTy() && value->getType()->isDoubleTy()) {
            value = builder->CreateFPToSI(value, getDefaultType(), "ftoi");
        } else if (allocaType->isIntegerTy() && value->getType()->isPointerTy()) {
            value = builder->CreatePtrToInt(value, getDefaultType(), "ptoi");
        } else if (allocaType->isPointerTy() && value->getType()->isIntegerTy()) {
            value = builder->CreateIntToPtr(value, llvm::PointerType::getUnqual(*context), "itop");
        }
    }

    builder->CreateStore(value, it->second);
    // Update string variable tracking after assignment.
    if (value->getType()->isPointerTy() || isStringExpr(expr->value.get()))
        stringVars_.insert(expr->name);
    else
        stringVars_.erase(expr->name);
    return value;
}

// ---------------------------------------------------------------------------
// Shared prefix/postfix increment/decrement helper
// ---------------------------------------------------------------------------
// Factored out of generatePostfix() and generatePrefix() which were ~90%
// identical.  The only semantic difference is the return value: postfix
// returns the value *before* the update, prefix returns the value *after*.

llvm::Value* CodeGenerator::generateIncDec(Expression* operandExpr, const std::string& op, bool isPostfix,
                                           const ASTNode* errorNode) {
    if (op != "++" && op != "--") {
        codegenError("Unknown increment/decrement operator: " + op, errorNode);
    }

    // Handle array element increment/decrement: arr[i]++ / ++arr[i]
    auto* indexExpr = dynamic_cast<IndexExpr*>(operandExpr);
    if (indexExpr) {
        llvm::Value* arrVal = generateExpression(indexExpr->array.get());
        llvm::Value* idxVal = generateExpression(indexExpr->index.get());

        llvm::Value* arrPtr = builder->CreateIntToPtr(arrVal, llvm::PointerType::getUnqual(*context), "incdec.arrptr");

        // Bounds check
        llvm::Value* lenVal = builder->CreateLoad(getDefaultType(), arrPtr, "incdec.len");
        llvm::Value* inBounds = builder->CreateICmpSLT(idxVal, lenVal, "incdec.inbounds");
        llvm::Value* notNeg =
            builder->CreateICmpSGE(idxVal, llvm::ConstantInt::get(getDefaultType(), 0), "incdec.notneg");
        llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "incdec.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "incdec.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "incdec.fail", function);
        builder->CreateCondBr(valid, okBB, failBB);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: array index out of bounds\n", "idx_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        llvm::Value* offset = builder->CreateAdd(idxVal, llvm::ConstantInt::get(getDefaultType(), 1), "incdec.offset");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, offset, "incdec.elem.ptr");
        llvm::Value* current = builder->CreateLoad(getDefaultType(), elemPtr, "incdec.elem");

        llvm::Value* delta = llvm::ConstantInt::get(getDefaultType(), 1, true);
        llvm::Value* updated =
            (op == "++") ? builder->CreateAdd(current, delta, "inc") : builder->CreateSub(current, delta, "dec");
        builder->CreateStore(updated, elemPtr);
        return isPostfix ? current : updated;
    }

    // Handle simple variable increment/decrement
    auto* identifier = dynamic_cast<IdentifierExpr*>(operandExpr);
    if (!identifier) {
        codegenError("Increment/decrement operators require an lvalue operand", errorNode);
    }

    auto it = namedValues.find(identifier->name);
    if (it == namedValues.end() || !it->second) {
        codegenError("Unknown variable: " + identifier->name, errorNode);
    }
    checkConstModification(identifier->name, "modify");

    auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(it->second);
    llvm::Type* loadType = allocaInst ? allocaInst->getAllocatedType() : getDefaultType();
    llvm::Value* current = builder->CreateLoad(loadType, it->second, identifier->name.c_str());

    llvm::Value* updated;
    if (current->getType()->isDoubleTy()) {
        llvm::Value* one = llvm::ConstantFP::get(getFloatType(), 1.0);
        updated = (op == "++") ? builder->CreateFAdd(current, one, "finc") : builder->CreateFSub(current, one, "fdec");
    } else {
        llvm::Value* delta = llvm::ConstantInt::get(getDefaultType(), 1, true);
        updated = (op == "++") ? builder->CreateAdd(current, delta, "inc") : builder->CreateSub(current, delta, "dec");
    }

    builder->CreateStore(updated, it->second);
    return isPostfix ? current : updated;
}

llvm::Value* CodeGenerator::generatePostfix(PostfixExpr* expr) {
    return generateIncDec(expr->operand.get(), expr->op, /*isPostfix=*/true, expr);
}

llvm::Value* CodeGenerator::generatePrefix(PrefixExpr* expr) {
    return generateIncDec(expr->operand.get(), expr->op, /*isPostfix=*/false, expr);
}

llvm::Value* CodeGenerator::generateTernary(TernaryExpr* expr) {
    llvm::Value* condition = generateExpression(expr->condition.get());
    llvm::Value* condBool = toBool(condition);

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(*context, "tern.then", function);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(*context, "tern.else", function);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "tern.cont", function);

    builder->CreateCondBr(condBool, thenBB, elseBB);

    builder->SetInsertPoint(thenBB);
    llvm::Value* thenVal = generateExpression(expr->thenExpr.get());
    thenBB = builder->GetInsertBlock();

    builder->SetInsertPoint(elseBB);
    llvm::Value* elseVal = generateExpression(expr->elseExpr.get());
    elseBB = builder->GetInsertBlock();

    // Ensure matching types for PHI node
    if (thenVal->getType() != elseVal->getType()) {
        if (thenVal->getType()->isDoubleTy() || elseVal->getType()->isDoubleTy()) {
            if (!thenVal->getType()->isDoubleTy()) {
                builder->SetInsertPoint(thenBB);
                thenVal = ensureFloat(thenVal);
            }
            if (!elseVal->getType()->isDoubleTy()) {
                builder->SetInsertPoint(elseBB);
                elseVal = ensureFloat(elseVal);
            }
        }
    }

    builder->SetInsertPoint(thenBB);
    builder->CreateBr(mergeBB);
    thenBB = builder->GetInsertBlock();

    builder->SetInsertPoint(elseBB);
    builder->CreateBr(mergeBB);
    elseBB = builder->GetInsertBlock();

    builder->SetInsertPoint(mergeBB);
    llvm::PHINode* phi = builder->CreatePHI(thenVal->getType(), 2, "ternval");
    phi->addIncoming(thenVal, thenBB);
    phi->addIncoming(elseVal, elseBB);

    return phi;
}

llvm::Value* CodeGenerator::generateArray(ArrayExpr* expr) {
    size_t numElements = expr->elements.size();
    // Allocate space for (1 + numElements) i64 slots: [length, elem0, elem1, ...]
    size_t totalSlots = 1 + numElements;

    // Use heap allocation (malloc) so that arrays survive function returns.
    // Stack allocation (alloca) would be invalidated when the enclosing
    // function returns, causing use-after-free when the caller accesses the
    // array through the returned pointer-as-i64.
    llvm::Value* byteSize = llvm::ConstantInt::get(getDefaultType(), totalSlots * 8);
    llvm::Value* arrPtr = builder->CreateCall(getOrDeclareMalloc(), {byteSize}, "arr");

    // Store the length in slot 0 (arrPtr points to slot 0)
    builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), numElements), arrPtr);

    // Store each element in slots 1..N
    for (size_t i = 0; i < numElements; i++) {
        llvm::Value* elemVal = generateExpression(expr->elements[i].get());
        // Array slots are i64, so convert if needed
        elemVal = toDefaultType(elemVal);
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr,
                                                  llvm::ConstantInt::get(getDefaultType(), i + 1), "arr.elem.ptr");
        builder->CreateStore(elemVal, elemPtr);
    }

    // Return the array pointer as an i64 for the dynamic type system
    return builder->CreatePtrToInt(arrPtr, getDefaultType(), "arr.int");
}

llvm::Value* CodeGenerator::generateIndex(IndexExpr* expr) {
    llvm::Value* arrVal = generateExpression(expr->array.get());
    llvm::Value* idxVal = generateExpression(expr->index.get());

    // Convert the i64 back to a pointer
    llvm::Value* arrPtr = builder->CreateIntToPtr(arrVal, llvm::PointerType::getUnqual(*context), "idx.arrptr");

    // Bounds check: load length from slot 0, verify 0 <= index < length
    llvm::Value* lenVal = builder->CreateLoad(getDefaultType(), arrPtr, "idx.len");
    llvm::Value* inBounds = builder->CreateICmpSLT(idxVal, lenVal, "idx.inbounds");
    llvm::Value* notNeg = builder->CreateICmpSGE(idxVal, llvm::ConstantInt::get(getDefaultType(), 0), "idx.notneg");
    llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "idx.valid");

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "idx.ok", function);
    llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "idx.fail", function);

    builder->CreateCondBr(valid, okBB, failBB);

    // Out-of-bounds path: print error and abort
    builder->SetInsertPoint(failBB);
    llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: array index out of bounds\n", "idx_oob_msg");
    builder->CreateCall(getPrintfFunction(), {errMsg});
    builder->CreateCall(getOrDeclareAbort());
    builder->CreateUnreachable();

    // Success path: load element at offset (index + 1)
    builder->SetInsertPoint(okBB);
    llvm::Value* offset = builder->CreateAdd(idxVal, llvm::ConstantInt::get(getDefaultType(), 1), "idx.offset");
    llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, offset, "idx.elem.ptr");
    return builder->CreateLoad(getDefaultType(), elemPtr, "idx.elem");
}

llvm::Value* CodeGenerator::generateIndexAssign(IndexAssignExpr* expr) {
    llvm::Value* arrVal = generateExpression(expr->array.get());
    llvm::Value* idxVal = generateExpression(expr->index.get());
    llvm::Value* newVal = generateExpression(expr->value.get());
    newVal = toDefaultType(newVal);

    // Convert the i64 back to a pointer
    llvm::Value* arrPtr = builder->CreateIntToPtr(arrVal, llvm::PointerType::getUnqual(*context), "idxa.arrptr");

    // Bounds check: load length from slot 0, verify 0 <= index < length
    llvm::Value* lenVal = builder->CreateLoad(getDefaultType(), arrPtr, "idxa.len");
    llvm::Value* inBounds = builder->CreateICmpSLT(idxVal, lenVal, "idxa.inbounds");
    llvm::Value* notNeg = builder->CreateICmpSGE(idxVal, llvm::ConstantInt::get(getDefaultType(), 0), "idxa.notneg");
    llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "idxa.valid");

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "idxa.ok", function);
    llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "idxa.fail", function);

    builder->CreateCondBr(valid, okBB, failBB);

    // Out-of-bounds path: print error and abort
    builder->SetInsertPoint(failBB);
    llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: array index out of bounds\n", "idx_oob_msg");
    builder->CreateCall(getPrintfFunction(), {errMsg});
    builder->CreateCall(getOrDeclareAbort());
    builder->CreateUnreachable();

    // Success path: store element at offset (index + 1)
    builder->SetInsertPoint(okBB);
    llvm::Value* offset = builder->CreateAdd(idxVal, llvm::ConstantInt::get(getDefaultType(), 1), "idxa.offset");
    llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, offset, "idxa.elem.ptr");
    builder->CreateStore(newVal, elemPtr);
    return newVal;
}

void CodeGenerator::generateVarDecl(VarDecl* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("Variable declaration outside of function", stmt);
    }

    llvm::Type* allocaType = getDefaultType();
    llvm::Value* initValue = nullptr;

    if (stmt->initializer) {
        initValue = generateExpression(stmt->initializer.get());
        allocaType = initValue->getType();
    }

    llvm::AllocaInst* alloca = createEntryBlockAlloca(function, stmt->name, allocaType);
    bindVariable(stmt->name, alloca, stmt->isConst);

    if (initValue) {
        builder->CreateStore(initValue, alloca);
        // Track whether this variable holds a string value so that print(),
        // concatenation, and comparison operators handle it correctly when
        // the variable's alloca type is i64 (e.g. assigned from a function
        // call that returns a string as i64 via ptrtoint).
        if (initValue->getType()->isPointerTy() || isStringExpr(stmt->initializer.get()))
            stringVars_.insert(stmt->name);
        else
            stringVars_.erase(stmt->name);
    } else {
        builder->CreateStore(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)), alloca);
    }
}

void CodeGenerator::generateReturn(ReturnStmt* stmt) {
    if (stmt->value) {
        llvm::Value* retValue = generateExpression(stmt->value.get());
        // Record that the current function returns a string value so that
        // callers can use isStringExpr() on the CallExpr and track the result.
        if (retValue->getType()->isPointerTy() || isStringExpr(stmt->value.get())) {
            if (builder->GetInsertBlock() && builder->GetInsertBlock()->getParent())
                stringReturningFunctions_.insert(std::string(builder->GetInsertBlock()->getParent()->getName()));
        }
        // Function return type is i64, so convert if needed
        retValue = toDefaultType(retValue);
        builder->CreateRet(retValue);
    } else {
        builder->CreateRet(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)));
    }
}

void CodeGenerator::generateIf(IfStmt* stmt) {
    llvm::Value* condition = generateExpression(stmt->condition.get());

    // Constant condition elimination: skip dead branch when condition is known
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(condition)) {
        if (ci->isZero()) {
            // Condition is false: only generate else branch if present
            if (stmt->elseBranch) {
                generateStatement(stmt->elseBranch.get());
            }
        } else {
            // Condition is true: only generate then branch
            generateStatement(stmt->thenBranch.get());
        }
        return;
    }

    llvm::Value* condBool = toBool(condition);

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
    llvm::Value* condBool = toBool(condition);
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
    llvm::Value* condBool = toBool(condition);
    builder->CreateCondBr(condBool, bodyBB, endBB);

    // End block
    builder->SetInsertPoint(endBB);

    endScope();
}

void CodeGenerator::generateFor(ForStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("For loop outside of function", stmt);
    }

    beginScope();

    // Allocate iterator variable
    llvm::AllocaInst* iterAlloca = createEntryBlockAlloca(function, stmt->iteratorVar);
    bindVariable(stmt->iteratorVar, iterAlloca);

    // Initialize iterator
    llvm::Value* startVal = generateExpression(stmt->start.get());
    // For-loop iterator is always integer, convert to integer
    startVal = toDefaultType(startVal);
    builder->CreateStore(startVal, iterAlloca);

    // Get end value
    llvm::Value* endVal = generateExpression(stmt->end.get());
    // Convert to integer since loop bounds are always integer
    endVal = toDefaultType(endVal);

    // Get step value (default to 1 if not specified)
    llvm::Value* stepVal;
    if (stmt->step) {
        stepVal = generateExpression(stmt->step.get());
        // Convert to integer since loop step is always integer
        stepVal = toDefaultType(stepVal);
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
    std::string errorMessage = "Runtime error: for-loop step cannot be zero for iterator '" + stmt->iteratorVar + "'\n";
    llvm::GlobalVariable* messageVar = builder->CreateGlobalString(errorMessage, "forstepmsg");
    llvm::Constant* zeroIndex = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0);
    llvm::Constant* indices[] = {zeroIndex, zeroIndex};
    llvm::Constant* message =
        llvm::ConstantExpr::getInBoundsGetElementPtr(messageVar->getValueType(), messageVar, indices);
    builder->CreateCall(getPrintfFunction(), {message});
    builder->CreateCall(getOrDeclareAbort());
    builder->CreateUnreachable();
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

void CodeGenerator::generateForEach(ForEachStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("For-each loop outside of function", stmt);
    }

    beginScope();

    // Evaluate the collection (array)
    llvm::Value* collVal = generateExpression(stmt->collection.get());
    collVal = toDefaultType(collVal);

    // Convert i64 to pointer — array layout is [length, elem0, elem1, ...]
    llvm::Value* arrPtr = builder->CreateIntToPtr(collVal, llvm::PointerType::getUnqual(*context), "foreach.arrptr");

    // Load length from slot 0
    llvm::Value* lenVal = builder->CreateLoad(getDefaultType(), arrPtr, "foreach.len");

    // Allocate hidden index variable and the user's iterator variable
    llvm::AllocaInst* idxAlloca = createEntryBlockAlloca(function, "_foreach_idx");
    builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), idxAlloca);

    llvm::AllocaInst* iterAlloca = createEntryBlockAlloca(function, stmt->iteratorVar);
    bindVariable(stmt->iteratorVar, iterAlloca);

    // Create blocks
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "foreach.cond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "foreach.body", function);
    llvm::BasicBlock* incBB = llvm::BasicBlock::Create(*context, "foreach.inc", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "foreach.end", function);

    builder->CreateBr(condBB);

    // Condition: idx < length
    builder->SetInsertPoint(condBB);
    llvm::Value* curIdx = builder->CreateLoad(getDefaultType(), idxAlloca, "foreach.idx");
    llvm::Value* cond = builder->CreateICmpSLT(curIdx, lenVal, "foreach.cmp");
    builder->CreateCondBr(cond, bodyBB, endBB);

    // Body: load current element into iterator variable, then execute body
    builder->SetInsertPoint(bodyBB);
    llvm::Value* bodyIdx = builder->CreateLoad(getDefaultType(), idxAlloca, "foreach.bidx");
    llvm::Value* offset = builder->CreateAdd(bodyIdx, llvm::ConstantInt::get(getDefaultType(), 1), "foreach.offset");
    llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, offset, "foreach.elem.ptr");
    llvm::Value* elemVal = builder->CreateLoad(getDefaultType(), elemPtr, "foreach.elem");
    builder->CreateStore(elemVal, iterAlloca);

    loopStack.push_back({endBB, incBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(incBB);
    }

    // Increment hidden index
    builder->SetInsertPoint(incBB);
    llvm::Value* nextIdx = builder->CreateLoad(getDefaultType(), idxAlloca, "foreach.nidx");
    llvm::Value* incIdx = builder->CreateAdd(nextIdx, llvm::ConstantInt::get(getDefaultType(), 1), "foreach.next");
    builder->CreateStore(incIdx, idxAlloca);
    builder->CreateBr(condBB);

    // End
    builder->SetInsertPoint(endBB);

    endScope();
}

void CodeGenerator::generateBlock(BlockStmt* stmt) {
    beginScope();
    for (auto& statement : stmt->statements) {
        if (builder->GetInsertBlock()->getTerminator()) {
            break; // Don't generate unreachable code
        }
        generateStatement(statement.get());
    }
    endScope();
}

void CodeGenerator::generateExprStmt(ExprStmt* stmt) {
    generateExpression(stmt->expression.get());
}

void CodeGenerator::generateSwitch(SwitchStmt* stmt) {
    llvm::Value* condVal = generateExpression(stmt->condition.get());
    condVal = toDefaultType(condVal);

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "switch.end", function);
    llvm::BasicBlock* defaultBB = mergeBB; // fall through to merge if no default

    // Find the default case block first so the switch instruction can reference it.
    for (auto& sc : stmt->cases) {
        if (sc.isDefault) {
            defaultBB = llvm::BasicBlock::Create(*context, "switch.default", function, mergeBB);
            break;
        }
    }

    llvm::SwitchInst* switchInst = builder->CreateSwitch(condVal, defaultBB, static_cast<unsigned>(stmt->cases.size()));

    // Push a loop context so that 'break' inside a case arm exits the switch
    // (matching C/C++ semantics where break leaves the nearest switch or loop).
    loopStack.push_back({mergeBB, nullptr});

    // Track seen case values to detect duplicates.
    std::set<int64_t> seenCaseValues;

    for (auto& sc : stmt->cases) {
        if (sc.isDefault) {
            // Generate default block body.
            builder->SetInsertPoint(defaultBB);
            beginScope();
            for (auto& s : sc.body) {
                generateStatement(s.get());
                if (builder->GetInsertBlock()->getTerminator())
                    break;
            }
            endScope();
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }
        } else {
            llvm::Value* caseVal = generateExpression(sc.value.get());
            if (caseVal->getType()->isDoubleTy()) {
                codegenError("case value must be an integer constant, not a float", sc.value.get());
            }
            caseVal = toDefaultType(caseVal);
            auto* caseConst = llvm::dyn_cast<llvm::ConstantInt>(caseVal);
            if (!caseConst) {
                codegenError("case value must be a compile-time integer constant", sc.value.get());
            }

            int64_t cv = caseConst->getSExtValue();
            if (!seenCaseValues.insert(cv).second) {
                codegenError("duplicate case value " + std::to_string(cv) + " in switch statement", sc.value.get());
            }

            llvm::BasicBlock* caseBB = llvm::BasicBlock::Create(*context, "switch.case", function, mergeBB);
            switchInst->addCase(caseConst, caseBB);

            builder->SetInsertPoint(caseBB);
            beginScope();
            for (auto& s : sc.body) {
                generateStatement(s.get());
                if (builder->GetInsertBlock()->getTerminator())
                    break;
            }
            endScope();
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }
        }
    }

    loopStack.pop_back();

    builder->SetInsertPoint(mergeBB);
}

void CodeGenerator::resolveTargetCPU(std::string& cpu, std::string& features) const {
    bool isNative = marchCpu_.empty() || marchCpu_ == "native";
    if (isNative) {
        cpu = llvm::sys::getHostCPUName().str();
        llvm::SubtargetFeatures featureSet;
#if LLVM_VERSION_MAJOR >= 19
        llvm::StringMap<bool> hostFeatures = llvm::sys::getHostCPUFeatures();
#else
        llvm::StringMap<bool> hostFeatures;
        llvm::sys::getHostCPUFeatures(hostFeatures);
#endif
        for (auto& feature : hostFeatures) {
            featureSet.AddFeature(feature.first(), feature.second);
        }
        features = featureSet.getString();
    } else {
        // Use the specified CPU; LLVM derives features from the CPU name.
        cpu = marchCpu_;
        features = "";
    }

    // -mtune overrides the CPU used for scheduling when -march is native or
    // unset.  When an explicit -march is given, it takes precedence because
    // LLVM's createTargetMachine uses a single CPU parameter for both
    // instruction selection and scheduling.
    if (!mtuneCpu_.empty() && isNative) {
        cpu = mtuneCpu_;
    }
}

// ---------------------------------------------------------------------------
// Shared TargetMachine construction
// ---------------------------------------------------------------------------
// Both runOptimizationPasses() and writeObjectFile() need a configured
// TargetMachine.  This helper consolidates the duplicated triple setup,
// target lookup, TargetOptions configuration, and version-conditional
// createTargetMachine() call into a single place.

std::unique_ptr<llvm::TargetMachine> CodeGenerator::createTargetMachine() const {
    std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
#endif

    std::string error;
#if LLVM_VERSION_MAJOR >= 19
    auto* target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
#else
    auto* target = llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
#endif
    if (!target)
        return nullptr;

    llvm::TargetOptions opt;
    if (useFastMath_) {
        opt.UnsafeFPMath = true;
        opt.NoInfsFPMath = true;
        opt.NoNaNsFPMath = true;
        opt.NoSignedZerosFPMath = true;
    }
    std::optional<llvm::Reloc::Model> RM = usePIC_ ? llvm::Reloc::PIC_ : llvm::Reloc::Static;

    std::string cpu;
    std::string features;
    resolveTargetCPU(cpu, features);

#if LLVM_VERSION_MAJOR >= 19
    return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(targetTriple, cpu, features, opt, RM));
#else
    return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(targetTripleStr, cpu, features, opt, RM));
#endif
}

void CodeGenerator::runOptimizationPasses() {
    // Set the target triple on the module.
    std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
    module->setTargetTriple(targetTriple);
#else
    module->setTargetTriple(targetTripleStr);
#endif

    auto targetMachine = createTargetMachine();
    if (targetMachine) {
        module->setDataLayout(targetMachine->createDataLayout());
    }

    if (optimizationLevel == OptimizationLevel::O0) {
        return;
    }

    // O1 uses a lightweight legacy function pass pipeline (no IPO).
    if (optimizationLevel == OptimizationLevel::O1) {
        llvm::legacy::FunctionPassManager fpm(module.get());
        fpm.add(llvm::createPromoteMemoryToRegisterPass());
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createReassociatePass());
        fpm.add(llvm::createGVNPass());
        fpm.add(llvm::createCFGSimplificationPass());
        fpm.add(llvm::createDeadCodeEliminationPass());
        fpm.doInitialization();
        for (auto& func : module->functions()) {
            if (!func.isDeclaration()) {
                fpm.run(func);
            }
        }
        fpm.doFinalization();
        return;
    }

    // O2 and O3 use the new pass manager's standard pipeline which includes
    // interprocedural optimizations: function inlining, IPSCCP (sparse
    // conditional constant propagation), GlobalDCE (dead function removal),
    // jump threading, correlated value propagation, and more.
    llvm::PassBuilder PB(targetMachine.get());
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::OptimizationLevel newPMLevel =
        (optimizationLevel == OptimizationLevel::O3) ? llvm::OptimizationLevel::O3 : llvm::OptimizationLevel::O2;
    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(newPMLevel);
    MPM.run(*module, MAM);
}

void CodeGenerator::optimizeFunction(llvm::Function* func) {
    // Per-function optimization for targeted optimization of individual functions.
    // This allows selectively optimizing specific functions (e.g., hot functions
    // identified at compile time, or OPTMAX-annotated functions) without running
    // the full module-wide optimization pipeline.
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
    fpm.add(llvm::createLoopSimplifyPass());
    fpm.add(llvm::createLoopStrengthReducePass());
    fpm.add(llvm::createLoopUnrollPass());
    // Phase 3: Post-loop optimizations
    fpm.add(llvm::createSinkingPass());
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

    auto targetMachine = createTargetMachine();
    if (!targetMachine) {
        throw std::runtime_error("Failed to create target machine");
    }

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
    // Detect errors during close (e.g. I/O errors that occurred during write).
    if (dest.has_error()) {
        std::string errMsg = "Error writing object file: " + dest.error().message();
        dest.clear_error();
        throw std::runtime_error(errMsg);
    }
}

void CodeGenerator::generateBytecode(Program* program) {
    bytecodeNextReg_ = 0;
    bytecodeLocalBase_ = 0;
    for (auto& func : program->functions) {
        if (func->name == "main") {
            emitBytecodeBlock(func->body.get());
            uint8_t rs = allocReg();
            bytecodeEmitter.emit(OpCode::PUSH_INT);
            bytecodeEmitter.emitReg(rs);
            bytecodeEmitter.emitInt(0);
            bytecodeEmitter.emit(OpCode::RETURN);
            bytecodeEmitter.emitReg(rs);
        }
    }
    bytecodeEmitter.emit(OpCode::HALT);
}

// ---------------------------------------------------------------------------
// Hybrid code generation
// ---------------------------------------------------------------------------

void CodeGenerator::emitBytecodeForFunction(FunctionDecl* func) {
    // Save and reset emitter + local variable state.
    BytecodeEmitter savedEmitter = std::move(bytecodeEmitter);
    auto savedLocals = std::move(bytecodeLocals_);
    uint8_t savedNextLocal = bytecodeNextLocal_;
    uint8_t savedNextReg = bytecodeNextReg_;
    uint8_t savedLocalBase = bytecodeLocalBase_;

    bytecodeEmitter = BytecodeEmitter();
    bytecodeLocals_.clear();
    bytecodeNextLocal_ = 0;
    bytecodeNextReg_ = 0;
    bytecodeLocalBase_ = 0;

    // Bind parameters as local variables at indices 0..arity-1.
    for (auto& param : func->parameters) {
        bytecodeLocals_[param.name] = bytecodeNextLocal_++;
    }

    emitBytecodeBlock(func->body.get());

    // Ensure the function always returns (implicit return 0).
    uint8_t rs = allocReg();
    bytecodeEmitter.emit(OpCode::PUSH_INT);
    bytecodeEmitter.emitReg(rs);
    bytecodeEmitter.emitInt(0);
    bytecodeEmitter.emit(OpCode::RETURN);
    bytecodeEmitter.emitReg(rs);

    CompiledBytecodeFunc compiled;
    compiled.name = func->name;
    compiled.arity = static_cast<uint8_t>(func->parameters.size());
    compiled.bytecode = bytecodeEmitter.getCode();
    bytecodeFunctions_.push_back(std::move(compiled));

    // Restore saved state.
    bytecodeEmitter = std::move(savedEmitter);
    bytecodeLocals_ = std::move(savedLocals);
    bytecodeNextLocal_ = savedNextLocal;
    bytecodeNextReg_ = savedNextReg;
    bytecodeLocalBase_ = savedLocalBase;
}

void CodeGenerator::generateHybrid(Program* program) {
    // Phase 1: Run the normal AOT pipeline which classifies all functions,
    // forward-declares them in LLVM, and generates IR for every function.
    generate(program);

    // Phase 2: For each Interpreted-tier function, also emit bytecode so
    // the VM can execute it (or later JIT-compile it).  If bytecode
    // emission fails (e.g. unsupported statement type), the function
    // remains AOT-only — no bytecode is produced for it.
    for (auto& func : program->functions) {
        if (functionTiers[func->name] == ExecutionTier::Interpreted) {
            try {
                emitBytecodeForFunction(func.get());
            } catch (const std::runtime_error&) {
                // Bytecode emission failed — keep function as AOT-only.
                // This can happen for functions using features not yet
                // supported in the bytecode emitter (e.g. break in switch).
                functionTiers[func->name] = ExecutionTier::AOT;
            }
        }
    }
}

// Helper: emit LOAD_LOCAL or LOAD_VAR depending on whether name is a known local.
uint8_t CodeGenerator::emitBytecodeLoad(const std::string& name) {
    uint8_t rd = allocReg();
    auto it = bytecodeLocals_.find(name);
    if (it != bytecodeLocals_.end()) {
        bytecodeEmitter.emit(OpCode::LOAD_LOCAL);
        bytecodeEmitter.emitReg(rd);
        bytecodeEmitter.emitByte(it->second);
    } else {
        bytecodeEmitter.emit(OpCode::LOAD_VAR);
        bytecodeEmitter.emitReg(rd);
        bytecodeEmitter.emitString(name);
    }
    return rd;
}

// Helper: emit STORE_LOCAL or STORE_VAR depending on whether name is a known local.
void CodeGenerator::emitBytecodeStore(const std::string& name, uint8_t rs) {
    auto it = bytecodeLocals_.find(name);
    if (it != bytecodeLocals_.end()) {
        bytecodeEmitter.emit(OpCode::STORE_LOCAL);
        bytecodeEmitter.emitByte(it->second);
        bytecodeEmitter.emitReg(rs);
    } else {
        bytecodeEmitter.emit(OpCode::STORE_VAR);
        bytecodeEmitter.emitReg(rs);
        bytecodeEmitter.emitString(name);
    }
}

uint8_t CodeGenerator::emitBytecodeExpression(Expression* expr) {
    switch (expr->type) {
    case ASTNodeType::LITERAL_EXPR: {
        auto* lit = static_cast<LiteralExpr*>(expr);
        uint8_t rd = allocReg();
        if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
            bytecodeEmitter.emit(OpCode::PUSH_INT);
            bytecodeEmitter.emitReg(rd);
            bytecodeEmitter.emitInt(lit->intValue);
        } else if (lit->literalType == LiteralExpr::LiteralType::FLOAT) {
            bytecodeEmitter.emit(OpCode::PUSH_FLOAT);
            bytecodeEmitter.emitReg(rd);
            bytecodeEmitter.emitFloat(lit->floatValue);
        } else if (lit->literalType == LiteralExpr::LiteralType::STRING) {
            bytecodeEmitter.emit(OpCode::PUSH_STRING);
            bytecodeEmitter.emitReg(rd);
            bytecodeEmitter.emitString(lit->stringValue);
        }
        return rd;
    }
    case ASTNodeType::IDENTIFIER_EXPR: {
        auto* id = static_cast<IdentifierExpr*>(expr);
        return emitBytecodeLoad(id->name);
    }
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<BinaryExpr*>(expr);
        // Bytecode constant folding: if both operands are integer literals,
        // evaluate at compile time and emit a single PUSH_INT.
        auto* leftLit =
            (bin->left->type == ASTNodeType::LITERAL_EXPR) ? static_cast<LiteralExpr*>(bin->left.get()) : nullptr;
        auto* rightLit =
            (bin->right->type == ASTNodeType::LITERAL_EXPR) ? static_cast<LiteralExpr*>(bin->right.get()) : nullptr;
        if (leftLit && rightLit && leftLit->literalType == LiteralExpr::LiteralType::INTEGER &&
            rightLit->literalType == LiteralExpr::LiteralType::INTEGER) {
            long long lv = leftLit->intValue;
            long long rv = rightLit->intValue;
            bool folded = true;
            long long result = 0;
            if (bin->op == "+")
                result = lv + rv;
            else if (bin->op == "-")
                result = lv - rv;
            else if (bin->op == "*")
                result = lv * rv;
            else if (bin->op == "/" && rv != 0)
                result = lv / rv;
            else if (bin->op == "%" && rv != 0)
                result = lv % rv;
            else if (bin->op == "&")
                result = lv & rv;
            else if (bin->op == "|")
                result = lv | rv;
            else if (bin->op == "^")
                result = lv ^ rv;
            else if (bin->op == "<<" && rv >= 0 && rv < 64)
                result = lv << rv;
            else if (bin->op == ">>" && rv >= 0 && rv < 64)
                result = lv >> rv;
            else if (bin->op == "**") {
                if (rv >= 0) {
                    result = 1;
                    for (long long i = 0; i < rv; i++)
                        result *= lv;
                } else {
                    result = 0;
                }
            } else if (bin->op == "==")
                result = static_cast<long long>(lv == rv);
            else if (bin->op == "!=")
                result = static_cast<long long>(lv != rv);
            else if (bin->op == "<")
                result = static_cast<long long>(lv < rv);
            else if (bin->op == "<=")
                result = static_cast<long long>(lv <= rv);
            else if (bin->op == ">")
                result = static_cast<long long>(lv > rv);
            else if (bin->op == ">=")
                result = static_cast<long long>(lv >= rv);
            else if (bin->op == "&&")
                result = static_cast<long long>((lv != 0) && (rv != 0));
            else if (bin->op == "||")
                result = static_cast<long long>((lv != 0) || (rv != 0));
            else
                folded = false;
            if (folded) {
                uint8_t rd = allocReg();
                bytecodeEmitter.emit(OpCode::PUSH_INT);
                bytecodeEmitter.emitReg(rd);
                bytecodeEmitter.emitInt(result);
                return rd;
            }
        }
        // Bytecode algebraic identity: when exactly one operand is a literal
        // integer with a known identity value, emit the other operand directly
        // instead of generating an unnecessary arithmetic instruction.
        // Note: when the result is a constant (e.g. x*0→0), we still evaluate
        // the non-literal operand to preserve any side effects it may have.
        if (leftLit && leftLit->literalType == LiteralExpr::LiteralType::INTEGER) {
            long long lv = leftLit->intValue;
            if (lv == 0 && (bin->op == "+" || bin->op == "|" || bin->op == "^"))
                return emitBytecodeExpression(bin->right.get()); // 0+x, 0|x, 0^x → x
            if ((lv == 0) && (bin->op == "*" || bin->op == "&")) {
                emitBytecodeExpression(bin->right.get()); // evaluate for side effects
                uint8_t rd = allocReg();                  // 0*x, 0&x → 0
                bytecodeEmitter.emit(OpCode::PUSH_INT);
                bytecodeEmitter.emitReg(rd);
                bytecodeEmitter.emitInt(0);
                return rd;
            }
            if (lv == 1 && bin->op == "*")
                return emitBytecodeExpression(bin->right.get()); // 1*x → x
        }
        if (rightLit && rightLit->literalType == LiteralExpr::LiteralType::INTEGER) {
            long long rv = rightLit->intValue;
            if (rv == 0 && (bin->op == "+" || bin->op == "-" || bin->op == "|" || bin->op == "^" || bin->op == "<<" ||
                            bin->op == ">>"))
                return emitBytecodeExpression(bin->left.get()); // x+0, x-0, x|0, x^0, x<<0, x>>0 → x
            if ((rv == 0) && (bin->op == "*" || bin->op == "&")) {
                emitBytecodeExpression(bin->left.get()); // evaluate for side effects
                uint8_t rd = allocReg();                 // x*0, x&0 → 0
                bytecodeEmitter.emit(OpCode::PUSH_INT);
                bytecodeEmitter.emitReg(rd);
                bytecodeEmitter.emitInt(0);
                return rd;
            }
            if (rv == 1 && (bin->op == "*" || bin->op == "/" || bin->op == "**"))
                return emitBytecodeExpression(bin->left.get()); // x*1, x/1, x**1 → x
            if (rv == 0 && bin->op == "**") {
                emitBytecodeExpression(bin->left.get()); // evaluate for side effects
                uint8_t rd = allocReg();                 // x**0 → 1
                bytecodeEmitter.emit(OpCode::PUSH_INT);
                bytecodeEmitter.emitReg(rd);
                bytecodeEmitter.emitInt(1);
                return rd;
            }
        }
        uint8_t rs1 = emitBytecodeExpression(bin->left.get());
        uint8_t rs2 = emitBytecodeExpression(bin->right.get());
        uint8_t rd = allocReg();
        if (bin->op == "+")
            bytecodeEmitter.emit(OpCode::ADD);
        else if (bin->op == "-")
            bytecodeEmitter.emit(OpCode::SUB);
        else if (bin->op == "*")
            bytecodeEmitter.emit(OpCode::MUL);
        else if (bin->op == "/")
            bytecodeEmitter.emit(OpCode::DIV);
        else if (bin->op == "%")
            bytecodeEmitter.emit(OpCode::MOD);
        else if (bin->op == "==")
            bytecodeEmitter.emit(OpCode::EQ);
        else if (bin->op == "!=")
            bytecodeEmitter.emit(OpCode::NE);
        else if (bin->op == "<")
            bytecodeEmitter.emit(OpCode::LT);
        else if (bin->op == "<=")
            bytecodeEmitter.emit(OpCode::LE);
        else if (bin->op == ">")
            bytecodeEmitter.emit(OpCode::GT);
        else if (bin->op == ">=")
            bytecodeEmitter.emit(OpCode::GE);
        else if (bin->op == "&&")
            bytecodeEmitter.emit(OpCode::AND);
        else if (bin->op == "||")
            bytecodeEmitter.emit(OpCode::OR);
        else if (bin->op == "&")
            bytecodeEmitter.emit(OpCode::BIT_AND);
        else if (bin->op == "|")
            bytecodeEmitter.emit(OpCode::BIT_OR);
        else if (bin->op == "^")
            bytecodeEmitter.emit(OpCode::BIT_XOR);
        else if (bin->op == "<<")
            bytecodeEmitter.emit(OpCode::SHL);
        else if (bin->op == ">>")
            bytecodeEmitter.emit(OpCode::SHR);
        else if (bin->op == "**")
            bytecodeEmitter.emit(OpCode::POW);
        else {
            throw std::runtime_error("Unsupported binary operator in bytecode: " + bin->op);
        }
        bytecodeEmitter.emitReg(rd);
        bytecodeEmitter.emitReg(rs1);
        bytecodeEmitter.emitReg(rs2);
        return rd;
    }
    case ASTNodeType::UNARY_EXPR: {
        auto* unary = static_cast<UnaryExpr*>(expr);
        // Bytecode constant folding for unary on integer literals.
        auto* operandLit = (unary->operand->type == ASTNodeType::LITERAL_EXPR)
                               ? static_cast<LiteralExpr*>(unary->operand.get())
                               : nullptr;
        if (operandLit && operandLit->literalType == LiteralExpr::LiteralType::INTEGER) {
            long long val = operandLit->intValue;
            bool folded = true;
            long long result = 0;
            if (unary->op == "-")
                result = -val;
            else if (unary->op == "!")
                result = (val == 0) ? 1 : 0;
            else if (unary->op == "~")
                result = ~val;
            else
                folded = false;
            if (folded) {
                uint8_t rd = allocReg();
                bytecodeEmitter.emit(OpCode::PUSH_INT);
                bytecodeEmitter.emitReg(rd);
                bytecodeEmitter.emitInt(result);
                return rd;
            }
        }
        uint8_t rs = emitBytecodeExpression(unary->operand.get());
        uint8_t rd = allocReg();
        if (unary->op == "-")
            bytecodeEmitter.emit(OpCode::NEG);
        else if (unary->op == "!")
            bytecodeEmitter.emit(OpCode::NOT);
        else if (unary->op == "~")
            bytecodeEmitter.emit(OpCode::BIT_NOT);
        else {
            throw std::runtime_error("Unsupported unary operator in bytecode: " + unary->op);
        }
        bytecodeEmitter.emitReg(rd);
        bytecodeEmitter.emitReg(rs);
        return rd;
    }
    case ASTNodeType::ASSIGN_EXPR: {
        auto* assign = static_cast<AssignExpr*>(expr);
        uint8_t rs = emitBytecodeExpression(assign->value.get());
        emitBytecodeStore(assign->name, rs);
        return rs;
    }
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<CallExpr*>(expr);
        if (call->callee == "print") {
            for (auto& arg : call->arguments) {
                uint8_t rs = emitBytecodeExpression(arg.get());
                bytecodeEmitter.emit(OpCode::PRINT);
                bytecodeEmitter.emitReg(rs);
            }
            uint8_t rd = allocReg();
            bytecodeEmitter.emit(OpCode::PUSH_INT);
            bytecodeEmitter.emitReg(rd);
            bytecodeEmitter.emitInt(0);
            return rd;
        }
        if (isStdlibFunction(call->callee)) {
            throw std::runtime_error("Stdlib function '" + call->callee +
                                     "' must be compiled to native code, not bytecode");
        }
        std::vector<uint8_t> argRegs;
        for (auto& arg : call->arguments) {
            argRegs.push_back(emitBytecodeExpression(arg.get()));
        }
        uint8_t rd = allocReg();
        bytecodeEmitter.emit(OpCode::CALL);
        bytecodeEmitter.emitReg(rd);
        bytecodeEmitter.emitString(call->callee);
        bytecodeEmitter.emitByte(static_cast<uint8_t>(call->arguments.size()));
        for (uint8_t reg : argRegs) {
            bytecodeEmitter.emitReg(reg);
        }
        return rd;
    }
    case ASTNodeType::POSTFIX_EXPR: {
        auto* postfix = static_cast<PostfixExpr*>(expr);
        auto* id = dynamic_cast<IdentifierExpr*>(postfix->operand.get());
        if (!id) {
            throw std::runtime_error("Postfix operator requires an identifier in bytecode");
        }
        uint8_t origReg = emitBytecodeLoad(id->name);
        uint8_t oneReg = allocReg();
        bytecodeEmitter.emit(OpCode::PUSH_INT);
        bytecodeEmitter.emitReg(oneReg);
        bytecodeEmitter.emitInt(1);
        uint8_t newReg = allocReg();
        bytecodeEmitter.emit(postfix->op == "++" ? OpCode::ADD : OpCode::SUB);
        bytecodeEmitter.emitReg(newReg);
        bytecodeEmitter.emitReg(origReg);
        bytecodeEmitter.emitReg(oneReg);
        emitBytecodeStore(id->name, newReg);
        return origReg;
    }
    case ASTNodeType::PREFIX_EXPR: {
        auto* prefix = static_cast<PrefixExpr*>(expr);
        auto* id = dynamic_cast<IdentifierExpr*>(prefix->operand.get());
        if (!id) {
            throw std::runtime_error("Prefix operator requires an identifier in bytecode");
        }
        uint8_t origReg = emitBytecodeLoad(id->name);
        uint8_t oneReg = allocReg();
        bytecodeEmitter.emit(OpCode::PUSH_INT);
        bytecodeEmitter.emitReg(oneReg);
        bytecodeEmitter.emitInt(1);
        uint8_t newReg = allocReg();
        bytecodeEmitter.emit(prefix->op == "++" ? OpCode::ADD : OpCode::SUB);
        bytecodeEmitter.emitReg(newReg);
        bytecodeEmitter.emitReg(origReg);
        bytecodeEmitter.emitReg(oneReg);
        emitBytecodeStore(id->name, newReg);
        return newReg;
    }
    case ASTNodeType::TERNARY_EXPR: {
        auto* ternary = static_cast<TernaryExpr*>(expr);
        uint8_t condReg = emitBytecodeExpression(ternary->condition.get());
        bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
        bytecodeEmitter.emitReg(condReg);
        size_t elsePatch = bytecodeEmitter.currentOffset();
        bytecodeEmitter.emitShort(0);

        uint8_t thenReg = emitBytecodeExpression(ternary->thenExpr.get());
        uint8_t resultReg = thenReg;
        bytecodeEmitter.emit(OpCode::JUMP);
        size_t endPatch = bytecodeEmitter.currentOffset();
        bytecodeEmitter.emitShort(0);

        bytecodeEmitter.patchJump(elsePatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
        uint8_t elseReg = emitBytecodeExpression(ternary->elseExpr.get());
        if (elseReg != resultReg) {
            bytecodeEmitter.emit(OpCode::MOV);
            bytecodeEmitter.emitReg(resultReg);
            bytecodeEmitter.emitReg(elseReg);
        }
        bytecodeEmitter.patchJump(endPatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
        return resultReg;
    }
    case ASTNodeType::INDEX_ASSIGN_EXPR:
        throw std::runtime_error("Array index assignment is not supported in bytecode mode");
    default:
        throw std::runtime_error("Unsupported expression type in bytecode generation");
    }
}

void CodeGenerator::emitBytecodeStatement(Statement* stmt) {
    switch (stmt->type) {
    case ASTNodeType::EXPR_STMT: {
        auto* exprStmt = static_cast<ExprStmt*>(stmt);
        emitBytecodeExpression(exprStmt->expression.get());
        // No POP needed in register-based mode
        break;
    }
    case ASTNodeType::VAR_DECL: {
        auto* varDecl = static_cast<VarDecl*>(stmt);
        uint8_t rs;
        if (varDecl->initializer) {
            rs = emitBytecodeExpression(varDecl->initializer.get());
        } else {
            rs = allocReg();
            bytecodeEmitter.emit(OpCode::PUSH_INT);
            bytecodeEmitter.emitReg(rs);
            bytecodeEmitter.emitInt(0);
        }
        if (isInBytecodeFunctionContext()) {
            if (bytecodeNextLocal_ == 255) {
                throw std::runtime_error("Too many local variables in function (max 255)");
            }
            uint8_t idx = bytecodeNextLocal_++;
            bytecodeLocals_[varDecl->name] = idx;
        }
        emitBytecodeStore(varDecl->name, rs);
        break;
    }
    case ASTNodeType::RETURN_STMT: {
        auto* retStmt = static_cast<ReturnStmt*>(stmt);
        uint8_t rs;
        if (retStmt->value) {
            rs = emitBytecodeExpression(retStmt->value.get());
        } else {
            rs = allocReg();
            bytecodeEmitter.emit(OpCode::PUSH_INT);
            bytecodeEmitter.emitReg(rs);
            bytecodeEmitter.emitInt(0);
        }
        bytecodeEmitter.emit(OpCode::RETURN);
        bytecodeEmitter.emitReg(rs);
        break;
    }
    case ASTNodeType::IF_STMT: {
        auto* ifStmt = static_cast<IfStmt*>(stmt);
        uint8_t condReg = emitBytecodeExpression(ifStmt->condition.get());

        bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
        bytecodeEmitter.emitReg(condReg);
        size_t elsePatch = bytecodeEmitter.currentOffset();
        bytecodeEmitter.emitShort(0);

        emitBytecodeStatement(ifStmt->thenBranch.get());

        if (ifStmt->elseBranch) {
            bytecodeEmitter.emit(OpCode::JUMP);
            size_t endPatch = bytecodeEmitter.currentOffset();
            bytecodeEmitter.emitShort(0);

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

        uint8_t condReg = emitBytecodeExpression(whileStmt->condition.get());
        bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
        bytecodeEmitter.emitReg(condReg);
        size_t exitPatch = bytecodeEmitter.currentOffset();
        bytecodeEmitter.emitShort(0);

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

        uint8_t condReg = emitBytecodeExpression(doWhileStmt->condition.get());
        uint8_t notReg = allocReg();
        bytecodeEmitter.emit(OpCode::NOT);
        bytecodeEmitter.emitReg(notReg);
        bytecodeEmitter.emitReg(condReg);
        bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
        bytecodeEmitter.emitReg(notReg);
        bytecodeEmitter.emitShort(static_cast<uint16_t>(loopStart));
        break;
    }
    case ASTNodeType::FOR_STMT: {
        auto* forStmt = static_cast<ForStmt*>(stmt);
        if (isInBytecodeFunctionContext()) {
            if (bytecodeLocals_.find(forStmt->iteratorVar) == bytecodeLocals_.end()) {
                bytecodeLocals_[forStmt->iteratorVar] = bytecodeNextLocal_++;
            }
        }

        // Evaluate step before the loop so the direction check can use it.
        uint8_t stepReg;
        if (forStmt->step) {
            stepReg = emitBytecodeExpression(forStmt->step.get());
        } else {
            stepReg = allocReg();
            bytecodeEmitter.emit(OpCode::PUSH_INT);
            bytecodeEmitter.emitReg(stepReg);
            bytecodeEmitter.emitInt(1);
        }
        // Store the step in a hidden variable so it's available each iteration.
        std::string stepVar = "__for_step_" + forStmt->iteratorVar + "__";
        if (isInBytecodeFunctionContext()) {
            if (bytecodeLocals_.find(stepVar) == bytecodeLocals_.end()) {
                bytecodeLocals_[stepVar] = bytecodeNextLocal_++;
            }
        }
        emitBytecodeStore(stepVar, stepReg);

        uint8_t startReg = emitBytecodeExpression(forStmt->start.get());
        emitBytecodeStore(forStmt->iteratorVar, startReg);

        size_t loopStart = bytecodeEmitter.currentOffset();

        uint8_t iterReg = emitBytecodeLoad(forStmt->iteratorVar);
        uint8_t endReg = emitBytecodeExpression(forStmt->end.get());

        // Direction-aware condition: use LT for positive step, GT for negative.
        // Compute: (step > 0 && iter < end) || (step <= 0 && iter > end)
        uint8_t loadedStep = emitBytecodeLoad(stepVar);
        uint8_t zeroReg = allocReg();
        bytecodeEmitter.emit(OpCode::PUSH_INT);
        bytecodeEmitter.emitReg(zeroReg);
        bytecodeEmitter.emitInt(0);

        uint8_t stepPosReg = allocReg();
        bytecodeEmitter.emit(OpCode::GT);
        bytecodeEmitter.emitReg(stepPosReg);
        bytecodeEmitter.emitReg(loadedStep);
        bytecodeEmitter.emitReg(zeroReg);

        uint8_t fwdReg = allocReg();
        bytecodeEmitter.emit(OpCode::LT);
        bytecodeEmitter.emitReg(fwdReg);
        bytecodeEmitter.emitReg(iterReg);
        bytecodeEmitter.emitReg(endReg);

        uint8_t bwdReg = allocReg();
        bytecodeEmitter.emit(OpCode::GT);
        bytecodeEmitter.emitReg(bwdReg);
        bytecodeEmitter.emitReg(iterReg);
        bytecodeEmitter.emitReg(endReg);

        uint8_t fwdMasked = allocReg();
        bytecodeEmitter.emit(OpCode::AND);
        bytecodeEmitter.emitReg(fwdMasked);
        bytecodeEmitter.emitReg(stepPosReg);
        bytecodeEmitter.emitReg(fwdReg);

        uint8_t notStepPos = allocReg();
        bytecodeEmitter.emit(OpCode::NOT);
        bytecodeEmitter.emitReg(notStepPos);
        bytecodeEmitter.emitReg(stepPosReg);

        uint8_t bwdMasked = allocReg();
        bytecodeEmitter.emit(OpCode::AND);
        bytecodeEmitter.emitReg(bwdMasked);
        bytecodeEmitter.emitReg(notStepPos);
        bytecodeEmitter.emitReg(bwdReg);

        uint8_t cmpReg = allocReg();
        bytecodeEmitter.emit(OpCode::OR);
        bytecodeEmitter.emitReg(cmpReg);
        bytecodeEmitter.emitReg(fwdMasked);
        bytecodeEmitter.emitReg(bwdMasked);

        bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
        bytecodeEmitter.emitReg(cmpReg);
        size_t exitPatch = bytecodeEmitter.currentOffset();
        bytecodeEmitter.emitShort(0);

        emitBytecodeStatement(forStmt->body.get());

        uint8_t curReg = emitBytecodeLoad(forStmt->iteratorVar);
        uint8_t curStep = emitBytecodeLoad(stepVar);
        uint8_t newIterReg = allocReg();
        bytecodeEmitter.emit(OpCode::ADD);
        bytecodeEmitter.emitReg(newIterReg);
        bytecodeEmitter.emitReg(curReg);
        bytecodeEmitter.emitReg(curStep);
        emitBytecodeStore(forStmt->iteratorVar, newIterReg);

        bytecodeEmitter.emit(OpCode::JUMP);
        bytecodeEmitter.emitShort(static_cast<uint16_t>(loopStart));

        bytecodeEmitter.patchJump(exitPatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
        break;
    }
    case ASTNodeType::SWITCH_STMT: {
        auto* switchStmt = static_cast<SwitchStmt*>(stmt);
        // Use member variable instead of function-local static for
        // thread-safety and deterministic output across instances.
        std::string tempVar = "__switch_cond_" + std::to_string(bytecodeSwitchCounter_++) + "__";
        if (isInBytecodeFunctionContext()) {
            bytecodeLocals_[tempVar] = bytecodeNextLocal_++;
        }
        uint8_t condReg = emitBytecodeExpression(switchStmt->condition.get());
        emitBytecodeStore(tempVar, condReg);

        std::vector<size_t> endPatches;
        const SwitchCase* defaultCase = nullptr;

        for (auto& sc : switchStmt->cases) {
            if (sc.isDefault) {
                defaultCase = &sc;
            } else {
                uint8_t loadedCond = emitBytecodeLoad(tempVar);
                uint8_t caseValReg = emitBytecodeExpression(sc.value.get());
                uint8_t eqReg = allocReg();
                bytecodeEmitter.emit(OpCode::EQ);
                bytecodeEmitter.emitReg(eqReg);
                bytecodeEmitter.emitReg(loadedCond);
                bytecodeEmitter.emitReg(caseValReg);
                bytecodeEmitter.emit(OpCode::JUMP_IF_FALSE);
                bytecodeEmitter.emitReg(eqReg);
                size_t skipPatch = bytecodeEmitter.currentOffset();
                bytecodeEmitter.emitShort(0);

                for (auto& s : sc.body) {
                    emitBytecodeStatement(s.get());
                }
                bytecodeEmitter.emit(OpCode::JUMP);
                endPatches.push_back(bytecodeEmitter.currentOffset());
                bytecodeEmitter.emitShort(0);

                bytecodeEmitter.patchJump(skipPatch, static_cast<uint16_t>(bytecodeEmitter.currentOffset()));
            }
        }

        if (defaultCase) {
            for (auto& s : defaultCase->body) {
                emitBytecodeStatement(s.get());
            }
        }

        auto endOffset = static_cast<uint16_t>(bytecodeEmitter.currentOffset());
        for (size_t patch : endPatches) {
            bytecodeEmitter.patchJump(patch, endOffset);
        }
        break;
    }
    case ASTNodeType::FOR_EACH_STMT:
        throw std::runtime_error("For-each loops are not supported in bytecode mode");
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
