#include "codegen.h"
#include "diagnostic.h"
#include "egraph.h"
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/PGOOptions.h>
#include <llvm/Support/TargetSelect.h>
#if LLVM_VERSION_MAJOR < 22
#include <llvm/Support/VirtualFileSystem.h>
#endif
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
#include <llvm/Transforms/Scalar/LoopDistribute.h>
#include <llvm/Transforms/Utils.h>
#include <optional>
#include <stdexcept>

// LLVM 21 removed Attribute::NoCapture in favour of the captures(...) attribute.
#if LLVM_VERSION_MAJOR >= 21
#define OMSC_ADD_NOCAPTURE(fn, idx)                                                                                    \
    (fn)->addParamAttr((idx), llvm::Attribute::getWithCaptureInfo((fn)->getContext(), llvm::CaptureInfo::none()))
#else
#define OMSC_ADD_NOCAPTURE(fn, idx) (fn)->addParamAttr((idx), llvm::Attribute::NoCapture)
#endif

// LLVM 19 introduced getOrInsertDeclaration; older versions only have getDeclaration.
#if LLVM_VERSION_MAJOR >= 19
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getOrInsertDeclaration
#else
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getDeclaration
#endif

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
using omscript::IdentifierExpr;
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

/// Concurrency builtin names.  User functions that call any of these must NOT
/// be marked nosync/nofree/willreturn because those attributes promise to the
/// optimizer that the function performs no synchronization, never frees memory,
/// and always returns — all of which are violated by pthreads primitives.
static const std::unordered_set<std::string> kConcurrencyBuiltins = {"thread_create", "thread_join",  "mutex_new",
                                                                     "mutex_lock",    "mutex_unlock", "mutex_destroy"};

/// Recursively check if an expression tree contains a call to any name in @p names.
static bool exprCallsAny(const Expression* e, const std::unordered_set<std::string>& names) {
    if (!e)
        return false;
    switch (e->type) {
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(e);
        if (names.count(call->callee))
            return true;
        for (auto& arg : call->arguments)
            if (exprCallsAny(arg.get(), names))
                return true;
        return false;
    }
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(e);
        return exprCallsAny(bin->left.get(), names) || exprCallsAny(bin->right.get(), names);
    }
    case ASTNodeType::UNARY_EXPR:
        return exprCallsAny(static_cast<const UnaryExpr*>(e)->operand.get(), names);
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const TernaryExpr*>(e);
        return exprCallsAny(tern->condition.get(), names) || exprCallsAny(tern->thenExpr.get(), names) ||
               exprCallsAny(tern->elseExpr.get(), names);
    }
    case ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<const IndexExpr*>(e);
        return exprCallsAny(idx->array.get(), names) || exprCallsAny(idx->index.get(), names);
    }
    case ASTNodeType::INDEX_ASSIGN_EXPR: {
        auto* ia = static_cast<const IndexAssignExpr*>(e);
        return exprCallsAny(ia->array.get(), names) || exprCallsAny(ia->index.get(), names) ||
               exprCallsAny(ia->value.get(), names);
    }
    case ASTNodeType::ASSIGN_EXPR:
        return exprCallsAny(static_cast<const AssignExpr*>(e)->value.get(), names);
    default:
        return false;
    }
}

/// Recursively check if a statement tree contains a call to any name in @p names.
static bool stmtCallsAny(const Statement* s, const std::unordered_set<std::string>& names) {
    if (!s)
        return false;
    switch (s->type) {
    case ASTNodeType::BLOCK: {
        auto* blk = static_cast<const BlockStmt*>(s);
        for (auto& st : blk->statements)
            if (stmtCallsAny(st.get(), names))
                return true;
        return false;
    }
    case ASTNodeType::RETURN_STMT:
        return exprCallsAny(static_cast<const omscript::ReturnStmt*>(s)->value.get(), names);
    case ASTNodeType::EXPR_STMT:
        return exprCallsAny(static_cast<const ExprStmt*>(s)->expression.get(), names);
    case ASTNodeType::VAR_DECL:
        return exprCallsAny(static_cast<const VarDecl*>(s)->initializer.get(), names);
    case ASTNodeType::IF_STMT: {
        auto* ifS = static_cast<const IfStmt*>(s);
        return exprCallsAny(ifS->condition.get(), names) || stmtCallsAny(ifS->thenBranch.get(), names) ||
               stmtCallsAny(ifS->elseBranch.get(), names);
    }
    case ASTNodeType::WHILE_STMT: {
        auto* wh = static_cast<const WhileStmt*>(s);
        return exprCallsAny(wh->condition.get(), names) || stmtCallsAny(wh->body.get(), names);
    }
    case ASTNodeType::DO_WHILE_STMT: {
        auto* dw = static_cast<const DoWhileStmt*>(s);
        return stmtCallsAny(dw->body.get(), names) || exprCallsAny(dw->condition.get(), names);
    }
    case ASTNodeType::FOR_STMT: {
        auto* fr = static_cast<const ForStmt*>(s);
        return exprCallsAny(fr->start.get(), names) || exprCallsAny(fr->end.get(), names) ||
               exprCallsAny(fr->step.get(), names) || stmtCallsAny(fr->body.get(), names);
    }
    case ASTNodeType::FOR_EACH_STMT: {
        auto* fe = static_cast<const ForEachStmt*>(s);
        return exprCallsAny(fe->collection.get(), names) || stmtCallsAny(fe->body.get(), names);
    }
    default:
        return false;
    }
}

/// Returns true if the function body contains calls to any concurrency builtin.
static bool usesConcurrencyPrimitive(const omscript::FunctionDecl* func) {
    if (!func->body)
        return false;
    return stmtCallsAny(func->body.get(), kConcurrencyBuiltins);
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
        const long long value = literal->intValue;
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
        const double value = literal->floatValue;
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
        const long long lval = leftLiteral->intValue;
        if (lval == 0 && op == "+")
            return right; // 0 + x → x
        if (lval == 0 && op == "-")
            return std::make_unique<UnaryExpr>("-", std::move(right)); // 0 - x → -x
        if (lval == 0 && (op == "*" || op == "&") && isPureExpression(right.get()))
            return std::make_unique<LiteralExpr>(static_cast<long long>(0)); // 0 * x, 0 & x → 0
        if (lval == 0 && (op == "|" || op == "^"))
            return right; // 0 | x, 0 ^ x → x
        if (lval == 1 && op == "*")
            return right; // 1 * x → x
        if (lval == 1 && op == "**" && isPureExpression(right.get()))
            return std::make_unique<LiteralExpr>(static_cast<long long>(1)); // 1 ** x → 1
        if (lval == -1 && op == "*")
            return std::make_unique<UnaryExpr>("-", std::move(right)); // -1 * x → -x
    }
    if (rightLiteral && rightLiteral->literalType == LiteralExpr::LiteralType::INTEGER) {
        const long long rval = rightLiteral->intValue;
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
        if (rval == 1 && op == "%")
            return std::make_unique<LiteralExpr>(static_cast<long long>(0)); // x % 1 → 0
        if (rval == -1 && op == "*")
            return std::make_unique<UnaryExpr>("-", std::move(left)); // x * -1 → -x
        if (rval == -1 && op == "/")
            return std::make_unique<UnaryExpr>("-", std::move(left)); // x / -1 → -x
        if (rval == 0 && op == "**" && isPureExpression(left.get()))
            return std::make_unique<LiteralExpr>(static_cast<long long>(1)); // x ** 0 → 1
    }

    // Self-identifier optimizations: when both sides are the same identifier
    // (pure, no side effects), several operations simplify to constants.
    {
        auto* leftId = dynamic_cast<IdentifierExpr*>(left.get());
        auto* rightId = dynamic_cast<IdentifierExpr*>(right.get());
        if (leftId && rightId && leftId->name == rightId->name) {
            // x - x → 0, x ^ x → 0
            if (op == "-" || op == "^")
                return std::make_unique<LiteralExpr>(static_cast<long long>(0));
            // x & x → x, x | x → x
            if (op == "&" || op == "|") {
                auto result = std::make_unique<IdentifierExpr>(leftId->name);
                return result;
            }
            // x == x → 1, x <= x → 1, x >= x → 1
            if (op == "==" || op == "<=" || op == ">=")
                return std::make_unique<LiteralExpr>(static_cast<long long>(1));
            // x != x → 0, x < x → 0, x > x → 0
            if (op == "!=" || op == "<" || op == ">")
                return std::make_unique<LiteralExpr>(static_cast<long long>(0));
            // x / x → 1 (safe: OPTMAX requires the user to guarantee no division by zero)
            if (op == "/")
                return std::make_unique<LiteralExpr>(static_cast<long long>(1));
            // x % x → 0
            if (op == "%")
                return std::make_unique<LiteralExpr>(static_cast<long long>(0));
        }
    }

    if (!leftLiteral || !rightLiteral) {
        return std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    if (leftLiteral->literalType == LiteralExpr::LiteralType::INTEGER &&
        rightLiteral->literalType == LiteralExpr::LiteralType::INTEGER) {
        const long long lval = leftLiteral->intValue;
        const long long rval = rightLiteral->intValue;
        if (op == "+")
            return std::make_unique<LiteralExpr>(lval + rval);
        if (op == "-")
            return std::make_unique<LiteralExpr>(lval - rval);
        if (op == "*")
            return std::make_unique<LiteralExpr>(lval * rval);
        if (op == "/" && rval != 0 && (lval != LLONG_MIN || rval != -1))
            return std::make_unique<LiteralExpr>(lval / rval);
        if (op == "%" && rval != 0 && (lval != LLONG_MIN || rval != -1))
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
                bool overflow = false;
                for (long long i = 0; i < rval; i++) {
                    if (lval != 0 && lval != 1 && lval != -1) {
                        const uint64_t ab = (lval < 0) ? static_cast<uint64_t>(-static_cast<uint64_t>(lval))
                                                 : static_cast<uint64_t>(lval);
                        const uint64_t ar = (result < 0) ? static_cast<uint64_t>(-static_cast<uint64_t>(result))
                                                   : static_cast<uint64_t>(result);
                        if (ar > static_cast<uint64_t>(LLONG_MAX) / ab) {
                            overflow = true;
                            break;
                        }
                    }
                    result *= lval;
                }
                if (!overflow)
                    return std::make_unique<LiteralExpr>(result);
                // Fall through to emit a runtime BinaryExpr on overflow.
            } else {
                // Negative exponent: base**(-n) = 1 / base**n in integer math.
                // |base| > 1 → truncates to 0; base=1 → 1; base=-1 → ±1.
                if (lval == 1)
                    return std::make_unique<LiteralExpr>(static_cast<long long>(1));
                if (lval == -1)
                    return std::make_unique<LiteralExpr>(static_cast<long long>((rval & 1) ? -1 : 1));
                return std::make_unique<LiteralExpr>(static_cast<long long>(0));
            }
        }
    } else if (leftLiteral->literalType == LiteralExpr::LiteralType::FLOAT &&
               rightLiteral->literalType == LiteralExpr::LiteralType::FLOAT) {
        const double lval = leftLiteral->floatValue;
        const double rval = rightLiteral->floatValue;
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
        const double leftDouble = (leftLiteral->literalType == LiteralExpr::LiteralType::FLOAT)
                                ? leftLiteral->floatValue
                                : static_cast<double>(leftLiteral->intValue);
        const double rightDouble = (rightLiteral->literalType == LiteralExpr::LiteralType::FLOAT)
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
            for (auto& v : sc.values) {
                v = optimizeOptMaxExpression(std::move(v));
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
// These functions are always compiled to native machine code via LLVM IR.
static const std::unordered_set<std::string> stdlibFunctions = {"abs",
                                                                "acos",
                                                                "array_any",
                                                                "array_concat",
                                                                "array_contains",
                                                                "array_copy",
                                                                "array_count",
                                                                "array_every",
                                                                "array_fill",
                                                                "array_filter",
                                                                "array_find",
                                                                "array_map",
                                                                "array_max",
                                                                "array_min",
                                                                "array_reduce",
                                                                "array_remove",
                                                                "array_slice",
                                                                "asin",
                                                                "assert",
                                                                "atan",
                                                                "atan2",
                                                                "cbrt",
                                                                "ceil",
                                                                "char_at",
                                                                "char_code",
                                                                "clamp",
                                                                "cos",
                                                                "exit_program",
                                                                "exit",
                                                                "exp",
                                                                "fast_add",
                                                                "fast_div",
                                                                "fast_mul",
                                                                "fast_sub",
                                                                "file_append",
                                                                "file_exists",
                                                                "file_read",
                                                                "file_write",
                                                                "floor",
                                                                "gcd",
                                                                "hypot",
                                                                "index_of",
                                                                "input",
                                                                "input_line",
                                                                "is_alpha",
                                                                "is_digit",
                                                                "is_even",
                                                                "is_odd",
                                                                "len",
                                                                "log",
                                                                "log10",
                                                                "log2",
                                                                "map_get",
                                                                "map_has",
                                                                "map_keys",
                                                                "map_new",
                                                                "map_remove",
                                                                "map_set",
                                                                "map_size",
                                                                "map_values",
                                                                "max",
                                                                "min",
                                                                "number_to_string",
                                                                "pop",
                                                                "pow",
                                                                "precise_add",
                                                                "precise_div",
                                                                "precise_mul",
                                                                "precise_sub",
                                                                "print",
                                                                "print_char",
                                                                "println",
                                                                "push",
                                                                "random",
                                                                "range",
                                                                "range_step",
                                                                "reverse",
                                                                "round",
                                                                "sign",
                                                                "sin",
                                                                "sleep",
                                                                "sort",
                                                                "sqrt",
                                                                "str_chars",
                                                                "str_concat",
                                                                "str_contains",
                                                                "str_count",
                                                                "str_ends_with",
                                                                "str_eq",
                                                                "str_find",
                                                                "str_index_of",
                                                                "str_join",
                                                                "str_len",
                                                                "str_lower",
                                                                "str_repeat",
                                                                "str_replace",
                                                                "str_reverse",
                                                                "str_split",
                                                                "str_starts_with",
                                                                "str_substr",
                                                                "str_to_float",
                                                                "str_to_int",
                                                                "str_trim",
                                                                "str_upper",
                                                                "string_to_number",
                                                                "sum",
                                                                "swap",
                                                                "tan",
                                                                "time",
                                                                "to_char",
                                                                "to_float",
                                                                "to_int",
                                                                "to_string",
                                                                "thread_create",
                                                                "thread_join",
                                                                "mutex_new",
                                                                "mutex_lock",
                                                                "mutex_unlock",
                                                                "mutex_destroy",
                                                                "typeof",
                                                                "write"};

bool isStdlibFunction(const std::string& name) {
    return stdlibFunctions.find(name) != stdlibFunctions.end();
}

CodeGenerator::CodeGenerator(OptimizationLevel optLevel)
    : inOptMaxFunction(false), hasOptMaxFunctions(false), optimizationLevel(optLevel) {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("omscript", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    setupPrintfDeclaration();
    initTBAAMetadata();
}

CodeGenerator::~CodeGenerator() = default;

// ---------------------------------------------------------------------------
// TBAA metadata initialization
// ---------------------------------------------------------------------------

void CodeGenerator::initTBAAMetadata() {
    // Build a TBAA type hierarchy so LLVM knows that different memory regions
    // are distinct types.  This allows LLVM to freely reorder loads/stores
    // across different categories and hoist invariant loads out of loops.
    //
    // LLVM scalar TBAA format (path-based):
    //   Root:    !{ !"label" }
    //   Type:    !{ !"label", !root, i64 0 }        (0 = may-alias-others)
    //   Access:  !{ !type, !type, i64 0 }            (offset 0, not constant)
    //
    // Hierarchy:
    //   OmScript TBAA (root)
    //   ├── array length      — slot 0 of arrays/maps
    //   ├── array element     — slots 1+ of arrays
    //   ├── struct field      — struct field loads/stores
    //   ├── string data       — string character byte accesses
    //   ├── map key           — key slots in map layout
    //   └── map value         — value slots in map layout
    auto& C = *context;
    tbaaRoot_ = llvm::MDNode::get(C, {llvm::MDString::get(C, "OmScript TBAA")});

    auto makeTBAAType = [&](const char* name) -> llvm::MDNode* {
        return llvm::MDNode::get(C, {
            llvm::MDString::get(C, name),
            tbaaRoot_,
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(C), 0))
        });
    };

    llvm::MDNode* tbaaLenType    = makeTBAAType("array length");
    llvm::MDNode* tbaaElemType   = makeTBAAType("array element");
    llvm::MDNode* tbaaStructType = makeTBAAType("struct field");
    llvm::MDNode* tbaaStrType    = makeTBAAType("string data");
    llvm::MDNode* tbaaMapKeyType = makeTBAAType("map key");
    llvm::MDNode* tbaaMapValType = makeTBAAType("map value");
    llvm::MDNode* tbaaMapHashType = makeTBAAType("map hash");

    // Access tag nodes: !{ !base-type, !access-type, offset }
    // For scalar types, base == access.
    auto* zero = llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(C), 0));
    tbaaArrayLen_    = llvm::MDNode::get(C, {tbaaLenType, tbaaLenType, zero});
    tbaaArrayElem_   = llvm::MDNode::get(C, {tbaaElemType, tbaaElemType, zero});
    tbaaStructField_ = llvm::MDNode::get(C, {tbaaStructType, tbaaStructType, zero});
    tbaaStringData_  = llvm::MDNode::get(C, {tbaaStrType, tbaaStrType, zero});
    tbaaMapKey_      = llvm::MDNode::get(C, {tbaaMapKeyType, tbaaMapKeyType, zero});
    tbaaMapVal_      = llvm::MDNode::get(C, {tbaaMapValType, tbaaMapValType, zero});
    tbaaMapHash_     = llvm::MDNode::get(C, {tbaaMapHashType, tbaaMapHashType, zero});

    // !range metadata: array lengths are always in [0, INT64_MAX).
    auto* i64Ty = llvm::Type::getInt64Ty(C);
    arrayLenRangeMD_ = llvm::MDNode::get(C, {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
            i64Ty, static_cast<uint64_t>(INT64_MAX)))
    });
}

void CodeGenerator::setupPrintfDeclaration() {
    // Declare printf function for output
    std::vector<llvm::Type*> printfArgs;
    printfArgs.push_back(llvm::PointerType::getUnqual(*context));

    llvm::FunctionType* printfType = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), printfArgs, true);

    auto* fn = llvm::Function::Create(printfType, llvm::Function::ExternalLinkage, "printf", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // printf reads through its format string arg (nocapture, readonly).
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
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

llvm::Type* CodeGenerator::resolveAnnotatedType(const std::string& annotation) {
    // Strip reference prefix '&' — borrowed references share the same
    // underlying type (e.g., &i32 → i32).
    std::string ann = annotation;
    if (!ann.empty() && ann[0] == '&') {
        ann = ann.substr(1);
    }
    if (ann == "float" || ann == "double")
        return getFloatType();                                  // f64
    if (ann == "bool")
        return llvm::Type::getInt1Ty(*context);                 // i1
    if (ann == "i8" || ann == "u8")
        return llvm::Type::getInt8Ty(*context);                 // i8/u8
    if (ann == "i16" || ann == "u16")
        return llvm::Type::getInt16Ty(*context);                // i16/u16
    if (ann == "i32" || ann == "u32")
        return llvm::Type::getInt32Ty(*context);                // i32/u32
    // -----------------------------------------------------------------------
    // SIMD vector types — map to LLVM fixed-vector types for handwritten SIMD
    // -----------------------------------------------------------------------
    if (ann == "f32x4")
        return llvm::FixedVectorType::get(llvm::Type::getFloatTy(*context), 4);
    if (ann == "f32x8")
        return llvm::FixedVectorType::get(llvm::Type::getFloatTy(*context), 8);
    if (ann == "f64x2")
        return llvm::FixedVectorType::get(llvm::Type::getDoubleTy(*context), 2);
    if (ann == "f64x4")
        return llvm::FixedVectorType::get(llvm::Type::getDoubleTy(*context), 4);
    if (ann == "i32x4")
        return llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*context), 4);
    if (ann == "i32x8")
        return llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*context), 8);
    if (ann == "i64x2")
        return llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*context), 2);
    if (ann == "i64x4")
        return llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*context), 4);
    // "int", "i64", "u64", "string", array types, struct names, generics,
    // and empty annotations all map to the default i64 representation.
    return getDefaultType();
}

llvm::Value* CodeGenerator::convertTo(llvm::Value* v, llvm::Type* targetTy) {
    if (v->getType() == targetTy)
        return v;
    // float → int
    if (v->getType()->isDoubleTy() && targetTy->isIntegerTy())
        return builder->CreateFPToSI(v, targetTy, "ftoi");
    // int → float
    if (v->getType()->isIntegerTy() && targetTy->isDoubleTy())
        return builder->CreateSIToFP(v, targetTy, "itof");
    // ptr → int
    if (v->getType()->isPointerTy() && targetTy->isIntegerTy())
        return builder->CreatePtrToInt(v, targetTy, "ptoi");
    // int → ptr
    if (v->getType()->isIntegerTy() && targetTy->isPointerTy())
        return builder->CreateIntToPtr(v, targetTy, "itop");
    // int → int (widening)
    if (v->getType()->isIntegerTy() && targetTy->isIntegerTy()) {
        const unsigned srcBits = v->getType()->getIntegerBitWidth();
        const unsigned dstBits = targetTy->getIntegerBitWidth();
        if (srcBits < dstBits)
            return builder->CreateSExt(v, targetTy, "sext");
        if (srcBits > dstBits)
            return builder->CreateTrunc(v, targetTy, "trunc");
    }
    // ptr → float (via int)
    if (v->getType()->isPointerTy() && targetTy->isDoubleTy()) {
        auto* intVal = builder->CreatePtrToInt(v, getDefaultType(), "ptoi");
        return builder->CreateSIToFP(intVal, targetTy, "itof");
    }
    return v;
}

llvm::Value* CodeGenerator::toBool(llvm::Value* v) {
    // When the value is already i1 (from a comparison), it IS the boolean
    // — no need to generate a redundant `icmp ne i1 %v, 0`.  This saves
    // one instruction per condition check and produces cleaner IR that
    // LLVM's branch analysis and vectorizer can process more efficiently.
    if (v->getType()->isIntegerTy(1)) {
        return v;
    }
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

llvm::Value* CodeGenerator::convertToVectorElement(llvm::Value* v, llvm::Type* elemTy) {
    if (v->getType() == elemTy)
        return v;
    if (elemTy->isFloatTy()) {
        if (v->getType()->isDoubleTy())
            return builder->CreateFPTrunc(v, elemTy, "elem.fptrunc");
        if (v->getType()->isIntegerTy())
            return builder->CreateSIToFP(v, elemTy, "elem.sitofp");
    } else if (elemTy->isDoubleTy()) {
        if (v->getType()->isIntegerTy())
            return builder->CreateSIToFP(v, elemTy, "elem.sitofp");
        if (v->getType()->isFloatTy())
            return builder->CreateFPExt(v, elemTy, "elem.fpext");
    } else if (elemTy->isIntegerTy()) {
        if (v->getType()->isDoubleTy() || v->getType()->isFloatTy())
            return builder->CreateFPToSI(v, elemTy, "elem.fptosi");
        if (v->getType()->isIntegerTy())
            return builder->CreateIntCast(v, elemTy, true, "elem.icast");
    }
    return v;
}

llvm::Value* CodeGenerator::splatScalarToVector(llvm::Value* scalar, llvm::Type* vecTy) {
    auto* fvt = llvm::cast<llvm::FixedVectorType>(vecTy);
    scalar = convertToVectorElement(scalar, fvt->getElementType());
    llvm::Value* undef = llvm::UndefValue::get(vecTy);
    llvm::Value* ins = builder->CreateInsertElement(undef, scalar,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "splat.ins");
    llvm::SmallVector<int, 16> mask(fvt->getNumElements(), 0);
    return builder->CreateShuffleVector(ins, mask, "splat");
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
        // Restore constIntFolds_ to the value that was in scope before this scope was entered.
        if (entry.second.hadPreviousIntFold) {
            constIntFolds_[entry.first] = entry.second.previousIntFold;
        } else {
            constIntFolds_.erase(entry.first);
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
            ConstBinding binding;
            if (existingConst == constValues.end()) {
                binding = {false, false};
            } else {
                binding = {true, existingConst->second};
            }
            // Save previous constIntFolds_ entry for this variable (for scope restoration).
            auto foldIt = constIntFolds_.find(name);
            if (foldIt != constIntFolds_.end()) {
                binding.hadPreviousIntFold = true;
                binding.previousIntFold = foldIt->second;
            }
            constScope[name] = binding;
        }
    }
    namedValues[name] = value;
    constValues[name] = isConst;
    // If the variable is being rebound (not a const), remove its int fold entry
    // since the new binding may have a different value.
    if (!isConst)
        constIntFolds_.erase(name);
}

void CodeGenerator::checkConstModification(const std::string& name, const std::string& action) {
    auto constIt = constValues.find(name);
    if (constIt != constValues.end() && constIt->second) {
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "Cannot " + action + " const variable: " + name});
    }
}

void CodeGenerator::validateScopeStacksMatch(const char* location) {
    if (scopeStack.size() != constScopeStack.size()) {
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error,
                                         {"", 0, 0},
                                         "Scope tracking mismatch in codegen (" + std::string(location) +
                                             "): values=" + std::to_string(scopeStack.size()) +
                                             ", consts=" + std::to_string(constScopeStack.size())});
    }
}

llvm::AllocaInst* CodeGenerator::createEntryBlockAlloca(llvm::Function* function, const std::string& name,
                                                        llvm::Type* type) {
    llvm::IRBuilder<> entryBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
    auto* alloca = entryBuilder.CreateAlloca(type ? type : getDefaultType(), nullptr, name);
    // Set explicit alignment for i64 allocas — the default type in OmScript.
    // This avoids backend alignment computation overhead and ensures that
    // loads/stores from these allocas can use aligned instructions (movq
    // instead of unaligned loads on x86-64).
    llvm::Type* allocaType = type ? type : getDefaultType();
    if (allocaType->isIntegerTy(64) || allocaType->isDoubleTy() || allocaType->isPointerTy()) {
        alloca->setAlignment(llvm::Align(8));
    }
    return alloca;
}

void CodeGenerator::codegenError(const std::string& message, const ASTNode* node) {
    SourceLocation loc;
    if (node && node->line > 0) {
        loc.line = node->line;
        loc.column = node->column;
    }
    throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, loc, message});
}

// ---------------------------------------------------------------------------
// Lazy C library function declarations
// ---------------------------------------------------------------------------
// These helpers ensure each C library function is declared at most once in
// the LLVM module, eliminating duplicated getFunction()/Create() blocks
// that were previously scattered across multiple built-in handlers.

llvm::Function* CodeGenerator::declareExternalFn(llvm::StringRef name, llvm::FunctionType* ty) {
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    return fn;
}

void CodeGenerator::attachLoopMetadata(llvm::BranchInst* backEdgeBr) {
    if (optimizationLevel < OptimizationLevel::O1)
        return;
    llvm::SmallVector<llvm::Metadata*, 2> mds;
    mds.push_back(nullptr);
    mds.push_back(llvm::MDNode::get(*context,
        {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
    llvm::MDNode* md = llvm::MDNode::get(*context, mds);
    md->replaceOperandWith(0, md);
    backEdgeBr->setMetadata(llvm::LLVMContext::MD_loop, md);
}

llvm::Function* CodeGenerator::getOrDeclareStrlen() {
    if (auto* fn = module->getFunction("strlen"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strlen", ty);
    // memory(argmem: read): strlen only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    OMSC_ADD_NOCAPTURE(fn, 0);                      // does not capture its pointer arg
    fn->addParamAttr(0, llvm::Attribute::ReadOnly); // only reads through the pointer
    fn->addParamAttr(0, llvm::Attribute::NonNull);  // never called with null
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMalloc() {
    if (auto* fn = module->getFunction("malloc"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::PointerType::getUnqual(*context), {getDefaultType()}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "malloc", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoBuiltin);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    // allocsize(0): parameter 0 is the allocation size — enables LLVM to
    // reason about the returned buffer size for alias analysis and to
    // eliminate redundant bounds checks on the allocated memory.
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 0, std::nullopt));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareCalloc() {
    if (auto* fn = module->getFunction("calloc"))
        return fn;
    // calloc(size_t count, size_t size) -> void*
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {getDefaultType(), getDefaultType()}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "calloc", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoBuiltin);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    // allocsize(0, 1): allocation size is arg0 * arg1 — enables LLVM to
    // reason about the returned buffer size for alias analysis and to
    // eliminate redundant bounds checks on the allocated memory.
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 0, 1));
    // Note: calloc CAN return NULL on OOM, so we do NOT add NonNull here.
    // The call site in array_fill does not check for NULL because OmScript's
    // runtime assumes allocation always succeeds (same as C benchmarks).
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrcpy() {
    if (auto* fn = module->getFunction("strcpy"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strcpy", ty);
    // memory(argmem: readwrite): strcpy only accesses memory through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::Returned); // strcpy returns the destination pointer
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrcat() {
    if (auto* fn = module->getFunction("strcat"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strcat", ty);
    // memory(argmem: readwrite): strcat only accesses memory through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::Returned); // strcat returns the destination pointer
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrcmp() {
    if (auto* fn = module->getFunction("strcmp"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(builder->getInt32Ty(), {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strcmp", ty);
    // memory(argmem: read): strcmp only reads through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrncmp() {
    if (auto* fn = module->getFunction("strncmp"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* sizeTy = getDefaultType();  // size_t mapped to i64
    auto* ty = llvm::FunctionType::get(builder->getInt32Ty(), {ptrTy, ptrTy, sizeTy}, false);
    llvm::Function* fn = declareExternalFn("strncmp", ty);
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMemcmp() {
    if (auto* fn = module->getFunction("memcmp"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* sizeTy = getDefaultType();  // size_t mapped to i64
    auto* ty = llvm::FunctionType::get(builder->getInt32Ty(), {ptrTy, ptrTy, sizeTy}, false);
    llvm::Function* fn = declareExternalFn("memcmp", ty);
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePutchar() {
    if (auto* fn = module->getFunction("putchar"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "putchar", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePuts() {
    if (auto* fn = module->getFunction("puts"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context),
                                       {llvm::PointerType::getUnqual(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "puts", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFputs() {
    if (auto* fn = module->getFunction("fputs"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // fputs(const char* str, FILE* stream) -> int
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context),
                                       {ptrTy, ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fputs", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Value* CodeGenerator::getOrDeclareStdout() {
    // Get the C library 'stdout' global (extern FILE *stdout).
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    if (auto* gv = module->getGlobalVariable("stdout")) {
        auto* load = builder->CreateLoad(ptrTy, gv, "stdout.val");
        // stdout is a process-wide constant — mark as nonnull and invariant
        // so LLVM can hoist/CSE repeated loads.
        load->setMetadata(llvm::LLVMContext::MD_nonnull,
                          llvm::MDNode::get(*context, {}));
        return load;
    }
    auto* gv = new llvm::GlobalVariable(
        *module, ptrTy, /*isConstant=*/false,
        llvm::GlobalValue::ExternalLinkage, nullptr, "stdout");
    auto* load = builder->CreateLoad(ptrTy, gv, "stdout.val");
    load->setMetadata(llvm::LLVMContext::MD_nonnull,
                      llvm::MDNode::get(*context, {}));
    return load;
}

llvm::Function* CodeGenerator::getOrDeclareScanf() {
    if (auto* fn = module->getFunction("scanf"))
        return fn;
    auto* ty =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::PointerType::getUnqual(*context)}, true);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "scanf", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareExit() {
    if (auto* fn = module->getFunction("exit"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "exit", module.get());
    fn->addFnAttr(llvm::Attribute::NoReturn);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    // cold: exit() is an error-handling/termination path — marking it cold
    // tells the branch predictor and code layout to keep the exit call site
    // out of the hot I-cache region, improving performance of normal paths.
    fn->addFnAttr(llvm::Attribute::Cold);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareAbort() {
    if (auto* fn = module->getFunction("abort"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "abort", module.get());
    fn->addFnAttr(llvm::Attribute::NoReturn);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    // cold: abort() is an error-handling path — marking it cold improves
    // code layout by keeping the abort call site out of hot I-cache lines.
    fn->addFnAttr(llvm::Attribute::Cold);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareSnprintf() {
    if (auto* fn = module->getFunction("snprintf"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, getDefaultType(), ptrTy}, true);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "snprintf", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // snprintf: dest is writeonly+nocapture, format string is readonly+nocapture.
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(2, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 2);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMemchr() {
    if (auto* fn = module->getFunction("memchr"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, llvm::Type::getInt32Ty(*context), getDefaultType()}, false);
    llvm::Function* fn = declareExternalFn("memchr", ty);
    // memory(argmem: read): memchr only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFree() {
    if (auto* fn = module->getFunction("free"))
        return fn;
    auto* ty =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {llvm::PointerType::getUnqual(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "free", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    OMSC_ADD_NOCAPTURE(fn, 0); // free does not capture its pointer argument
    // allockind("free"): marks free as a deallocation function — enables the
    // optimizer to pair malloc/free and eliminate dead allocation sequences.
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Free)));
    // memory(argmem: readwrite, inaccessiblemem: readwrite): free reads the
    // pointed-to allocation metadata and returns the block to the allocator's
    // internal (inaccessible) structures.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::inaccessibleOrArgMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::get(*context, llvm::Attribute::AllocatedPointer));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrstr() {
    if (auto* fn = module->getFunction("strstr"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strstr", ty);
    // memory(argmem: read): strstr only reads through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMemcpy() {
    if (auto* fn = module->getFunction("memcpy"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, getDefaultType()}, false);
    llvm::Function* fn = declareExternalFn("memcpy", ty);
    // memory(argmem: readwrite): memcpy only accesses memory through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::Returned); // memcpy returns the destination pointer
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMemmove() {
    if (auto* fn = module->getFunction("memmove"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, getDefaultType()}, false);
    llvm::Function* fn = declareExternalFn("memmove", ty);
    // memory(argmem: readwrite): memmove only accesses memory through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::Returned); // memmove returns the destination pointer
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareToupper() {
    if (auto* fn = module->getFunction("toupper"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("toupper", ty);
    // memory(none): toupper is a pure function — no reads or writes to memory.
    // This lets the optimizer hoist it out of loops and CSE duplicate calls.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareTolower() {
    if (auto* fn = module->getFunction("tolower"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("tolower", ty);
    // memory(none): tolower is a pure function — no reads or writes to memory.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareIsspace() {
    if (auto* fn = module->getFunction("isspace"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("isspace", ty);
    // memory(none): isspace is a pure function — no reads or writes to memory.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrtoll() {
    if (auto* fn = module->getFunction("strtoll"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy, ptrTy, llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("strtoll", ty);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrtod() {
    if (auto* fn = module->getFunction("strtod"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getFloatType(), {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strtod", ty);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrdup() {
    if (auto* fn = module->getFunction("strdup"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strdup", ty);
    // memory(argmem: read, inaccessiblemem: readwrite): strdup reads the source
    // string and allocates new memory through the heap allocator.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects(
            llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref) |
            llvm::MemoryEffects::inaccessibleMemOnly())));
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFloor() {
    if (auto* fn = module->getFunction("floor"))
        return fn;
    auto* ty = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
    llvm::Function* fn = declareExternalFn("floor", ty);
    // memory(none): floor is a pure mathematical function.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    // speculatable: floor never reads/writes memory or has side effects, so
    // LLVM may hoist or speculate calls across branches and into loop preheaders.
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareCeil() {
    if (auto* fn = module->getFunction("ceil"))
        return fn;
    auto* ty = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
    llvm::Function* fn = declareExternalFn("ceil", ty);
    // memory(none): ceil is a pure mathematical function.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    // speculatable: same rationale as floor.
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareRound() {
    if (auto* fn = module->getFunction("round"))
        return fn;
    auto* ty = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
    llvm::Function* fn = declareExternalFn("round", ty);
    // memory(none): round is a pure mathematical function.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    // speculatable: same rationale as floor.
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareQsort() {
    if (auto* fn = module->getFunction("qsort"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                                       {ptrTy, getDefaultType(), getDefaultType(), ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "qsort", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareRand() {
    if (auto* fn = module->getFunction("rand"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "rand", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    // rand() only accesses global PRNG state (inaccessible to the caller);
    // this lets LLVM reorder it past pure memory operations.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context,
        llvm::MemoryEffects::inaccessibleMemOnly()));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareSrand() {
    if (auto* fn = module->getFunction("srand"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "srand", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    // srand() only modifies global PRNG state (inaccessible to the caller);
    // it must not be reordered past rand() but can float past local stores.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context,
        llvm::MemoryEffects::inaccessibleMemOnly()));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareTimeFunc() {
    if (auto* fn = module->getFunction("time"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "time", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareUsleep() {
    if (auto* fn = module->getFunction("usleep"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "usleep", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrchr() {
    if (auto* fn = module->getFunction("strchr"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "strchr", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // memory(argmem: read): strchr only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrndup() {
    if (auto* fn = module->getFunction("strndup"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, getDefaultType()}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "strndup", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);  // strndup never returns null (we abort on failure)
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(0, llvm::Attribute::NonNull);  // source string is never null
    // allocsize(1): parameter 1 upper-bounds the allocation size — enables LLVM
    // to reason about the returned buffer size for alias analysis.
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 1, std::nullopt));
    // allockind("alloc,uninitialized"): strndup allocates new heap memory (the
    // content is initialised by the function itself, but from LLVM's perspective
    // the returned block is freshly allocated).
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Alloc |
                                                             llvm::AllocFnKind::Uninitialized)));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareRealloc() {
    if (auto* fn = module->getFunction("realloc"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, getDefaultType()}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "realloc", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    OMSC_ADD_NOCAPTURE(fn, 0); // realloc does not capture the old pointer
    // allocsize(1): parameter 1 is the new allocation size.
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 1, std::nullopt));
    // allockind("realloc"): marks realloc as a reallocation function so LLVM can
    // track the pointer through resize operations for alias analysis.
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Realloc)));
    // memory(argmem: readwrite, inaccessiblemem: readwrite): realloc reads the
    // old allocation and writes to allocator-internal structures.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::inaccessibleOrArgMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::get(*context, llvm::Attribute::AllocatedPointer));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareAtoi() {
    if (auto* fn = module->getFunction("atoi"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "atoi", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // memory(argmem: read): atoi only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareAtof() {
    if (auto* fn = module->getFunction("atof"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getDoubleTy(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "atof", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // memory(argmem: read): atof only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFwrite() {
    if (auto* fn = module->getFunction("fwrite"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy, getDefaultType(), getDefaultType(), ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fwrite", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(3, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 3);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFflush() {
    if (auto* fn = module->getFunction("fflush"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fflush", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFgets() {
    if (auto* fn = module->getFunction("fgets"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, llvm::Type::getInt32Ty(*context), ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fgets", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(2, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 2);
    return fn;
}


llvm::Function* CodeGenerator::getOrDeclareFopen() {
    if (auto* fn = module->getFunction("fopen"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fopen", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFclose() {
    if (auto* fn = module->getFunction("fclose"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fclose", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFread() {
    if (auto* fn = module->getFunction("fread"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy, getDefaultType(), getDefaultType(), ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fread", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(3, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 3);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFseek() {
    if (auto* fn = module->getFunction("fseek"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context),
                                       {ptrTy, getDefaultType(), llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fseek", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFtell() {
    if (auto* fn = module->getFunction("ftell"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "ftell", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareAccess() {
    if (auto* fn = module->getFunction("access"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "access", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

// ---------------------------------------------------------------------------
// pthread function declarations for concurrency primitives
// ---------------------------------------------------------------------------

llvm::Function* CodeGenerator::getOrDeclarePthreadCreate() {
    if (auto* fn = module->getFunction("pthread_create"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    //                    void *(*start_routine)(void*), void *arg)
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, ptrTy, ptrTy, ptrTy}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_create", module.get());
}

llvm::Function* CodeGenerator::getOrDeclarePthreadJoin() {
    if (auto* fn = module->getFunction("pthread_join"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // int pthread_join(pthread_t thread, void **retval)
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {getDefaultType(), ptrTy}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_join", module.get());
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexInit() {
    if (auto* fn = module->getFunction("pthread_mutex_init"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, ptrTy}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_init", module.get());
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexLock() {
    if (auto* fn = module->getFunction("pthread_mutex_lock"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_lock", module.get());
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexUnlock() {
    if (auto* fn = module->getFunction("pthread_mutex_unlock"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_unlock", module.get());
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexDestroy() {
    if (auto* fn = module->getFunction("pthread_mutex_destroy"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_destroy", module.get());
}

// ---------------------------------------------------------------------------
// Hash-table map runtime helpers (emitted into the LLVM module)
// ---------------------------------------------------------------------------
//
// Layout (all i64):
//   slot 0: capacity (power of 2, always >= 8)
//   slot 1: size     (number of active key-value pairs)
//   slot 2 + i*3 + 0: hash_i  (0 = empty, 1 = tombstone, >=2 = occupied)
//   slot 2 + i*3 + 1: key_i
//   slot 2 + i*3 + 2: val_i
//
// Total allocation: (2 + 3 * capacity) * 8 bytes
//
// Hash function: "Rotate-Accumulate" (RA) hash — 4 IR instructions.
//
//   h  = key * 0xD6E8FEB86659FD93     (multiply: primary avalanche)
//   r  = ror(h, 37)                    (rotate right by prime 37)
//   h  = h + r                         (add: carry-propagation mixing)
//   h |= 2                             (reserve sentinels 0 and 1)
//
// Novel design rationale:
//
// 1. SINGLE MULTIPLY: The large odd constant 0xD6E8FEB86659FD93 (from the
//    murmur3/splitmix family) spreads bits across the full 64-bit word.
//    For sequential integer keys, the products are perfectly spaced with
//    zero collisions modulo any power of 2.
//
// 2. ROTATE (not shift): `ror(h, 37)` is a lossless permutation — no
//    information is destroyed, unlike `h >> k` which discards k low bits.
//    37 is prime and coprime to 64, maximizing the bit displacement:
//    every bit moves to a position 37 away (mod 64) with no fixed points
//    and no short cycles.  Compiles to a single `ror` instruction.
//
// 3. ADD (not XOR): Addition provides strictly superior mixing via carry
//    propagation — a single bit flip at position i affects all positions
//    >= i through the carry chain.  XOR only affects position i.  This
//    is the key novelty: after rotate-add, every output bit depends on
//    ~half of all input bits through the combined multiply+carry chains.
//
// Performance: 4 IR instructions → 4 machine instructions on x86
// (imul, ror, add, or).  Critical-path latency ~5 cycles (mul=3, ror=1,
// add=1, or folded).  The shortest dependency chain possible for a
// quality hash.

/// Emit the Rotate-Accumulate hash for a 64-bit integer key.
/// Returns a hash value guaranteed >= 2 (0=empty, 1=tombstone reserved).
/// Only 4 LLVM IR instructions: mul, fshr (ror), add, or.
llvm::Value* CodeGenerator::emitKeyHash(llvm::Value* key) {
    auto* i64Ty = getDefaultType();

    // Step 1: multiply by large odd constant — primary avalanche.
    llvm::Value* h = builder->CreateMul(
        key, llvm::ConstantInt::get(i64Ty, 0xD6E8FEB86659FD93ULL), "h.mul");

    // Step 2: rotate right by 37 (prime, coprime to 64) — lossless permutation.
    // llvm.fshr(h, h, 37) = (h concat h) >> 37 = ror(h, 37).
    // Compiles to a single `ror` on x86 / `ror` on ARM64.
    llvm::Function* fshr = OMSC_GET_INTRINSIC(
        module.get(), llvm::Intrinsic::fshr, {i64Ty});
    llvm::Value* rotated = builder->CreateCall(
        fshr, {h, h, llvm::ConstantInt::get(i64Ty, 37)}, "h.rot");

    // Step 3: add (not xor) — carry propagation provides additional mixing.
    // A bit flip at position i propagates to all positions > i via carry.
    // NUW only (not NSW): the add can produce any 64-bit value including
    // signed overflow, but unsigned wrapping is intentional hash behavior.
    h = builder->CreateAdd(h, rotated, "h.mix", /*HasNUW=*/true, /*HasNSW=*/false);

    // Step 4: ensure hash >= 2 (0=empty, 1=tombstone are reserved).
    return builder->CreateOr(h, llvm::ConstantInt::get(i64Ty, 2), "hash");
}

/// Helper: set common attributes on emitted map functions.
static void setHashMapFnAttrs(llvm::Function* fn) {
    fn->setLinkage(llvm::Function::InternalLinkage);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    // AlwaysInline: these are small helpers (typically < 50 LLVM IR instructions)
    // that benefit greatly from inlining at all opt levels to enable CSE of
    // hash computations and elimination of redundant loads.
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
}

// __omsc_hmap_new() -> ptr
//   Allocates a hash table with capacity 8, size 0, all entries zeroed.
llvm::Function* CodeGenerator::getOrEmitHashMapNew() {
    if (auto* fn = module->getFunction("__omsc_hmap_new"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_new", module.get());
    setHashMapFnAttrs(fn);
    // Returns a fresh calloc'd allocation — always unique and non-null.
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);

    auto savedIP = builder->saveIP();
    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    builder->SetInsertPoint(entryBB);

    // capacity = 8, total slots = 2 + 3*8 = 26, bytes = 26*8 = 208
    llvm::Value* cap = llvm::ConstantInt::get(i64Ty, 8);
    llvm::Value* totalBytes = llvm::ConstantInt::get(i64Ty, (2 + 3 * 8) * 8);
    // Use calloc so all hash slots start as 0 (empty)
    llvm::Value* buf = builder->CreateCall(getOrDeclareCalloc(), {
        llvm::ConstantInt::get(i64Ty, 1), totalBytes
    }, "hmap.buf");
    // Store capacity in slot 0
    builder->CreateAlignedStore(cap, buf, llvm::MaybeAlign(8));
    // Size (slot 1) is already 0 from calloc
    builder->CreateRet(buf);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_set(map: ptr, key: i64, val: i64) -> ptr
//   Insert or update a key-value pair.  Returns the (possibly reallocated) map.
llvm::Function* CodeGenerator::getOrEmitHashMapSet() {
    if (auto* fn = module->getFunction("__omsc_hmap_set"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_set", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map pointer is the sole reference to this allocation.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    // Returns the (possibly reallocated) map — still a unique allocation.
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    fn->getArg(0)->setName("map");
    fn->getArg(1)->setName("key");
    fn->getArg(2)->setName("val");

    auto savedIP = builder->saveIP();

    // Basic blocks
    auto* entryBB    = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* probeBB    = llvm::BasicBlock::Create(*context, "probe", fn);
    auto* checkBB    = llvm::BasicBlock::Create(*context, "check", fn);
    auto* emptyBB    = llvm::BasicBlock::Create(*context, "empty", fn);
    auto* occupBB    = llvm::BasicBlock::Create(*context, "occup", fn);
    auto* matchBB    = llvm::BasicBlock::Create(*context, "match", fn);
    auto* nextBB     = llvm::BasicBlock::Create(*context, "next", fn);
    auto* insertBB   = llvm::BasicBlock::Create(*context, "insert", fn);
    auto* growBB     = llvm::BasicBlock::Create(*context, "grow", fn);
    auto* doneBB     = llvm::BasicBlock::Create(*context, "done", fn);

    // Entry: compute hash, load capacity/size
    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* keyArg = fn->getArg(1);
    llvm::Value* valArg = fn->getArg(2);

    // Rotate-Accumulate (RA) hash
    llvm::Value* hashVal = emitKeyHash(keyArg);
    auto* capPtr = mapArg;  // slot 0
    auto* capLoad = builder->CreateAlignedLoad(i64Ty, capPtr, llvm::MaybeAlign(8), "cap");
    llvm::Value* cap = capLoad;

    // Capacity is always a power of 2 >= 8.  Communicate this to LLVM:
    //  (a) !range metadata:  cap ∈ [8, 2^62)  — eliminates dead zero-checks
    //  (b) llvm.assume(cap & (cap-1) == 0)    — lets CorrelatedValuePropagation
    //      prove slot < cap after slot & mask, enabling downstream optimizations
    {
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 8)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        capLoad->setMetadata(llvm::LLVMContext::MD_range,
                             llvm::MDNode::get(*context, rangeMD));
        llvm::Value* capM1 = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* isPow2 = builder->CreateICmpEQ(
            builder->CreateAnd(cap, capM1), llvm::ConstantInt::get(i64Ty, 0), "cap.pow2");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isPow2});
    }

    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1), "sizeptr");
    llvm::Value* size = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    llvm::Value* mask = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1), "mask", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* startSlot = builder->CreateAnd(hashVal, mask, "startslot");
    builder->CreateBr(probeBB);

    // Probe loop: linear probing
    builder->SetInsertPoint(probeBB);
    llvm::PHINode* slot = builder->CreatePHI(i64Ty, 2, "slot");
    slot->addIncoming(startSlot, entryBB);
    llvm::PHINode* firstTombstone = builder->CreatePHI(i64Ty, 2, "first.tomb");
    firstTombstone->addIncoming(llvm::ConstantInt::get(i64Ty, -1), entryBB);  // -1 = no tombstone seen

    // Compute entry base: mapPtr + 2 + slot*3
    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* entryOff = builder->CreateAdd(
        builder->CreateMul(slot, three, "slot3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "entryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, entryOff, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    // Check if empty (hash == 0)
    llvm::Value* isEmpty = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 0), "isempty");
    builder->CreateCondBr(isEmpty, emptyBB, checkBB);

    // Check: is it occupied or tombstone?
    builder->SetInsertPoint(checkBB);
    llvm::Value* isTomb = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 1), "istomb");
    // Update first tombstone tracker
    llvm::Value* hasTomb = builder->CreateICmpNE(firstTombstone, llvm::ConstantInt::get(i64Ty, -1), "hastomb");
    llvm::Value* newTomb = builder->CreateSelect(
        builder->CreateAnd(isTomb, builder->CreateNot(hasTomb)),
        slot, firstTombstone, "newtomb");
    builder->CreateCondBr(isTomb, nextBB, occupBB);

    // Occupied: first compare stored hash vs computed hash (cheap i64 cmp).
    // On mismatch we skip the key load entirely — avoids a cache miss on
    // probe chains where most slots hold different keys.
    builder->SetInsertPoint(occupBB);
    auto* checkKeyBB = llvm::BasicBlock::Create(*context, "checkkey", fn);
    llvm::Value* hashMatch = builder->CreateICmpEQ(slotHash, hashVal, "hashmatch");
    // Hash match is unlikely on long probe chains; weight accordingly.
    auto* hashW = llvm::MDBuilder(*context).createBranchWeights(1, 4);
    builder->CreateCondBr(hashMatch, checkKeyBB, nextBB, hashW);

    // Hash matched — now load and compare the actual key.
    builder->SetInsertPoint(checkKeyBB);
    llvm::Value* keyOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "ekeyptr");
    llvm::Value* eKey = builder->CreateAlignedLoad(i64Ty, eKeyPtr, llvm::MaybeAlign(8), "ekey");
    llvm::cast<llvm::Instruction>(eKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* keyMatch = builder->CreateICmpEQ(eKey, keyArg, "keymatch");
    // When hashes match, key match is overwhelmingly likely (collision ~1/2^62).
    auto* keyW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
    builder->CreateCondBr(keyMatch, matchBB, nextBB, keyW);

    // Match: update value in place
    builder->SetInsertPoint(matchBB);
    llvm::Value* valOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 2), "valoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eValPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, valOff, "evalptr");
    {   auto* st = builder->CreateAlignedStore(valArg, eValPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_); }
    builder->CreateBr(doneBB);

    // Next: advance to next slot (linear probe), wrap around
    builder->SetInsertPoint(nextBB);
    llvm::PHINode* tombForNext = builder->CreatePHI(i64Ty, 3, "tomb.next");
    tombForNext->addIncoming(newTomb, checkBB);          // tombstone path: may update tracker
    tombForNext->addIncoming(firstTombstone, occupBB);   // hash mismatch: no change
    tombForNext->addIncoming(firstTombstone, checkKeyBB); // key mismatch: no change
    llvm::Value* nextSlot = builder->CreateAnd(
        builder->CreateAdd(slot, llvm::ConstantInt::get(i64Ty, 1), "slot1", /*HasNUW=*/true, /*HasNSW=*/true),
        mask, "nextslot");
    slot->addIncoming(nextSlot, nextBB);
    firstTombstone->addIncoming(tombForNext, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(probeBB)));

    // Empty: key not found — insert
    builder->SetInsertPoint(emptyBB);
    // Check if we need to grow: size+1 > cap*3/4
    llvm::Value* newSize = builder->CreateAdd(size, llvm::ConstantInt::get(i64Ty, 1), "newsize", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* threshold = builder->CreateLShr(
        builder->CreateMul(cap, llvm::ConstantInt::get(i64Ty, 3), "cap3thr", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "threshold");
    llvm::Value* needGrow = builder->CreateICmpUGT(newSize, threshold, "needgrow");
    // Growth is rare: only triggered when load factor exceeds 75%, which
    // happens at most log2(N) times over N insertions.  Weight accordingly
    // so the branch predictor and code layout favor the no-grow path.
    llvm::MDNode* growWeights = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
    builder->CreateCondBr(needGrow, growBB, insertBB, growWeights);

    // Insert: write into the slot (prefer tombstone slot if available)
    builder->SetInsertPoint(insertBB);
    llvm::Value* hasTombForInsert = builder->CreateICmpNE(firstTombstone, llvm::ConstantInt::get(i64Ty, -1), "hastomb2");
    llvm::Value* insSlot = builder->CreateSelect(hasTombForInsert, firstTombstone, slot, "insslot");
    llvm::Value* insOff = builder->CreateAdd(
        builder->CreateMul(insSlot, three, "ins3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "insoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* insHashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, insOff, "inshashptr");
    {   auto* st = builder->CreateAlignedStore(hashVal, insHashPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_); }
    llvm::Value* insKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg,
        builder->CreateAdd(insOff, llvm::ConstantInt::get(i64Ty, 1), "inskeyoff", /*HasNUW=*/true, /*HasNSW=*/true), "inskeyptr");
    {   auto* st = builder->CreateAlignedStore(keyArg, insKeyPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_); }
    llvm::Value* insValPtr = builder->CreateInBoundsGEP(i64Ty, mapArg,
        builder->CreateAdd(insOff, llvm::ConstantInt::get(i64Ty, 2), "insvaloff", /*HasNUW=*/true, /*HasNSW=*/true), "insvalptr");
    {   auto* st = builder->CreateAlignedStore(valArg, insValPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_); }
    // Update size
    builder->CreateAlignedStore(newSize, sizePtr, llvm::MaybeAlign(8));
    builder->CreateBr(doneBB);

    // Grow: double capacity, rehash all entries, then insert the new key
    builder->SetInsertPoint(growBB);
    llvm::Value* newCap = builder->CreateShl(cap, llvm::ConstantInt::get(i64Ty, 1), "newcap");
    llvm::Value* newTotalSlots = builder->CreateAdd(
        builder->CreateMul(newCap, three, "cap3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "newtotalslots", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* newTotalBytes = builder->CreateMul(newTotalSlots, llvm::ConstantInt::get(i64Ty, 8), "newtotalbytes", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* newBuf = builder->CreateCall(getOrDeclareCalloc(), {
        llvm::ConstantInt::get(i64Ty, 1), newTotalBytes
    }, "newbuf");
    // Store new capacity and size+1
    builder->CreateAlignedStore(newCap, newBuf, llvm::MaybeAlign(8));
    llvm::Value* newSizePtr = builder->CreateInBoundsGEP(i64Ty, newBuf, llvm::ConstantInt::get(i64Ty, 1));
    builder->CreateAlignedStore(newSize, newSizePtr, llvm::MaybeAlign(8));
    // Rehash loop: iterate old entries, insert non-empty/non-tombstone into new table
    llvm::Value* newMask = builder->CreateSub(newCap, llvm::ConstantInt::get(i64Ty, 1), "newmask", /*HasNUW=*/true, /*HasNSW=*/true);
    auto* rehashLoopBB = llvm::BasicBlock::Create(*context, "rehash.loop", fn);
    auto* rehashBodyBB = llvm::BasicBlock::Create(*context, "rehash.body", fn);
    auto* rehashProbeBB = llvm::BasicBlock::Create(*context, "rehash.probe", fn);
    auto* rehashWriteBB = llvm::BasicBlock::Create(*context, "rehash.write", fn);
    auto* rehashNextBB = llvm::BasicBlock::Create(*context, "rehash.next", fn);
    auto* rehashDoneBB = llvm::BasicBlock::Create(*context, "rehash.done", fn);
    auto* insertNewBB  = llvm::BasicBlock::Create(*context, "insertnew", fn);

    builder->CreateBr(rehashLoopBB);
    builder->SetInsertPoint(rehashLoopBB);
    llvm::PHINode* ri = builder->CreatePHI(i64Ty, 2, "ri");
    ri->addIncoming(llvm::ConstantInt::get(i64Ty, 0), growBB);
    llvm::Value* riDone = builder->CreateICmpULT(ri, cap, "ridone");
    builder->CreateCondBr(riDone, rehashBodyBB, rehashDoneBB);

    // Rehash body: read old entry, skip empty/tombstone
    builder->SetInsertPoint(rehashBodyBB);
    llvm::Value* oldOff = builder->CreateAdd(
        builder->CreateMul(ri, three, "ri3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "oldoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* oldHashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, oldOff, "oldhashptr");
    llvm::Value* oldHash = builder->CreateAlignedLoad(i64Ty, oldHashPtr, llvm::MaybeAlign(8), "oldhash");
    llvm::cast<llvm::Instruction>(oldHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isActive = builder->CreateICmpUGE(oldHash, llvm::ConstantInt::get(i64Ty, 2), "isactive");
    builder->CreateCondBr(isActive, rehashProbeBB, rehashNextBB);

    // Rehash probe: find empty slot in new table
    builder->SetInsertPoint(rehashProbeBB);
    llvm::Value* oldKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg,
        builder->CreateAdd(oldOff, llvm::ConstantInt::get(i64Ty, 1), "oldkeyoff", /*HasNUW=*/true, /*HasNSW=*/true), "oldkeyptr");
    llvm::Value* oldKey = builder->CreateAlignedLoad(i64Ty, oldKeyPtr, llvm::MaybeAlign(8), "oldkey");
    llvm::cast<llvm::Instruction>(oldKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* oldValPtr = builder->CreateInBoundsGEP(i64Ty, mapArg,
        builder->CreateAdd(oldOff, llvm::ConstantInt::get(i64Ty, 2), "oldvaloff", /*HasNUW=*/true, /*HasNSW=*/true), "oldvalptr");
    llvm::Value* oldVal = builder->CreateAlignedLoad(i64Ty, oldValPtr, llvm::MaybeAlign(8), "oldval");
    llvm::cast<llvm::Instruction>(oldVal)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
    // Find empty slot in new table using oldHash
    llvm::Value* rStartSlot = builder->CreateAnd(oldHash, newMask, "rstartslot");

    // Inner probe loop for rehash: find empty slot in new table
    auto* rProbeLpBB  = llvm::BasicBlock::Create(*context, "rprobe.lp", fn);
    auto* rAdvanceBB  = llvm::BasicBlock::Create(*context, "rprobe.adv", fn);

    builder->CreateBr(rProbeLpBB);
    builder->SetInsertPoint(rProbeLpBB);
    llvm::PHINode* rs = builder->CreatePHI(i64Ty, 2, "rs");
    rs->addIncoming(rStartSlot, rehashProbeBB);
    llvm::Value* rEntryOff = builder->CreateAdd(
        builder->CreateMul(rs, three, "rs3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "rentryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* rHashPtr = builder->CreateInBoundsGEP(i64Ty, newBuf, rEntryOff, "rhashptr");
    llvm::Value* rSlotHash = builder->CreateAlignedLoad(i64Ty, rHashPtr, llvm::MaybeAlign(8), "rslothash");
    llvm::cast<llvm::Instruction>(rSlotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* rIsEmpty = builder->CreateICmpEQ(rSlotHash, llvm::ConstantInt::get(i64Ty, 0), "risempty");
    builder->CreateCondBr(rIsEmpty, rehashWriteBB, rAdvanceBB);

    // Advance probe: compute next slot, loop back
    builder->SetInsertPoint(rAdvanceBB);
    llvm::Value* rNext = builder->CreateAnd(
        builder->CreateAdd(rs, llvm::ConstantInt::get(i64Ty, 1), "rs1", /*HasNUW=*/true, /*HasNSW=*/true),
        newMask, "rnext");
    rs->addIncoming(rNext, rAdvanceBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(rProbeLpBB)));

    // Write rehashed entry into new table
    builder->SetInsertPoint(rehashWriteBB);
    {   auto* st = builder->CreateAlignedStore(oldHash, rHashPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_); }
    llvm::Value* rKeyPtr = builder->CreateInBoundsGEP(i64Ty, newBuf,
        builder->CreateAdd(rEntryOff, llvm::ConstantInt::get(i64Ty, 1), "rkeyoff", /*HasNUW=*/true, /*HasNSW=*/true), "rkeyptr");
    {   auto* st = builder->CreateAlignedStore(oldKey, rKeyPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_); }
    llvm::Value* rValPtr = builder->CreateInBoundsGEP(i64Ty, newBuf,
        builder->CreateAdd(rEntryOff, llvm::ConstantInt::get(i64Ty, 2), "rvaloff", /*HasNUW=*/true, /*HasNSW=*/true), "rvalptr");
    {   auto* st = builder->CreateAlignedStore(oldVal, rValPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_); }
    builder->CreateBr(rehashNextBB);

    // Rehash next: advance outer loop
    builder->SetInsertPoint(rehashNextBB);
    llvm::Value* riNext = builder->CreateAdd(ri, llvm::ConstantInt::get(i64Ty, 1), "rinext", /*HasNUW=*/true, /*HasNSW=*/true);
    ri->addIncoming(riNext, rehashNextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(rehashLoopBB)));

    // After rehash: insert the new key into the new table
    builder->SetInsertPoint(rehashDoneBB);
    // Free old buffer
    builder->CreateCall(getOrDeclareFree(), {mapArg});
    builder->CreateBr(insertNewBB);

    // Insert new key into grown table (same linear probe logic)
    builder->SetInsertPoint(insertNewBB);
    llvm::Value* nStartSlot = builder->CreateAnd(hashVal, newMask, "nstartslot");
    auto* nProbeLpBB = llvm::BasicBlock::Create(*context, "nprobe.lp", fn);
    builder->CreateBr(nProbeLpBB);
    builder->SetInsertPoint(nProbeLpBB);
    llvm::PHINode* ns = builder->CreatePHI(i64Ty, 2, "ns");
    ns->addIncoming(nStartSlot, insertNewBB);
    llvm::Value* nEntryOff = builder->CreateAdd(
        builder->CreateMul(ns, three, "ns3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "nentryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* nHashPtr = builder->CreateInBoundsGEP(i64Ty, newBuf, nEntryOff, "nhashptr");
    llvm::Value* nSlotHash = builder->CreateAlignedLoad(i64Ty, nHashPtr, llvm::MaybeAlign(8), "nslothash");
    llvm::cast<llvm::Instruction>(nSlotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* nIsEmpty = builder->CreateICmpEQ(nSlotHash, llvm::ConstantInt::get(i64Ty, 0), "nisempty");
    auto* nWriteBB = llvm::BasicBlock::Create(*context, "nprobe.write", fn);
    auto* nAdvBB   = llvm::BasicBlock::Create(*context, "nprobe.adv", fn);
    builder->CreateCondBr(nIsEmpty, nWriteBB, nAdvBB);

    builder->SetInsertPoint(nAdvBB);
    llvm::Value* nNext = builder->CreateAnd(
        builder->CreateAdd(ns, llvm::ConstantInt::get(i64Ty, 1), "ns1", /*HasNUW=*/true, /*HasNSW=*/true),
        newMask, "nnext");
    ns->addIncoming(nNext, nAdvBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(nProbeLpBB)));

    builder->SetInsertPoint(nWriteBB);
    {   auto* st = builder->CreateAlignedStore(hashVal, nHashPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_); }
    llvm::Value* nKeyPtr = builder->CreateInBoundsGEP(i64Ty, newBuf,
        builder->CreateAdd(nEntryOff, llvm::ConstantInt::get(i64Ty, 1), "nkeyoff", /*HasNUW=*/true, /*HasNSW=*/true), "nkeyptr");
    {   auto* st = builder->CreateAlignedStore(keyArg, nKeyPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_); }
    llvm::Value* nValPtr = builder->CreateInBoundsGEP(i64Ty, newBuf,
        builder->CreateAdd(nEntryOff, llvm::ConstantInt::get(i64Ty, 2), "nvaloff", /*HasNUW=*/true, /*HasNSW=*/true), "nvalptr");
    {   auto* st = builder->CreateAlignedStore(valArg, nValPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_); }
    builder->CreateBr(doneBB);

    // Done: return map pointer
    builder->SetInsertPoint(doneBB);
    llvm::PHINode* result = builder->CreatePHI(ptrTy, 3, "result");
    result->addIncoming(mapArg, matchBB);   // updated in place
    result->addIncoming(mapArg, insertBB);  // inserted without grow
    result->addIncoming(newBuf, nWriteBB);  // grew and inserted
    builder->CreateRet(result);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_get(map: ptr, key: i64, def: i64) -> i64
llvm::Function* CodeGenerator::getOrEmitHashMapGet() {
    if (auto* fn = module->getFunction("__omsc_hmap_get"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_get", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; get is read-only.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->getArg(0)->setName("map");
    fn->getArg(1)->setName("key");
    fn->getArg(2)->setName("def");

    auto savedIP = builder->saveIP();

    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* probeBB = llvm::BasicBlock::Create(*context, "probe", fn);
    auto* checkBB = llvm::BasicBlock::Create(*context, "check", fn);
    auto* foundBB = llvm::BasicBlock::Create(*context, "found", fn);
    auto* nextBB  = llvm::BasicBlock::Create(*context, "next", fn);
    auto* notfoundBB = llvm::BasicBlock::Create(*context, "notfound", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* keyArg = fn->getArg(1);
    llvm::Value* defArg = fn->getArg(2);

    // Rotate-Accumulate (RA) hash
    llvm::Value* hashVal = emitKeyHash(keyArg);

    auto* capLoad = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    // Capacity is invariant during a read-only GET — let LLVM hoist/CSE it.
    capLoad->setMetadata(llvm::LLVMContext::MD_invariant_load,
                         llvm::MDNode::get(*context, {}));
    llvm::Value* cap = capLoad;
    // Capacity is always a power of 2 >= 8.
    {
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 8)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        capLoad->setMetadata(llvm::LLVMContext::MD_range,
                             llvm::MDNode::get(*context, rangeMD));
        llvm::Value* capM1 = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* isPow2 = builder->CreateICmpEQ(
            builder->CreateAnd(cap, capM1), llvm::ConstantInt::get(i64Ty, 0), "cap.pow2");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isPow2});
    }
    llvm::Value* mask = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1), "mask", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* startSlot = builder->CreateAnd(hashVal, mask, "startslot");
    builder->CreateBr(probeBB);

    // Probe loop
    builder->SetInsertPoint(probeBB);
    llvm::PHINode* slot = builder->CreatePHI(i64Ty, 2, "slot");
    slot->addIncoming(startSlot, entryBB);

    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* entryOff = builder->CreateAdd(
        builder->CreateMul(slot, three, "slot3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "entryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, entryOff, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    // Empty => not found
    llvm::Value* isEmpty = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 0), "isempty");
    builder->CreateCondBr(isEmpty, notfoundBB, checkBB);

    // Check: tombstone or occupied?
    builder->SetInsertPoint(checkBB);
    llvm::Value* isTomb = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 1), "istomb");
    // If tombstone, skip to next; otherwise compare hashes first
    auto* hashCmpBB = llvm::BasicBlock::Create(*context, "hashcmp", fn);
    builder->CreateCondBr(isTomb, nextBB, hashCmpBB);

    // Hash pre-filter: compare stored hash vs computed hash before loading key.
    // Avoids an expensive key load (potential cache miss) on hash mismatch.
    builder->SetInsertPoint(hashCmpBB);
    auto* checkKeyBB = llvm::BasicBlock::Create(*context, "checkkey", fn);
    llvm::Value* hashMatch = builder->CreateICmpEQ(slotHash, hashVal, "hashmatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1, 4);
        builder->CreateCondBr(hashMatch, checkKeyBB, nextBB, w); }

    builder->SetInsertPoint(checkKeyBB);
    llvm::Value* keyOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "ekeyptr");
    llvm::Value* eKey = builder->CreateAlignedLoad(i64Ty, eKeyPtr, llvm::MaybeAlign(8), "ekey");
    llvm::cast<llvm::Instruction>(eKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* keyMatch = builder->CreateICmpEQ(eKey, keyArg, "keymatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(keyMatch, foundBB, nextBB, w); }

    // Found: load value
    builder->SetInsertPoint(foundBB);
    llvm::Value* valOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 2), "valoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eValPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, valOff, "evalptr");
    llvm::Value* val = builder->CreateAlignedLoad(i64Ty, eValPtr, llvm::MaybeAlign(8), "val");
    llvm::cast<llvm::Instruction>(val)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
    builder->CreateRet(val);

    // Next: advance slot (both tombstone and no-match paths funnel here)
    builder->SetInsertPoint(nextBB);
    llvm::Value* nextSlot = builder->CreateAnd(
        builder->CreateAdd(slot, llvm::ConstantInt::get(i64Ty, 1), "slot1", /*HasNUW=*/true, /*HasNSW=*/true),
        mask, "nextslot");
    slot->addIncoming(nextSlot, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(probeBB)));

    // Not found: return default
    builder->SetInsertPoint(notfoundBB);
    builder->CreateRet(defArg);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_has(map: ptr, key: i64) -> i64 (0 or 1)
llvm::Function* CodeGenerator::getOrEmitHashMapHas() {
    if (auto* fn = module->getFunction("__omsc_hmap_has"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_has", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; has is read-only.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->getArg(0)->setName("map");
    fn->getArg(1)->setName("key");

    auto savedIP = builder->saveIP();

    auto* entryBB    = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* probeBB    = llvm::BasicBlock::Create(*context, "probe", fn);
    auto* checkBB    = llvm::BasicBlock::Create(*context, "check", fn);
    auto* checkKeyBB = llvm::BasicBlock::Create(*context, "checkkey", fn);
    auto* foundBB    = llvm::BasicBlock::Create(*context, "found", fn);
    auto* nextBB     = llvm::BasicBlock::Create(*context, "next", fn);
    auto* notfoundBB = llvm::BasicBlock::Create(*context, "notfound", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* keyArg = fn->getArg(1);

    // Rotate-Accumulate (RA) hash
    llvm::Value* hashVal = emitKeyHash(keyArg);

    auto* capLoad_has = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    // Capacity is invariant during a read-only HAS — let LLVM hoist/CSE it.
    capLoad_has->setMetadata(llvm::LLVMContext::MD_invariant_load,
                             llvm::MDNode::get(*context, {}));
    llvm::Value* cap = capLoad_has;
    // Capacity is always a power of 2 >= 8.
    {
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 8)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        capLoad_has->setMetadata(llvm::LLVMContext::MD_range,
                             llvm::MDNode::get(*context, rangeMD));
        llvm::Value* capM1 = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* isPow2 = builder->CreateICmpEQ(
            builder->CreateAnd(cap, capM1), llvm::ConstantInt::get(i64Ty, 0), "cap.pow2");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isPow2});
    }
    llvm::Value* mask = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1), "mask", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* startSlot = builder->CreateAnd(hashVal, mask, "startslot");
    builder->CreateBr(probeBB);

    builder->SetInsertPoint(probeBB);
    llvm::PHINode* slot = builder->CreatePHI(i64Ty, 2, "slot");
    slot->addIncoming(startSlot, entryBB);

    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* entryOff = builder->CreateAdd(
        builder->CreateMul(slot, three, "slot3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "entryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, entryOff, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isEmpty = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 0), "isempty");
    builder->CreateCondBr(isEmpty, notfoundBB, checkBB);

    builder->SetInsertPoint(checkBB);
    llvm::Value* isTomb = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 1), "istomb");
    auto* hashCmpBB = llvm::BasicBlock::Create(*context, "hashcmp", fn);
    builder->CreateCondBr(isTomb, nextBB, hashCmpBB);

    // Hash pre-filter: compare stored hash vs computed hash before loading key.
    builder->SetInsertPoint(hashCmpBB);
    llvm::Value* hashMatch = builder->CreateICmpEQ(slotHash, hashVal, "hashmatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1, 4);
        builder->CreateCondBr(hashMatch, checkKeyBB, nextBB, w); }

    builder->SetInsertPoint(checkKeyBB);
    llvm::Value* keyOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "ekeyptr");
    llvm::Value* eKey = builder->CreateAlignedLoad(i64Ty, eKeyPtr, llvm::MaybeAlign(8), "ekey");
    llvm::cast<llvm::Instruction>(eKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* keyMatch = builder->CreateICmpEQ(eKey, keyArg, "keymatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(keyMatch, foundBB, nextBB, w); }

    builder->SetInsertPoint(foundBB);
    builder->CreateRet(llvm::ConstantInt::get(i64Ty, 1));

    builder->SetInsertPoint(nextBB);
    llvm::Value* nextSlot = builder->CreateAnd(
        builder->CreateAdd(slot, llvm::ConstantInt::get(i64Ty, 1), "slot1", /*HasNUW=*/true, /*HasNSW=*/true),
        mask, "nextslot");
    slot->addIncoming(nextSlot, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(probeBB)));

    builder->SetInsertPoint(notfoundBB);
    builder->CreateRet(llvm::ConstantInt::get(i64Ty, 0));

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_remove(map: ptr, key: i64) -> ptr
//   Mark the entry as tombstone (hash=1).  Returns the same map pointer.
llvm::Function* CodeGenerator::getOrEmitHashMapRemove() {
    if (auto* fn = module->getFunction("__omsc_hmap_remove"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_remove", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    // Returns the same map pointer (unique ownership).
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    fn->getArg(0)->setName("map");
    fn->getArg(1)->setName("key");

    auto savedIP = builder->saveIP();

    auto* entryBB    = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* probeBB    = llvm::BasicBlock::Create(*context, "probe", fn);
    auto* checkBB    = llvm::BasicBlock::Create(*context, "check", fn);
    auto* checkKeyBB = llvm::BasicBlock::Create(*context, "checkkey", fn);
    auto* foundBB    = llvm::BasicBlock::Create(*context, "found", fn);
    auto* nextBB     = llvm::BasicBlock::Create(*context, "next", fn);
    auto* doneBB     = llvm::BasicBlock::Create(*context, "done", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* keyArg = fn->getArg(1);

    // Rotate-Accumulate (RA) hash
    llvm::Value* hashVal = emitKeyHash(keyArg);

    auto* capLoad_rm = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    llvm::Value* cap = capLoad_rm;
    // Capacity is always a power of 2 >= 8.
    {
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 8)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        capLoad_rm->setMetadata(llvm::LLVMContext::MD_range,
                             llvm::MDNode::get(*context, rangeMD));
        llvm::Value* capM1 = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* isPow2 = builder->CreateICmpEQ(
            builder->CreateAnd(cap, capM1), llvm::ConstantInt::get(i64Ty, 0), "cap.pow2");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isPow2});
    }
    llvm::Value* mask = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1), "mask", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* startSlot = builder->CreateAnd(hashVal, mask, "startslot");
    builder->CreateBr(probeBB);

    builder->SetInsertPoint(probeBB);
    llvm::PHINode* slot = builder->CreatePHI(i64Ty, 2, "slot");
    slot->addIncoming(startSlot, entryBB);

    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* entryOff = builder->CreateAdd(
        builder->CreateMul(slot, three, "slot3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "entryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, entryOff, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isEmpty = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 0), "isempty");
    builder->CreateCondBr(isEmpty, doneBB, checkBB);

    builder->SetInsertPoint(checkBB);
    llvm::Value* isTomb = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 1), "istomb");
    auto* hashCmpBB = llvm::BasicBlock::Create(*context, "hashcmp", fn);
    builder->CreateCondBr(isTomb, nextBB, hashCmpBB);

    // Hash pre-filter: compare stored hash vs computed hash before loading key.
    builder->SetInsertPoint(hashCmpBB);
    llvm::Value* hashMatch = builder->CreateICmpEQ(slotHash, hashVal, "hashmatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1, 4);
        builder->CreateCondBr(hashMatch, checkKeyBB, nextBB, w); }

    builder->SetInsertPoint(checkKeyBB);
    llvm::Value* keyOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "ekeyptr");
    llvm::Value* eKey = builder->CreateAlignedLoad(i64Ty, eKeyPtr, llvm::MaybeAlign(8), "ekey");
    llvm::cast<llvm::Instruction>(eKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* keyMatch = builder->CreateICmpEQ(eKey, keyArg, "keymatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(keyMatch, foundBB, nextBB, w); }

    // Found: mark as tombstone, decrement size
    builder->SetInsertPoint(foundBB);
    {   auto* st = builder->CreateAlignedStore(llvm::ConstantInt::get(i64Ty, 1), hashPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_); }
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1));
    llvm::Value* size = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    llvm::Value* newSize = builder->CreateSub(size, llvm::ConstantInt::get(i64Ty, 1), "newsize", /*HasNUW=*/true, /*HasNSW=*/true);
    builder->CreateAlignedStore(newSize, sizePtr, llvm::MaybeAlign(8));
    builder->CreateBr(doneBB);

    builder->SetInsertPoint(nextBB);
    llvm::Value* nextSlot = builder->CreateAnd(
        builder->CreateAdd(slot, llvm::ConstantInt::get(i64Ty, 1), "slot1", /*HasNUW=*/true, /*HasNSW=*/true),
        mask, "nextslot");
    slot->addIncoming(nextSlot, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(probeBB)));

    // Done: return same map pointer (mutated in place)
    builder->SetInsertPoint(doneBB);
    builder->CreateRet(mapArg);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_keys(map: ptr) -> ptr (array)
llvm::Function* CodeGenerator::getOrEmitHashMapKeys() {
    if (auto* fn = module->getFunction("__omsc_hmap_keys"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_keys", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; keys only reads the map.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    // Returns a fresh malloc'd array — always unique and non-null.
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    fn->getArg(0)->setName("map");

    auto savedIP = builder->saveIP();
    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* loopBB  = llvm::BasicBlock::Create(*context, "loop", fn);
    auto* bodyBB  = llvm::BasicBlock::Create(*context, "body", fn);
    auto* storeBB = llvm::BasicBlock::Create(*context, "store", fn);
    auto* nextBB  = llvm::BasicBlock::Create(*context, "next", fn);
    auto* doneBB  = llvm::BasicBlock::Create(*context, "done", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* cap = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1));
    llvm::Value* size = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");

    // Allocate output array: (size + 1) * 8
    llvm::Value* eight = llvm::ConstantInt::get(i64Ty, 8);
    llvm::Value* arrSlots = builder->CreateAdd(size, llvm::ConstantInt::get(i64Ty, 1), "arrslots", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrBytes = builder->CreateMul(arrSlots, eight, "arrbytes", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrBytes}, "buf");
    builder->CreateAlignedStore(size, buf, llvm::MaybeAlign(8));
    builder->CreateBr(loopBB);

    // Loop over hash table slots
    builder->SetInsertPoint(loopBB);
    llvm::PHINode* i = builder->CreatePHI(i64Ty, 2, "i");
    i->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entryBB);
    llvm::PHINode* writeIdx = builder->CreatePHI(i64Ty, 2, "widx");
    writeIdx->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entryBB);
    llvm::Value* cond = builder->CreateICmpULT(i, cap, "cond");
    builder->CreateCondBr(cond, bodyBB, doneBB);

    builder->SetInsertPoint(bodyBB);
    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* off = builder->CreateAdd(
        builder->CreateMul(i, three, "i3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "off", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, off, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isActive = builder->CreateICmpUGE(slotHash, llvm::ConstantInt::get(i64Ty, 2), "isactive");
    builder->CreateCondBr(isActive, storeBB, nextBB);

    builder->SetInsertPoint(storeBB);
    llvm::Value* keyOff = builder->CreateAdd(off, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* keyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "keyptr");
    llvm::Value* key = builder->CreateAlignedLoad(i64Ty, keyPtr, llvm::MaybeAlign(8), "key");
    llvm::cast<llvm::Instruction>(key)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* arrOff = builder->CreateAdd(writeIdx, llvm::ConstantInt::get(i64Ty, 1), "arroff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrPtr = builder->CreateInBoundsGEP(i64Ty, buf, arrOff, "arrptr");
    builder->CreateAlignedStore(key, arrPtr, llvm::MaybeAlign(8));
    llvm::Value* newWIdx = builder->CreateAdd(writeIdx, llvm::ConstantInt::get(i64Ty, 1), "newidx", /*HasNUW=*/true, /*HasNSW=*/true);
    builder->CreateBr(nextBB);

    builder->SetInsertPoint(nextBB);
    llvm::PHINode* wPhi = builder->CreatePHI(i64Ty, 2, "wphi");
    wPhi->addIncoming(writeIdx, bodyBB);   // not active: unchanged
    wPhi->addIncoming(newWIdx, storeBB);   // active: incremented
    llvm::Value* iNext = builder->CreateAdd(i, llvm::ConstantInt::get(i64Ty, 1), "inext", /*HasNUW=*/true, /*HasNSW=*/true);
    i->addIncoming(iNext, nextBB);
    writeIdx->addIncoming(wPhi, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

    builder->SetInsertPoint(doneBB);
    builder->CreateRet(buf);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_values(map: ptr) -> ptr (array)
llvm::Function* CodeGenerator::getOrEmitHashMapValues() {
    if (auto* fn = module->getFunction("__omsc_hmap_values"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_values", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; values only reads the map.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    // Returns a fresh malloc'd array — always unique and non-null.
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    fn->getArg(0)->setName("map");

    auto savedIP = builder->saveIP();
    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* loopBB  = llvm::BasicBlock::Create(*context, "loop", fn);
    auto* bodyBB  = llvm::BasicBlock::Create(*context, "body", fn);
    auto* storeBB = llvm::BasicBlock::Create(*context, "store", fn);
    auto* nextBB  = llvm::BasicBlock::Create(*context, "next", fn);
    auto* doneBB  = llvm::BasicBlock::Create(*context, "done", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* cap = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1));
    llvm::Value* size = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");

    llvm::Value* eight = llvm::ConstantInt::get(i64Ty, 8);
    llvm::Value* arrSlots = builder->CreateAdd(size, llvm::ConstantInt::get(i64Ty, 1), "arrslots", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrBytes = builder->CreateMul(arrSlots, eight, "arrbytes", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrBytes}, "buf");
    builder->CreateAlignedStore(size, buf, llvm::MaybeAlign(8));
    builder->CreateBr(loopBB);

    builder->SetInsertPoint(loopBB);
    llvm::PHINode* i = builder->CreatePHI(i64Ty, 2, "i");
    i->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entryBB);
    llvm::PHINode* writeIdx = builder->CreatePHI(i64Ty, 2, "widx");
    writeIdx->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entryBB);
    llvm::Value* cond = builder->CreateICmpULT(i, cap, "cond");
    builder->CreateCondBr(cond, bodyBB, doneBB);

    builder->SetInsertPoint(bodyBB);
    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* off = builder->CreateAdd(
        builder->CreateMul(i, three, "i3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "off", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, off, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isActive = builder->CreateICmpUGE(slotHash, llvm::ConstantInt::get(i64Ty, 2), "isactive");
    builder->CreateCondBr(isActive, storeBB, nextBB);

    builder->SetInsertPoint(storeBB);
    llvm::Value* valOff = builder->CreateAdd(off, llvm::ConstantInt::get(i64Ty, 2), "valoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* valPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, valOff, "valptr");
    llvm::Value* val = builder->CreateAlignedLoad(i64Ty, valPtr, llvm::MaybeAlign(8), "val");
    llvm::cast<llvm::Instruction>(val)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
    llvm::Value* arrOff = builder->CreateAdd(writeIdx, llvm::ConstantInt::get(i64Ty, 1), "arroff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrPtr = builder->CreateInBoundsGEP(i64Ty, buf, arrOff, "arrptr");
    builder->CreateAlignedStore(val, arrPtr, llvm::MaybeAlign(8));
    llvm::Value* newWIdx = builder->CreateAdd(writeIdx, llvm::ConstantInt::get(i64Ty, 1), "newidx", /*HasNUW=*/true, /*HasNSW=*/true);
    builder->CreateBr(nextBB);

    builder->SetInsertPoint(nextBB);
    llvm::PHINode* wPhi = builder->CreatePHI(i64Ty, 2, "wphi");
    wPhi->addIncoming(writeIdx, bodyBB);
    wPhi->addIncoming(newWIdx, storeBB);
    llvm::Value* iNext = builder->CreateAdd(i, llvm::ConstantInt::get(i64Ty, 1), "inext", /*HasNUW=*/true, /*HasNSW=*/true);
    i->addIncoming(iNext, nextBB);
    writeIdx->addIncoming(wPhi, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

    builder->SetInsertPoint(doneBB);
    builder->CreateRet(buf);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_size(map: ptr) -> i64
llvm::Function* CodeGenerator::getOrEmitHashMapSize() {
    if (auto* fn = module->getFunction("__omsc_hmap_size"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_size", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; size only reads slot[1].
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->getArg(0)->setName("map");

    auto savedIP = builder->saveIP();
    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    builder->SetInsertPoint(entryBB);
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, fn->getArg(0),
        llvm::ConstantInt::get(i64Ty, 1), "sizeptr");
    llvm::Value* size = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    builder->CreateRet(size);

    builder->restoreIP(savedIP);
    return fn;
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
        if (bin->op == "+" || bin->op == "*")
            return isPreAnalysisStringExpr(bin->left.get(), paramStringIndices, func) ||
                   isPreAnalysisStringExpr(bin->right.get(), paramStringIndices, func);
        return false;
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
    // Seed string type information from explicit type annotations.
    // When a parameter is annotated as `: string` or a function has
    // `-> string` return type, we know the type without flow analysis.
    // This bootstraps the fixpoint loop so that downstream callers/callees
    // of annotated functions get correct string type propagation immediately.
    for (auto& func : program->functions) {
        // Seed string-returning functions from return type annotations.
        if (func->returnType == "string") {
            stringReturningFunctions_.insert(func->name);
        }
        // Seed string parameter types from parameter type annotations.
        for (size_t i = 0; i < func->parameters.size(); ++i) {
            if (func->parameters[i].typeName == "string") {
                funcParamStringTypes_[func->name].insert(i);
            }
        }
    }

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
    if (expr->type == ASTNodeType::LITERAL_EXPR)
        return static_cast<LiteralExpr*>(expr)->literalType == LiteralExpr::LiteralType::STRING;
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
        auto* id = static_cast<IdentifierExpr*>(expr);
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
    // arr[i] where arr is a string array → the element is a string.
    if (expr->type == ASTNodeType::INDEX_EXPR)
        return isStringArrayExpr(static_cast<IndexExpr*>(expr)->array.get());
    if (expr->type == ASTNodeType::BINARY_EXPR) {
        auto* bin = static_cast<BinaryExpr*>(expr);
        if (bin->op == "+")
            return isStringExpr(bin->left.get()) || isStringExpr(bin->right.get());
        // str * n  and  n * str  both produce a string
        if (bin->op == "*")
            return isStringExpr(bin->left.get()) || isStringExpr(bin->right.get());
        return false;
    }
    if (expr->type == ASTNodeType::TERNARY_EXPR) {
        auto* tern = static_cast<TernaryExpr*>(expr);
        return isStringExpr(tern->thenExpr.get()) || isStringExpr(tern->elseExpr.get());
    }
    if (expr->type == ASTNodeType::CALL_EXPR)
        return stringReturningFunctions_.count(static_cast<CallExpr*>(expr)->callee) > 0;
    return false;
}

// Returns true if the expression is an array whose elements are string pointers.
bool CodeGenerator::isStringArrayExpr(Expression* expr) const {
    if (!expr)
        return false;
    // Variable whose name is tracked as a string array.
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR)
        return stringArrayVars_.count(static_cast<IdentifierExpr*>(expr)->name) > 0;
    // Array literal whose elements are all strings.
    if (expr->type == ASTNodeType::ARRAY_EXPR) {
        auto* arr = static_cast<ArrayExpr*>(expr);
        if (arr->elements.empty())
            return false;
        for (const auto& e : arr->elements) {
            if (!isStringExpr(e.get()))
                return false;
        }
        return true;
    }
    // str_split() always returns an array of strings.
    // push(strArr, elem) returns a string array if the input is one.
    // array_concat/array_copy preserve the string-array type of the input.
    if (expr->type == ASTNodeType::CALL_EXPR) {
        auto* call = static_cast<CallExpr*>(expr);
        if (call->callee == "str_split")
            return true;
        if ((call->callee == "push" || call->callee == "array_copy") && !call->arguments.empty())
            return isStringArrayExpr(call->arguments[0].get());
        if (call->callee == "array_concat" && !call->arguments.empty())
            return isStringArrayExpr(call->arguments[0].get());
        return false;
    }
    return false;
}


void CodeGenerator::generate(Program* program) {
    hasOptMaxFunctions = false;
    optMaxFunctions.clear();
    irInstructionCount_ = 0;
    fileNoAlias_ = program->fileNoAlias;

    // --- DWARF debug info initialization ---
    if (debugMode_) {
        debugBuilder_ = std::make_unique<llvm::DIBuilder>(*module);
        const std::string filename = sourceFilename_.empty() ? "source.om" : sourceFilename_;
        debugFile_ = debugBuilder_->createFile(filename, ".");
        debugCU_ = debugBuilder_->createCompileUnit(llvm::dwarf::DW_LANG_C, debugFile_, "OmScript Compiler",
                                                    optimizationLevel != OptimizationLevel::O0, /*Flags=*/"", /*RV=*/0);
        debugScope_ = debugFile_;
        module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        // On Linux, DWARF4 is the most compatible format.
        module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
    }

    // Detect preferred SIMD vector width from the target's CPU features.
    // The width is expressed in *i64 lanes* since OmScript's default type
    // is i64.  AVX-512 has 512-bit registers → 8 × i64; AVX2 has 256-bit
    // registers → 4 × i64; SSE2/NEON has 128-bit registers → 2 × i64.
    // Using the correct width avoids over-requesting: for example, hinting
    // VF=8 on AVX2 would require the vectorizer to split across two
    // registers and may cause harmful type widening (i64 → i128 for
    // modulo-by-constant patterns) that has no native hardware support.
    {
        std::string cpu, features;
        resolveTargetCPU(cpu, features);
        if (features.find("+avx512f") != std::string::npos) {
            preferredVectorWidth_ = 8;  // 8 × i64 = 512 bits
        } else if (features.find("+avx2") != std::string::npos) {
            preferredVectorWidth_ = 4;  // 4 × i64 = 256 bits
        } else {
            preferredVectorWidth_ = 2;  // 2 × i64 = 128 bits (SSE2 / NEON)
        }
    }

    // Resource budget: limit number of functions to prevent DoS.
    if (program->functions.size() > kMaxFunctions) {
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error,
                                         {"", 0, 0},
                                         "Compilation aborted: function count limit exceeded (" +
                                             std::to_string(kMaxFunctions) + "). Input program is too large."});
    }

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
    }
    if (!hasMain) {
        // Program-level error — no specific AST node to reference for location.
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "No 'main' function defined"});
    }

    // Set fast-math flags on the builder for all generated FP operations.
    // This enables FMA fusion (a*b+c -> fmadd), reassociation, reciprocal
    // approximations, and better SIMD vectorization of floating-point loops.
    if (useFastMath_) {
        llvm::FastMathFlags FMF;
        FMF.setFast(); // enables all unsafe FP optimizations
        builder->setFastMathFlags(FMF);
    } else if (optimizationLevel >= OptimizationLevel::O2) {
        // At O2+, enable FP contraction (a*b+c → fma) without full
        // fast-math.  This matches clang's default -ffp-contract=on
        // behaviour: FMA contraction is safe — it produces a more precise
        // result (one rounding instead of two) and utilises hardware FMA
        // units, improving throughput on modern CPUs.  No other unsafe FP
        // transformations (reassociation, reciprocal approximation, etc.)
        // are enabled, preserving IEEE-754 semantics for everything else.
        llvm::FastMathFlags FMF;
        FMF.setAllowContract(true);
        builder->setFastMathFlags(FMF);
    }

    // Forward-declare all functions so that any function can reference any
    // other regardless of source-file ordering (enables mutual recursion).
    for (auto& func : program->functions) {
        // Resolve parameter types from annotations: "float" → double, else i64.
        std::vector<llvm::Type*> paramTypes;
        paramTypes.reserve(func->parameters.size());
        for (auto& param : func->parameters) {
            paramTypes.push_back(resolveAnnotatedType(param.typeName));
        }
        // Resolve return type from annotation (e.g. "-> float" → double).
        llvm::Type* retType = resolveAnnotatedType(func->returnType);
        llvm::FunctionType* funcType = llvm::FunctionType::get(retType, paramTypes, false);
        // Non-main functions use InternalLinkage at O2+ (equivalent to C's
        // `static`).  This tells the optimizer that no external code can call
        // the function, enabling:
        //   - fastcc calling convention (GlobalOpt promotes eligible internals)
        //   - deeper recursive inlining (the inliner has full visibility)
        //   - better alias analysis (no escaping through external callers)
        // "main" must remain ExternalLinkage so the linker can find it.
        auto linkage = (func->name != "main" && optimizationLevel >= OptimizationLevel::O2)
                           ? llvm::Function::InternalLinkage
                           : llvm::Function::ExternalLinkage;
        llvm::Function* function =
            llvm::Function::Create(funcType, linkage, func->name, module.get());
        // Optimization-enabling attributes for all user functions:
        // nounwind   – omscript has no C++ exceptions; elides LSDA / unwind tables
        //              and turns calls into simpler instructions.
        // mustprogress – every function is required to make forward progress;
        //              unlocks loop-idiom recognition (auto-memset/memcpy detection).
        function->addFnAttr(llvm::Attribute::NoUnwind);
        function->addFnAttr(llvm::Attribute::MustProgress);
        // prefer-vector-width: use the target-aware preferred SIMD width
        // detected from CPU features.  AVX-512 → 512, AVX2 → 256, SSE → 128.
        // This balances vectorization throughput with avoiding expensive type
        // promotion on targets without wide vector support.
        function->addFnAttr("prefer-vector-width",
            std::to_string(preferredVectorWidth_ * 64));
        // nosync, nofree, willreturn — these promise the optimizer that the
        // function never synchronizes, never frees memory, and always returns.
        // These promises are WRONG for functions that use concurrency
        // primitives (mutex_lock blocks, mutex_destroy frees, thread_join
        // blocks), so we only add them when the function body is
        // concurrency-free.
        if (!usesConcurrencyPrimitive(func.get())) {
            function->addFnAttr(llvm::Attribute::NoSync);
            function->addFnAttr(llvm::Attribute::NoFree);
            function->addFnAttr(llvm::Attribute::WillReturn);
        }
        // noundef on all parameters and the return value — omscript always
        // initializes every variable, so the optimizer can assume no undef/
        // poison values flow through function boundaries.  This strengthens
        // SCEV, value-range propagation, and alias analysis.
        // signext/zeroext on integer params/return — enables better codegen on
        // targets where the calling convention requires extension (e.g. AArch64).
        // Unsigned type annotations (u8, u16, u32, u64) get zeroext instead
        // of signext, which enables more efficient codegen and lets LLVM prove
        // non-negativity for range analysis and strength reduction.
        // Float (double) params/return must NOT get signext/zeroext.
        for (unsigned i = 0; i < func->parameters.size(); ++i) {
            function->addParamAttr(i, llvm::Attribute::NoUndef);
            if (paramTypes[i]->isIntegerTy()) {
                const auto& tn = func->parameters[i].typeName;
                if (tn == "u8" || tn == "u16" || tn == "u32" || tn == "u64")
                    function->addParamAttr(i, llvm::Attribute::ZExt);
                else
                    function->addParamAttr(i, llvm::Attribute::SExt);
            }
        }
        function->addRetAttr(llvm::Attribute::NoUndef);
        if (retType->isIntegerTy()) {
            if (func->returnType == "u8" || func->returnType == "u16" ||
                func->returnType == "u32" || func->returnType == "u64")
                function->addRetAttr(llvm::Attribute::ZExt);
            else
                function->addRetAttr(llvm::Attribute::SExt);
        }
        functions[func->name] = function;
        functionDecls_[func->name] = func.get();
    }

    // Process enum declarations: store constant values for identifier resolution.
    for (auto& enumDecl : program->enums) {
        for (auto& [memberName, memberValue] : enumDecl->members) {
            const std::string fullName = enumDecl->name + "_" + memberName;
            enumConstants_[fullName] = memberValue;
        }
    }

    // Process struct declarations: store field layouts for struct operations.
    // When hot/cold field attributes are present, reorder fields so that
    // hot fields are grouped first (cache-friendly) and cold fields last.
    // This improves spatial locality for performance-critical access patterns.
    for (auto& structDecl : program->structs) {
        if (!structDecl->fieldDecls.empty()) {
            // Check if any field has hot or cold annotations.
            bool hasLayoutHints = false;
            for (const auto& fd : structDecl->fieldDecls) {
                if (fd.attrs.hot || fd.attrs.cold) {
                    hasLayoutHints = true;
                    break;
                }
            }

            if (hasLayoutHints && optimizationLevel >= OptimizationLevel::O2) {
                // Reorder: hot fields first, normal fields next, cold fields last.
                // Build a permutation that maps original index → new index.
                std::vector<size_t> hotIdx, normalIdx, coldIdx;
                for (size_t i = 0; i < structDecl->fieldDecls.size(); ++i) {
                    if (structDecl->fieldDecls[i].attrs.hot)
                        hotIdx.push_back(i);
                    else if (structDecl->fieldDecls[i].attrs.cold)
                        coldIdx.push_back(i);
                    else
                        normalIdx.push_back(i);
                }

                // Build reordered field lists.
                std::vector<std::string> reorderedFields;
                std::vector<StructField> reorderedDecls;
                reorderedFields.reserve(structDecl->fields.size());
                reorderedDecls.reserve(structDecl->fieldDecls.size());

                for (size_t i : hotIdx) {
                    reorderedFields.push_back(structDecl->fields[i]);
                    reorderedDecls.push_back(structDecl->fieldDecls[i]);
                }
                for (size_t i : normalIdx) {
                    reorderedFields.push_back(structDecl->fields[i]);
                    reorderedDecls.push_back(structDecl->fieldDecls[i]);
                }
                for (size_t i : coldIdx) {
                    reorderedFields.push_back(structDecl->fields[i]);
                    reorderedDecls.push_back(structDecl->fieldDecls[i]);
                }

                structDefs_[structDecl->name] = reorderedFields;
                structFieldDecls_[structDecl->name] = reorderedDecls;
            } else {
                structDefs_[structDecl->name] = structDecl->fields;
                structFieldDecls_[structDecl->name] = structDecl->fieldDecls;
            }
        } else {
            structDefs_[structDecl->name] = structDecl->fields;
        }
    }

    // Register operator overloads: generate implementation functions and store
    // in the operatorOverloads_ registry for dispatch in generateBinary().
    for (auto& structDecl : program->structs) {
        for (auto& overload : structDecl->operators) {
            const std::string key = structDecl->name + "::" + overload.op;
            const std::string funcName = overload.impl->name;
            operatorOverloads_[key] = funcName;
            // Add the operator implementation function to the program's function
            // list so it gets code-generated alongside other functions.
            program->functions.push_back(std::move(overload.impl));
        }
    }

    // Pre-analyze string types: determine which functions return strings and
    // which parameters receive string arguments, so that print/concat/etc.
    // work correctly when strings cross function boundaries.
    preAnalyzeStringTypes(program);

    // E-Graph Equality Saturation: apply algebraic simplification, constant
    // folding, strength reduction, and other rewrites at the AST level using
    // an e-graph.  This discovers globally optimal expression representations
    // before LLVM codegen, enabling optimizations that LLVM's pass pipeline
    // may miss (e.g., cross-expression algebraic identities).
    // Enabled at O2+ unless explicitly disabled with -fno-egraph.
    if (enableEGraph_ && optimizationLevel >= OptimizationLevel::O2) {
        if (verbose_) {
            std::cout << "  [opt] Running e-graph equality saturation on AST ("
                      << program->functions.size() << " functions)..." << std::endl;
        }
        egraph::optimizeProgram(program);
        if (verbose_) {
            auto rules = egraph::getAllRules();
            std::cout << "  [opt] E-graph saturation complete (" << rules.size()
                      << " rewrite rules applied)" << std::endl;
        }
    } else if (verbose_ && optimizationLevel < OptimizationLevel::O2) {
        std::cout << "  [opt] E-graph optimization skipped (requires O2+)" << std::endl;
    }

    if (verbose_) {
        std::cout << "  [codegen] Generating LLVM IR for " << program->functions.size()
                  << " functions..." << std::endl;
    }

    // Generate all function bodies
    for (auto& func : program->functions) {
        generateFunction(func.get());
    }

    // Infer memory effect attributes on user-defined functions.
    // After all function bodies have been generated, scan LLVM IR to detect
    // functions that only read memory (readonly) or don't access memory at
    // all (readnone).  This enables interprocedural optimizations: LICM can
    // hoist readonly calls out of loops, and the inliner uses memory effects
    // to avoid unnecessary spills around call sites.
    //
    // Also infer argmem-only (memory(argmem: ...)) for functions whose
    // non-local memory accesses all trace back to function arguments.
    // argmemonly enables LLVM to prove that calls don't alias globals or
    // heap state, unlocking store-to-load forwarding and dead store
    // elimination across call sites.
    if (optimizationLevel >= OptimizationLevel::O1) {
        for (auto& func : module->functions()) {
            if (func.isDeclaration() || func.getName() == "main")
                continue;
            // Skip functions that already have explicit memory attributes.
            if (func.hasFnAttribute(llvm::Attribute::Memory))
                continue;
            // Scan all instructions: track whether the function reads/writes memory.
            bool hasMemoryWrite = false;
            bool hasMemoryRead = false;
            bool hasUnknownSideEffect = false;
            // Track whether all non-local memory accesses go through
            // function arguments.  If so, the function is argmem-only.
            bool allAccessesThroughArgs = true;
            // Helper: collect the set of function arguments for fast lookup.
            llvm::SmallPtrSet<llvm::Value*, 8> funcArgs;
            for (auto& arg : func.args())
                funcArgs.insert(&arg);
            // Helper: strip GEP chains to find the underlying allocation.
            // A store to `getelementptr(alloca, ...)` is still a local write.
            auto isLocalAlloca = [](llvm::Value* ptr) -> bool {
                while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr))
                    ptr = gep->getPointerOperand();
                return llvm::isa<llvm::AllocaInst>(ptr);
            };
            // Helper: strip GEP/bitcast chains and check if the base is a
            // function argument or a load from a function argument (one level
            // of indirection covers OmScript's array-of-pointers pattern).
            auto isArgDerived = [&](llvm::Value* ptr) -> bool {
                while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr))
                    ptr = gep->getPointerOperand();
                if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(ptr))
                    ptr = bc->getOperand(0);
                if (funcArgs.count(ptr))
                    return true;
                // One level of load indirection: load from arg-derived ptr.
                if (auto* li = llvm::dyn_cast<llvm::LoadInst>(ptr)) {
                    llvm::Value* loadPtr = li->getPointerOperand();
                    while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(loadPtr))
                        loadPtr = gep->getPointerOperand();
                    return funcArgs.count(loadPtr) > 0;
                }
                return false;
            };
            for (auto& BB : func) {
                for (auto& I : BB) {
                    if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                        // Stores to allocas (local variables) don't count as
                        // external writes — they're promoted to SSA by mem2reg.
                        // Also check through GEP chains (struct field / array
                        // element stores) whose base is an alloca.
                        if (!isLocalAlloca(SI->getPointerOperand())) {
                            hasMemoryWrite = true;
                            if (!isArgDerived(SI->getPointerOperand()))
                                allAccessesThroughArgs = false;
                        }
                    } else if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                        if (!isLocalAlloca(LI->getPointerOperand())) {
                            hasMemoryRead = true;
                            if (!isArgDerived(LI->getPointerOperand()))
                                allAccessesThroughArgs = false;
                        }
                    } else if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                        auto* calledFn = CI->getCalledFunction();
                        if (!calledFn) {
                            hasUnknownSideEffect = true;
                            allAccessesThroughArgs = false;
                        } else {
                            // Check callee's memory effects.
                            auto ME = calledFn->getMemoryEffects();
                            if (!ME.onlyReadsMemory() && !ME.doesNotAccessMemory())
                                hasMemoryWrite = true;
                            if (!ME.doesNotAccessMemory())
                                hasMemoryRead = true;
                            if (ME == llvm::MemoryEffects::unknown()) {
                                hasUnknownSideEffect = true;
                                allAccessesThroughArgs = false;
                            }
                            // If callee accesses non-arg memory, this function
                            // transitively accesses non-arg memory too.
                            if (!ME.doesNotAccessMemory() &&
                                !calledFn->onlyAccessesArgMemory())
                                allAccessesThroughArgs = false;
                        }
                    }
                }
            }
            if (!hasUnknownSideEffect && !hasMemoryWrite && !hasMemoryRead) {
                // Function doesn't access memory at all (directly or
                // transitively through calls) → readnone.
                func.addFnAttr(llvm::Attribute::getWithMemoryEffects(
                    *context, llvm::MemoryEffects::none()));
            } else if (!hasUnknownSideEffect && allAccessesThroughArgs) {
                // All non-local memory accesses go through function arguments.
                // This enables LLVM to prove non-interference with globals and
                // heap allocations not passed to this function.
                if (!hasMemoryWrite) {
                    func.addFnAttr(llvm::Attribute::getWithMemoryEffects(
                        *context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
                } else {
                    func.addFnAttr(llvm::Attribute::getWithMemoryEffects(
                        *context, llvm::MemoryEffects::argMemOnly()));
                }
            } else if (!hasUnknownSideEffect && !hasMemoryWrite) {
                // Function reads memory (possibly non-arg) → readonly.
                func.addFnAttr(llvm::Attribute::getWithMemoryEffects(
                    *context, llvm::MemoryEffects::readOnly()));
            }
        }
    }

    // Infer norecurse attribute on user-defined functions.
    // A function that never calls itself (directly or indirectly through
    // internal functions) can be marked norecurse, which enables IPSCCP and
    // the inliner to reason more aggressively about call effects.
    if (optimizationLevel >= OptimizationLevel::O1) {
        // Build a set of function names for quick lookup.
        std::unordered_set<std::string> allFunctions;
        for (auto& func : module->functions()) {
            if (!func.isDeclaration())
                allFunctions.insert(func.getName().str());
        }
        for (auto& func : module->functions()) {
            if (func.isDeclaration() || func.getName() == "main")
                continue;
            bool callsSelf = false;
            for (auto& BB : func) {
                for (auto& I : BB) {
                    if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                        auto* calledFn = CI->getCalledFunction();
                        if (calledFn && calledFn->getName() == func.getName()) {
                            callsSelf = true;
                            break;
                        }
                    }
                }
                if (callsSelf) break;
            }
            if (!callsSelf) {
                func.addFnAttr(llvm::Attribute::NoRecurse);
            }
        }
    }

    // Apply OPTMAX per-function optimization passes BEFORE the module-wide IPO
    // pipeline.  This ensures the aggressively-optimized OPTMAX function bodies
    // feed into the inliner and constant-propagation passes in
    // runOptimizationPasses(), rather than being optimized on already-inlined
    // (and possibly dead) copies after IPO has run.
    if (hasOptMaxFunctions && enableOptMax_) {
        if (verbose_) {
            std::cout << "  [opt] Running OPTMAX per-function optimization passes..." << std::endl;
        }
        optimizeOptMaxFunctions();
    }

    if (verbose_) {
        std::cout << "  [opt] Running LLVM optimization pipeline..." << std::endl;
    }
    runOptimizationPasses();

    // Finalize DWARF debug info before module verification.
    if (debugMode_ && debugBuilder_) {
        debugBuilder_->finalize();
    }

    // Verify the module
    std::string errorStr;
    llvm::raw_string_ostream errorStream(errorStr);
    if (llvm::verifyModule(*module, &errorStream)) {
        // Trim trailing whitespace from LLVM's verification output.
        while (!errorStr.empty() && (errorStr.back() == '\n' || errorStr.back() == ' ')) {
            errorStr.pop_back();
        }
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "Module verification failed: " + errorStr});
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

    // --- Attach DWARF debug subprogram ---
    if (debugMode_ && debugBuilder_ && debugFile_) {
        llvm::DISubroutineType* debugFuncType =
            debugBuilder_->createSubroutineType(debugBuilder_->getOrCreateTypeArray({}));
        llvm::DISubprogram* SP = debugBuilder_->createFunction(
            debugFile_, func->name, llvm::StringRef(), debugFile_, func->line > 0 ? func->line : 1, debugFuncType,
            /*ScopeLine=*/func->line > 0 ? func->line : 1, llvm::DINode::FlagPrototyped,
            llvm::DISubprogram::SPFlagDefinition);
        function->setSubprogram(SP);
    }

    // Detect direct self-recursion.  Used for:
    //   - norecurse attribute (O1+): helps interprocedural alias analysis
    //     and enables GlobalOpt to internalize/eliminate the function.
    //   - alwaysinline guard (O3): prevents infinite inliner loops.
    bool isSelfRecursive = false;
    if (func->name != "main" && optimizationLevel >= OptimizationLevel::O1 && func->body) {
        // exprCheck: walk an expression tree for a direct call to func->name.
        std::function<bool(Expression*)> exprCheck = [&](Expression* e) -> bool {
            if (!e)
                return false;
            if (auto* call = dynamic_cast<CallExpr*>(e)) {
                if (call->callee == func->name)
                    return true;
                for (auto& arg : call->arguments)
                    if (exprCheck(arg.get()))
                        return true;
            }
            if (auto* bin = dynamic_cast<BinaryExpr*>(e))
                return exprCheck(bin->left.get()) || exprCheck(bin->right.get());
            if (auto* un = dynamic_cast<UnaryExpr*>(e))
                return exprCheck(un->operand.get());
            if (auto* tern = dynamic_cast<TernaryExpr*>(e))
                return exprCheck(tern->condition.get()) || exprCheck(tern->thenExpr.get()) ||
                       exprCheck(tern->elseExpr.get());
            return false;
        };
        // hasSelfCall: walk all statement kinds that can contain expressions.
        std::function<bool(Statement*)> hasSelfCall = [&](Statement* s) -> bool {
            if (!s)
                return false;
            if (auto* blk = dynamic_cast<BlockStmt*>(s)) {
                for (auto& st : blk->statements)
                    if (hasSelfCall(st.get()))
                        return true;
                return false;
            }
            if (auto* ret = dynamic_cast<ReturnStmt*>(s))
                return exprCheck(ret->value.get());
            if (auto* exprS = dynamic_cast<ExprStmt*>(s))
                return exprCheck(exprS->expression.get());
            if (auto* var = dynamic_cast<VarDecl*>(s))
                return exprCheck(var->initializer.get());
            if (auto* ifS = dynamic_cast<IfStmt*>(s))
                return exprCheck(ifS->condition.get()) || hasSelfCall(ifS->thenBranch.get()) ||
                       hasSelfCall(ifS->elseBranch.get());
            if (auto* wh = dynamic_cast<WhileStmt*>(s))
                return exprCheck(wh->condition.get()) || hasSelfCall(wh->body.get());
            if (auto* dw = dynamic_cast<DoWhileStmt*>(s))
                return hasSelfCall(dw->body.get()) || exprCheck(dw->condition.get());
            if (auto* fr = dynamic_cast<ForStmt*>(s))
                return exprCheck(fr->start.get()) || exprCheck(fr->end.get()) || exprCheck(fr->step.get()) ||
                       hasSelfCall(fr->body.get());
            if (auto* fe = dynamic_cast<ForEachStmt*>(s))
                return exprCheck(fe->collection.get()) || hasSelfCall(fe->body.get());
            return false;
        };
        isSelfRecursive = hasSelfCall(func->body.get());
        if (!isSelfRecursive) {
            function->addFnAttr(llvm::Attribute::NoRecurse);
        }
    }

    // Hint small helper functions for inlining at O2+.  OPTMAX functions
    // already get aggressive optimization; non-main functions with few
    // statements benefit from being inlined into their callers.
    // 8 statements covers most simple accessors and arithmetic helpers;
    // O3 doubles the threshold to capture larger helpers.
    // At O3, functions at or below the kAlwaysInlineStatements threshold get
    // the stronger alwaysinline attribute — the inliner will unconditionally
    // inline them regardless of call-site weight, eliminating all call
    // overhead for tiny predicates, accessors, and single-operation helpers.
    // Recursive functions are explicitly excluded: alwaysinline on a directly
    // recursive function causes the inliner to loop infinitely.
    // The "deep" statement count walks into nested blocks/loops to capture
    // the true code complexity.  A function with 3 top-level statements that
    // contains triple-nested for-loops has a deep count of ~9, preventing
    // it from being unconditionally inlined where it would inflate code size
    // and cause I-cache pressure.
    static constexpr size_t kMaxInlineHintStatements = 10;
    static constexpr size_t kMaxInlineHintStatementsO3 = 20;
    static constexpr size_t kAlwaysInlineStatements = 8;
    // Recursive deep statement counter — counts all statements in nested
    // blocks, loops, and if/else chains.  Loop bodies are double-counted
    // because loop codegen (header, latch, condition, increment, metadata)
    // emits roughly 2× more IR than a plain statement.
    std::function<size_t(Statement*)> deepStmtCount = [&](Statement* s) -> size_t {
        if (!s) return 0;
        if (auto* blk = dynamic_cast<BlockStmt*>(s)) {
            size_t cnt = 0;
            for (auto& st : blk->statements) cnt += deepStmtCount(st.get());
            return cnt;
        }
        if (auto* ifS = dynamic_cast<IfStmt*>(s))
            return 1 + deepStmtCount(ifS->thenBranch.get()) + deepStmtCount(ifS->elseBranch.get());
        if (auto* wh = dynamic_cast<WhileStmt*>(s))
            return 2 + 2 * deepStmtCount(wh->body.get());
        if (auto* dw = dynamic_cast<DoWhileStmt*>(s))
            return 2 + 2 * deepStmtCount(dw->body.get());
        if (auto* fr = dynamic_cast<ForStmt*>(s))
            return 2 + 2 * deepStmtCount(fr->body.get());
        if (auto* fe = dynamic_cast<ForEachStmt*>(s))
            return 2 + 2 * deepStmtCount(fe->body.get());
        return 1;
    };
    const size_t shallowCount = func->body ? func->body->statements.size() : 0;
    const size_t deepCount = func->body ? deepStmtCount(func->body.get()) : 0;
    const size_t inlineThreshold =
        (optimizationLevel >= OptimizationLevel::O3) ? kMaxInlineHintStatementsO3 : kMaxInlineHintStatements;
    // Zero-cost abstraction guarantee: lambda functions and very small
    // helpers (struct accessors, predicates) get alwaysinline at O2+.
    // This ensures that abstractions like lambdas passed to array_map /
    // array_filter / array_reduce are fully inlined, eliminating all call
    // overhead.  At O3, a higher deep-statement threshold is used.
    //
    // At O1, we also add InlineHint for small functions (but never AlwaysInline).
    // This is critical to prevent a compiler hang: LLVM's O1 interprocedural
    // constant-propagation passes (IPSCCP, FunctionSpecializationPass) can hang
    // when multiple functions call inner functions containing while-loops with
    // constant integer arguments (e.g. collatz_steps(837799), fib_iter(150)).
    // Adding InlineHint causes the inliner to eliminate those cross-function
    // constant-argument call sites before the IPO passes see them, avoiding
    // the expensive analysis.
    static constexpr size_t kAlwaysInlineStatementsO2 = 4;
    const bool isLambda = func->name.rfind("__lambda_", 0) == 0;
    if (func->name != "main" && optimizationLevel >= OptimizationLevel::O1 && func->body &&
        shallowCount <= inlineThreshold) {
        if (optimizationLevel >= OptimizationLevel::O2) {
            const size_t alwaysInlineThreshold =
                (optimizationLevel >= OptimizationLevel::O3) ? kAlwaysInlineStatements : kAlwaysInlineStatementsO2;
            if ((isLambda || deepCount <= alwaysInlineThreshold) && !isSelfRecursive) {
                function->addFnAttr(llvm::Attribute::AlwaysInline);
            } else {
                function->addFnAttr(llvm::Attribute::InlineHint);
            }
        } else {
            // O1: hint small non-recursive functions for inlining only.
            // No AlwaysInline — that would force inlining even of larger
            // functions, bloating code size at a level meant for fast compilation.
            if (!isSelfRecursive) {
                function->addFnAttr(llvm::Attribute::InlineHint);
            }
        }
    }

    // Apply user-specified function annotations.
    // These override the automatic inlining heuristics above.
    if (func->hintInline) {
        function->removeFnAttr(llvm::Attribute::NoInline);
        function->addFnAttr(llvm::Attribute::AlwaysInline);
    }
    if (func->hintNoInline) {
        function->removeFnAttr(llvm::Attribute::AlwaysInline);
        function->removeFnAttr(llvm::Attribute::InlineHint);
        function->addFnAttr(llvm::Attribute::NoInline);
    }
    if (func->hintCold) {
        function->addFnAttr(llvm::Attribute::Cold);
        userAnnotatedColdFunctions_.insert(func->name);
    }
    if (func->hintHot) {
        function->addFnAttr(llvm::Attribute::Hot);
        userAnnotatedHotFunctions_.insert(func->name);
        currentFuncHintHot_ = true;
    }
    if (func->hintPure) {
        // @pure: function has no side effects and does not read/write memory
        // beyond its arguments.  This is a COMPILE-TIME GUARANTEE — the
        // programmer asserts purity.  Enables aggressive CSE, LICM, dead
        // call elimination, and speculative execution.
        function->setOnlyReadsMemory();
        function->setDoesNotThrow();
        function->setWillReturn();
        function->setDoesNotFreeMemory();
    }
    if (func->hintNoReturn) {
        function->addFnAttr(llvm::Attribute::NoReturn);
    }
    if (func->hintStatic) {
        // @static: use internal linkage so the function is only visible within
        // the translation unit.  Enables better interprocedural optimizations
        // (constant propagation, dead argument elimination) because LLVM knows
        // no external code can call it.
        function->setLinkage(llvm::GlobalValue::InternalLinkage);
    }
    if (func->hintFlatten) {
        // @flatten: inline all callees into this function.  This is the
        // opposite of @noinline — it tells the optimizer to aggressively
        // inline everything called from this function.
        function->addFnAttr("flatten");
    }
    if (func->hintRestrict || fileNoAlias_) {
        // @restrict / @noalias / file-level @noalias: tell LLVM this function
        // only accesses memory through its arguments (argmem).  This enables
        // aggressive alias-based optimizations: the optimizer can prove that
        // separate call sites don't alias, enabling load/store reordering
        // and vectorization.
        function->setOnlyAccessesArgMemory();
        function->setDoesNotThrow();
        // Mark all pointer parameters as noalias — OmScript's ownership
        // semantics guarantee that distinct variables cannot alias the same
        // memory region unless explicitly declared.
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NoAlias);
            }
        }
    }

    if (func->hintMinSize) {
        // @minsize: optimize for minimum code size.  Maps to LLVM's OptimizeForSize
        // + MinSize attributes which instruct the backend to prefer smaller code
        // sequences over faster ones (fewer bytes, not fewer cycles).  Useful for
        // rarely-called functions or embedded/size-critical scenarios.
        function->addFnAttr(llvm::Attribute::OptimizeForSize);
        function->addFnAttr(llvm::Attribute::MinSize);
    }
    if (func->hintOptNone) {
        // @optnone: disable all optimizations for this function.  Maps to LLVM's
        // OptimizeNone attribute which completely bypasses the optimizer for the
        // function.  Useful for debugging (keep variable values observable in a
        // debugger), testing (force a specific code path without optimization),
        // or isolating performance regressions.
        function->addFnAttr(llvm::Attribute::OptimizeNone);
        // OptimizeNone requires NoInline per LLVM verifier rules.
        function->addFnAttr(llvm::Attribute::NoInline);
        function->removeFnAttr(llvm::Attribute::AlwaysInline);
        function->removeFnAttr(llvm::Attribute::InlineHint);
    }
    if (func->hintNoUnwind) {
        // @nounwind: function never throws C++ exceptions.  Maps to LLVM's
        // NoUnwind attribute which enables the compiler to omit exception-handling
        // (unwind) tables for this function, saving code size and allowing
        // more aggressive inlining across call boundaries.
        function->addFnAttr(llvm::Attribute::NoUnwind);
    }

    // OmScript uses a flag-based error model (not C++ exceptions / DWARF
    // unwind), so all user-defined functions are inherently nounwind.  Adding
    // this unconditionally (at O2+) lets LLVM:
    //   1. Omit .eh_frame / .gcc_except_table sections → smaller binaries
    //   2. Inline across call boundaries without needing invoke/landingpad
    //   3. Eliminate dead exception-handling code in the backend
    if (optimizationLevel >= OptimizationLevel::O2 &&
        !function->hasFnAttribute(llvm::Attribute::NoUnwind)) {
        function->addFnAttr(llvm::Attribute::NoUnwind);
    }

    // At O2+, align function entry to 16 bytes for better I-cache locality
    // and branch target prediction.  Hot functions get 32-byte alignment to
    // avoid crossing cache-line boundaries on the entry block.
    if (optimizationLevel >= OptimizationLevel::O2) {
        function->setAlignment(func->hintHot ? llvm::Align(32) : llvm::Align(16));

        // mustprogress: tells LLVM that every loop in this function will
        // eventually terminate (no infinite spin-loops).  This enables:
        //   1. Dead store elimination through loops
        //   2. LICM of loads/stores across loop boundaries
        //   3. Loop deletion when the loop body has no observable side effects
        // OmScript programs don't intentionally spin-loop, so this is safe for
        // all user functions.  Functions with @optnone are excluded.
        if (!function->hasFnAttribute(llvm::Attribute::OptimizeNone)) {
            function->addFnAttr(llvm::Attribute::MustProgress);
        }

        // OmScript's ownership model guarantees that pointer parameters
        // (arrays, strings) are always valid (non-null) distinct allocations.
        // Mark ALL pointer parameters as nonnull at O2+ — this lets LLVM
        // eliminate null-checks and enables speculative loads/hoisting.
        // noalias is handled by the default fallback below (line ~4013+)
        // which applies to all functions regardless of optimization level,
        // since no-aliasing is a language-level invariant.
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NonNull);
            }
        }
    }

    // In OPTMAX functions, mark all parameters noalias and add WillReturn.
    // The OPTMAX annotation is the user's compile-time guarantee that the
    // function is safe and well-behaved, enabling maximum optimization.
    if (inOptMaxFunction) {
        // OPTMAX is the user's guarantee that this function always terminates,
        // has no side effects on synchronization, and doesn't free memory that
        // callers depend on.  These attributes enable LLVM to speculate calls,
        // hoist them out of loops, and eliminate dead calls.
        function->addFnAttr(llvm::Attribute::WillReturn);
        function->addFnAttr(llvm::Attribute::NoSync);
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NoAlias);
                function->addParamAttr(i, llvm::Attribute::NonNull);
                // OmScript arrays always have a valid header (at least 8 bytes
                // for the length slot), so pointer parameters are dereferenceable.
                // This enables LLVM to speculate loads and hoist them above
                // null/bounds checks without runtime verification.
                function->addParamAttr(i, llvm::Attribute::getWithDereferenceableBytes(
                    *context, 8));
                OMSC_ADD_NOCAPTURE(function, i);
            }
        }
    }

    // @hot functions at O2+: add nonnull and noalias on pointer parameters.
    // OmScript's ownership model guarantees arrays/strings passed to
    // functions are always valid (non-null) allocations.  This lets LLVM
    // eliminate null-checks and enables speculative loads.
    // noalias is safe because OmScript's ownership semantics guarantee that
    // distinct parameters cannot alias the same memory region — the borrow
    // checker prevents multiple mutable references to the same allocation.
    // This is equivalent to C's __restrict qualifier and is critical for
    // enabling vectorization of loops that access multiple array parameters.
    if (currentFuncHintHot_ && optimizationLevel >= OptimizationLevel::O2 && !inOptMaxFunction) {
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NonNull);
                function->addParamAttr(i, llvm::Attribute::NoAlias);
                // OmScript arrays always have a valid header (at least 8 bytes).
                function->addParamAttr(i, llvm::Attribute::getWithDereferenceableBytes(
                    *context, 8));
                OMSC_ADD_NOCAPTURE(function, i);
            }
        }
    }

    // Default noalias fallback: OmScript's ownership model guarantees that
    // distinct pointer parameters never alias the same memory region — the
    // borrow checker prevents multiple mutable references.  Applied at all
    // optimization levels (including O0) because this is a language-level
    // invariant, not a heuristic.  The guard excludes functions already
    // handled above (@optmax, @hot, @restrict, file-level @noalias) to
    // avoid redundant attribute additions.
    if (!inOptMaxFunction && !currentFuncHintHot_
        && !func->hintRestrict && !fileNoAlias_) {
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NoAlias);
                function->addParamAttr(i, llvm::Attribute::NonNull);
                // OmScript arrays always have a valid header: at least 8 bytes
                // for the i64 length slot.  This is the minimum dereferenceable
                // size for any OmScript pointer (arrays, strings).
                function->addParamAttr(i, llvm::Attribute::getWithDereferenceableBytes(
                    *context, 8));
                // OmScript's ownership model ensures pointer parameters are
                // never captured (stored into global state or returned as
                // pointers).  The borrow checker prevents escaping references.
                // nocapture enables LLVM to prove the pointer doesn't escape,
                // allowing stack promotion and more aggressive alias analysis.
                OMSC_ADD_NOCAPTURE(function, i);
            }
        }
    }

    // OmScript is a single-threaded language — no concurrent memory access
    // is possible.  nosync tells LLVM that this function does not communicate
    // with other threads via memory or synchronization primitives, enabling
    // store-to-load forwarding and dead store elimination across calls.
    if (optimizationLevel >= OptimizationLevel::O2 && !inOptMaxFunction) {
        function->addFnAttr(llvm::Attribute::NoSync);
    }

    // @unroll / @nounroll: per-function loop unrolling control.
    // These are stored and applied to every loop emitted within this function.
    currentFuncHintUnroll_ = func->hintUnroll;
    currentFuncHintNoUnroll_ = func->hintNoUnroll;

    // @vectorize / @novectorize: per-function loop vectorization control.
    // These are stored and applied to every loop emitted within this function.
    currentFuncHintVectorize_ = func->hintVectorize;
    currentFuncHintNoVectorize_ = func->hintNoVectorize;

    // @parallel / @noparallel: per-function auto-parallelization control.
    // These are stored and applied to every loop emitted within this function.
    currentFuncHintParallelize_ = func->hintParallelize;
    currentFuncHintNoParallelize_ = func->hintNoParallelize;

    // @hot: per-function hot annotation.  Used for bounds check elimination
    // and other performance-critical optimizations.
    currentFuncHintHot_ = func->hintHot;

    // @optmax: enable full fast-math flags for all float operations in this
    // function.  The user's @optmax annotation is a compile-time guarantee
    // that the function is safe and well-behaved, which includes IEEE-754
    // relaxation.  Enabling all FMF flags (nnan, ninf, nsz, arcp, contract,
    // reassoc, afn) allows LLVM to:
    //   1. Form FMA instructions (fmul+fadd → fmadd): 2x float throughput
    //   2. Reassociate float reductions for vectorization (sum += a[i])
    //   3. Eliminate NaN/Inf checks on known-finite values
    //   4. Use reciprocal approximations for division
    // The flags are saved and restored after the function body so they don't
    // leak into subsequent non-OPTMAX functions.
    // Guard: when useFastMath_ is already globally enabled, the builder's FMF
    // is already set to fast (line 2314-2316), so no per-function override is
    // needed.  The save/restore is still correct — savedFMF captures the
    // existing fast flags and restores them identically.
    llvm::FastMathFlags savedFMF = builder->getFastMathFlags();
    if (inOptMaxFunction && !useFastMath_) { // skip if globally fast (already set)
        llvm::FastMathFlags FMF;
        FMF.setFast();
        builder->setFastMathFlags(FMF);
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
    stringArrayVars_.clear();
    stringLenCache_.clear();
    stringCapCache_.clear();
    deadVars_.clear();
    deadVarReason_.clear();
    prefetchedParams_.clear();
    prefetchedVars_.clear();
    prefetchedImmutVars_.clear();
    registerVars_.clear();
    simdVars_.clear();
    dictVarNames_.clear();
    nonNegValues_.clear();
    constIntFolds_.clear();

    // Pre-populate stringVars_ for parameters known to receive string arguments.
    auto paramStrIt = funcParamStringTypes_.find(func->name);
    auto argIt = function->arg_begin();
    for (size_t paramIdx = 0; paramIdx < func->parameters.size(); ++paramIdx) {
        auto& param = func->parameters[paramIdx];
        argIt->setName(param.name);

        // Use the parameter's actual LLVM type (respects type annotations).
        llvm::AllocaInst* alloca = createEntryBlockAlloca(function, param.name, argIt->getType());
        builder->CreateStore(&(*argIt), alloca);
        bindVariable(param.name, alloca);

        if (paramStrIt != funcParamStringTypes_.end() && paramStrIt->second.count(paramIdx))
            stringVars_.insert(param.name);

        // @prefetch: emit llvm.prefetch at function entry for annotated params.
        // This hints to the CPU to load the parameter's memory into cache
        // before it is accessed, reducing memory latency.  The parameter
        // value is interpreted as a memory address (i64 → ptr cast); the
        // caller is responsible for passing a valid pointer-sized value
        // (e.g. an array or string reference).
        if (param.hintPrefetch) {
            prefetchedParams_.insert(param.name);
            llvm::Value* ptrVal = builder->CreateLoad(argIt->getType(), alloca, param.name + ".pf.load");
            // Cast to ptr for the prefetch intrinsic (parameter value is treated as a pointer)
            llvm::Value* ptr = builder->CreateIntToPtr(ptrVal, llvm::PointerType::getUnqual(*context), param.name + ".pf.ptr");
            llvm::Function* prefetchFn = OMSC_GET_INTRINSIC(
                module.get(), llvm::Intrinsic::prefetch,
                {llvm::PointerType::getUnqual(*context)});
            // Args: ptr, rw (0=read), locality (3=keep in all cache levels), cache_type (1=data)
            builder->CreateCall(prefetchFn, {
                ptr,
                builder->getInt32(0),  // read prefetch
                builder->getInt32(3),  // high temporal locality
                builder->getInt32(1)   // data cache
            });
        }

        ++argIt;
    }

    // Generate function body
    generateBlock(func->body.get());

    // Add default return if needed
    if (!builder->GetInsertBlock()->getTerminator()) {
        // @prefetch enforcement: at function exit, emit cache invalidation
        // (prefetch with locality=0 "no temporal reuse") for any @prefetch
        // parameters.  The default return path means the prefetched memory
        // is NOT being transferred out (returned), so we evict it.
        for (const auto& pfParam : prefetchedParams_) {
            auto it = namedValues.find(pfParam);
            if (it != namedValues.end()) {
                llvm::Value* ptrVal = builder->CreateLoad(
                    llvm::cast<llvm::AllocaInst>(it->second)->getAllocatedType(),
                    it->second, pfParam + ".pf.exit");
                llvm::Value* ptr = builder->CreateIntToPtr(
                    ptrVal, llvm::PointerType::getUnqual(*context), pfParam + ".pf.evict");
                llvm::Function* prefetchFn = OMSC_GET_INTRINSIC(
                    module.get(), llvm::Intrinsic::prefetch,
                    {llvm::PointerType::getUnqual(*context)});
                // locality=0 means "no temporal reuse" — hint CPU to evict
                builder->CreateCall(prefetchFn, {
                    ptr,
                    builder->getInt32(0),  // read
                    builder->getInt32(0),  // locality=0: evict from cache
                    builder->getInt32(1)   // data cache
                });
            }
        }
        llvm::Type* retTy = function->getReturnType();
        if (retTy->isDoubleTy())
            builder->CreateRet(llvm::ConstantFP::get(retTy, 0.0));
        else
            builder->CreateRet(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)));
    }

    // Verify function
    std::string errorStr;
    llvm::raw_string_ostream errorStream(errorStr);
    if (llvm::verifyFunction(*function, &errorStream)) {
        function->print(llvm::errs());
        // Trim trailing whitespace from LLVM's verification output.
        while (!errorStr.empty() && (errorStr.back() == '\n' || errorStr.back() == ' ')) {
            errorStr.pop_back();
        }
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error,
                                         {"", func->line, func->column},
                                         "Function verification failed for '" + func->name + "': " + errorStr});
    }

    // Force register promotion for functions that use the `register` keyword.
    // Run mem2reg immediately so register-annotated allocas are promoted to
    // SSA registers regardless of the global optimization level.
    if (!registerVars_.empty()) {
        llvm::legacy::FunctionPassManager fpm(module.get());
        fpm.add(llvm::createPromoteMemoryToRegisterPass());
        fpm.doInitialization();
        fpm.run(*function);
        fpm.doFinalization();
    }

    inOptMaxFunction = false;

    // Restore fast-math flags to pre-OPTMAX state so subsequent functions
    // are not affected by the aggressive flags set for this function.
    builder->setFastMathFlags(savedFMF);

    return function;
}

void CodeGenerator::generateStatement(Statement* stmt) {
    checkIRBudget();
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
    case ASTNodeType::TRY_CATCH_STMT:
        generateTryCatch(static_cast<TryCatchStmt*>(stmt));
        break;
    case ASTNodeType::THROW_STMT:
        generateThrow(static_cast<ThrowStmt*>(stmt));
        break;
    case ASTNodeType::ENUM_DECL:
    case ASTNodeType::STRUCT_DECL:
        // Enums and structs are handled at program level, nothing to do here.
        break;
    case ASTNodeType::INVALIDATE_STMT:
        generateInvalidate(static_cast<InvalidateStmt*>(stmt));
        break;
    case ASTNodeType::MOVE_DECL:
        generateMoveDecl(static_cast<MoveDecl*>(stmt));
        break;
    case ASTNodeType::PREFETCH_STMT:
        generatePrefetch(static_cast<PrefetchStmt*>(stmt));
        break;
    default:
        codegenError("Unknown statement type", stmt);
    }
}

llvm::Value* CodeGenerator::generateExpression(Expression* expr) {
    checkIRBudget();
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
    case ASTNodeType::STRUCT_LITERAL_EXPR:
        return generateStructLiteral(static_cast<StructLiteralExpr*>(expr));
    case ASTNodeType::FIELD_ACCESS_EXPR:
        return generateFieldAccess(static_cast<FieldAccessExpr*>(expr));
    case ASTNodeType::FIELD_ASSIGN_EXPR:
        return generateFieldAssign(static_cast<FieldAssignExpr*>(expr));
    case ASTNodeType::PIPE_EXPR: {
        // Desugar: expr |> fn  =>  fn(expr)
        auto* pipe = static_cast<PipeExpr*>(expr);
        // Create a synthetic CallExpr and generate it
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(std::move(pipe->left));
        auto callExpr = std::make_unique<CallExpr>(pipe->functionName, std::move(args));
        callExpr->line = pipe->line;
        callExpr->column = pipe->column;
        return generateCall(callExpr.get());
    }
    case ASTNodeType::LAMBDA_EXPR:
        // Lambdas are desugared by the parser to string literals
        codegenError("Lambda expression should have been desugared by parser", expr);
    case ASTNodeType::SPREAD_EXPR:
        // Spread expressions are only valid inside array literals
        codegenError("Spread operator '...' is only valid inside array literals", expr);
    case ASTNodeType::MOVE_EXPR:
        return generateMoveExpr(static_cast<MoveExpr*>(expr));
    case ASTNodeType::BORROW_EXPR:
        return generateBorrowExpr(static_cast<BorrowExpr*>(expr));
    case ASTNodeType::DICT_EXPR:
        return generateDict(static_cast<DictExpr*>(expr));
    default:
        codegenError("Unknown expression type", expr);
    }
}

} // namespace omscript
