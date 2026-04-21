#include "codegen.h"
#include "diagnostic.h"
#include "egraph.h"
#include "opt_context.h"
#include "opt_orchestrator.h"
#include "synthesize.h"
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
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

/// Returns true if a type-annotation string represents an unsigned integer
/// (uint, or any uN for N in [1..256]).
static bool isUnsignedAnnotation(const std::string& tn) {
    if (tn == "uint") return true;
    if (tn.size() >= 2 && tn[0] == 'u') {
        for (size_t j = 1; j < tn.size(); ++j)
            if (!std::isdigit(static_cast<unsigned char>(tn[j]))) return false;
        return true; // u1, u2, ..., u256, u64, etc.
    }
    return false;
}

// Canonical set of all stdlib built-in function names.
// These functions are always compiled to native machine code via LLVM IR.
static const std::unordered_set<std::string> stdlibFunctions = {"abs",
                                                                "acos",
                                                                "array_any",
                                                                "array_concat",
                                                                "array_contains",
                                                                "array_copy",
                                                                "array_count",
                                                                "array_drop",
                                                                "array_every",
                                                                "array_fill",
                                                                "array_filter",
                                                                "array_find",
                                                                "array_insert",
                                                                "array_last",
                                                                "array_map",
                                                                "array_max",
                                                                "array_mean",
                                                                "array_min",
                                                                "array_product",
                                                                "array_reduce",
                                                                "array_remove",
                                                                "array_rotate",
                                                                "array_slice",
                                                                "array_take",
                                                                "array_unique",
                                                                "asin",
                                                                "assert",
                                                                "assume",
                                                                "atan",
                                                                "atan2",
                                                                "bitreverse",
                                                                "bswap",
                                                                "cbrt",
                                                                "ceil",
                                                                "char_at",
                                                                "char_code",
                                                                "clamp",
                                                                "clz",
                                                                "command",
                                                                "copysign",
                                                                "cos",
                                                                "ctz",
                                                                "exit_program",
                                                                "exit",
                                                                "exp",
                                                                "exp2",
                                                                "expect",
                                                                "fast_add",
                                                                "fast_div",
                                                                "fast_mul",
                                                                "fast_sub",
                                                                "file_append",
                                                                "file_exists",
                                                                "file_read",
                                                                "file_write",
                                                                "filter",
                                                                "floor",
                                                                "fma",
                                                                "gcd",
                                                                "hypot",
                                                                "index_of",
                                                                "input",
                                                                "input_line",
                                                                "is_alnum",
                                                                "is_alpha",
                                                                "is_digit",
                                                                "is_even",
                                                                "is_lower",
                                                                "is_odd",
                                                                "is_power_of_2",
                                                                "is_space",
                                                                "is_upper",
                                                                "lcm",
                                                                "len",
                                                                "log",
                                                                "log10",
                                                                "log2",
                                                                "map_filter",
                                                                "map_get",
                                                                "map_has",
                                                                "map_invert",
                                                                "map_keys",
                                                                "map_merge",
                                                                "map_new",
                                                                "map_remove",
                                                                "map_set",
                                                                "map_size",
                                                                "map_values",
                                                                "max",
                                                                "max_float",
                                                                "min",
                                                                "min_float",
                                                                "number_to_string",
                                                                "pop",
                                                                "popcount",
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
                                                                "rotate_left",
                                                                "rotate_right",
                                                                "round",
                                                                "saturating_add",
                                                                "saturating_sub",
                                                                "shell",
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
                                                                "str_filter",
                                                                "str_find",
                                                                "str_index_of",
                                                                "str_join",
                                                                "str_len",
                                                                "str_lower",
                                                                "str_lstrip",
                                                                "str_pad_left",
                                                                "str_pad_right",
                                                                "str_remove",
                                                                "str_repeat",
                                                                "str_replace",
                                                                "str_reverse",
                                                                "str_rstrip",
                                                                "str_split",
                                                                "str_starts_with",
                                                                "str_substr",
                                                                "str_to_float",
                                                                "str_to_int",
                                                                "str_trim",
                                                                "str_upper",
                                                                "string_to_number",
                                                                "sudo_command",
                                                                "env_get",
                                                                "env_set",
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
                                                                "unreachable",
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
    tbaaStructTypeNode_ = tbaaStructType;
    llvm::MDNode* tbaaStrType    = makeTBAAType("string data");
    llvm::MDNode* tbaaMapKeyType = makeTBAAType("map key");
    llvm::MDNode* tbaaMapValType = makeTBAAType("map value");
    llvm::MDNode* tbaaMapHashType = makeTBAAType("map hash");
    llvm::MDNode* tbaaMapMetaType = makeTBAAType("map meta");

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
    tbaaMapMeta_     = llvm::MDNode::get(C, {tbaaMapMetaType, tbaaMapMetaType, zero});

    // !range metadata: array lengths are always in [0, INT64_MAX).
    auto* i64Ty = llvm::Type::getInt64Ty(C);
    arrayLenRangeMD_ = llvm::MDNode::get(C, {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
            i64Ty, static_cast<uint64_t>(INT64_MAX)))
    });
    // !range [0, 2): boolean i64 results (is_alpha, is_digit, str_eq, etc.)
    boolRangeMD_ = llvm::MDNode::get(C, {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 2))
    });
    // !range [0, 256): char i64 results (char_at)
    charRangeMD_ = llvm::MDNode::get(C, {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 256))
    });
    // !range [0, 65): bit-count i64 results (popcount/clz/ctz).
    // These always return a value in [0, 64]; the upper bound is exclusive
    // so 65 is the correct sentinel.  This lets CVP/LVI prove (clz(x) > 64)
    // is always false and enables tighter bounds propagation through arithmetic.
    bitcountRangeMD_ = llvm::MDNode::get(C, {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 65))
    });
}

llvm::MDNode* CodeGenerator::getOrCreateFieldTBAA(const std::string& structType, size_t fieldIdx) {
    auto key = std::make_pair(structType, fieldIdx);
    auto it = tbaaStructFieldCache_.find(key);
    if (it != tbaaStructFieldCache_.end()) return it->second;

    // Build a unique per-field TBAA type node as a child of tbaaStructTypeNode_.
    // Two accesses to different fields will have sibling type nodes that do not alias.
    // A generic struct-field access using tbaaStructField_ still aliases all per-field
    // accesses because tbaaStructTypeNode_ is their common ancestor.
    auto& C = *context;
    auto* zero = llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(C), 0));
    const std::string nodeName = "struct." + structType + "." + std::to_string(fieldIdx);
    llvm::MDNode* fieldTypeNode = llvm::MDNode::get(C, {
        llvm::MDString::get(C, nodeName),
        tbaaStructTypeNode_,
        zero
    });
    // Access tag: {fieldTypeNode, fieldTypeNode, 0}
    llvm::MDNode* accessTag = llvm::MDNode::get(C, {fieldTypeNode, fieldTypeNode, zero});
    tbaaStructFieldCache_[key] = accessTag;
    return accessTag;
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
    // -----------------------------------------------------------------------
    // Pointer-holding types — use LLVM pointer type instead of i64.
    // This preserves pointer provenance through LLVM's alias analysis.
    // Without this, ptr→i64→ptr round-trips at alloca boundaries destroy
    // BasicAA / SCEVAA tracking, preventing LICM from hoisting loads and
    // the vectorizer from proving non-aliasing.
    // -----------------------------------------------------------------------
    if (ann == "string")
        return llvm::PointerType::getUnqual(*context);
    // bigint: heap-allocated arbitrary-precision integer — opaque pointer
    if (ann == "bigint")
        return llvm::PointerType::getUnqual(*context);
    // Array types: int[], string[], float[], etc.
    if (ann.size() >= 2 && ann.compare(ann.size() - 2, 2, "[]") == 0)
        return llvm::PointerType::getUnqual(*context);
    // Dict/map types
    if (ann == "dict" || ann.rfind("dict[", 0) == 0)
        return llvm::PointerType::getUnqual(*context);
    // Known struct types — heap-allocated, accessed via pointer
    if (structDefs_.count(ann))
        return llvm::PointerType::getUnqual(*context);

    // General iN/uN handler for arbitrary bit widths [1..256].
    // Handles i2, i3, ..., i128, u128, i256, u256, etc.
    // Must come AFTER the specific i8/u8, i16/u16, i32/u32 checks above.
    // i64/u64 and others also fall through to here (returning i64 as before).
    {
        const std::string& a = ann;
        if (a.size() >= 2 && (a[0] == 'i' || a[0] == 'u')) {
            bool allDigits = true;
            int n = 0;
            for (size_t j = 1; j < a.size(); ++j) {
                if (!std::isdigit(static_cast<unsigned char>(a[j]))) { allDigits = false; break; }
                n = n * 10 + (a[j] - '0');
                if (n > 256) { allDigits = false; break; }
            }
            if (allDigits && n >= 1 && n <= 256) {
                return llvm::IntegerType::get(*context, static_cast<unsigned>(n));
            }
        }
    }

    // "int", "uint", generics, and empty annotations map to i64.
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
    if (v->getType()->isIntegerTy()) {
        const unsigned srcBits = v->getType()->getIntegerBitWidth();
        if (srcBits > 64) {
            // Wide integers (i65–i256): keep at native width; callers that
            // need i64 must truncate explicitly with convertTo().
            return v;
        }
        // Narrow integers (i1–i63): zero-extend to i64.
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
    const llvm::SmallVector<int, 16> mask(fvt->getNumElements(), 0);
    return builder->CreateShuffleVector(ins, mask, "splat");
}

void CodeGenerator::beginScope() {
    validateScopeStacksMatch(__func__);
    scopeStack.emplace_back();
    constScopeStack.emplace_back();
    borrowScopeStack_.emplace_back(); // new borrow scope
}

void CodeGenerator::endScope() {
    validateScopeStacksMatch(__func__);
    if (scopeStack.empty()) {
        return;
    }

    // Release all borrows introduced in this scope before restoring variables.
    // This must happen first so that checkConstModification / borrow-state
    // queries made during scope teardown see the correct unlocked state.
    if (!borrowScopeStack_.empty()) {
        for (const auto& info : borrowScopeStack_.back()) {
            releaseBorrow(info.refVar);
        }
        borrowScopeStack_.pop_back();
    }

    auto& scope = scopeStack.back();
    auto& constScope = constScopeStack.back();
    for (const auto& entry : scope) {
        if (entry.second) {
            namedValues[entry.first] = entry.second;
        } else {
            namedValues.erase(entry.first);
            varTypeAnnotations_.erase(entry.first);
        }
    }
    for (const auto& entry : constScope) {
        if (entry.second.wasPreviouslyDefined) {
            constValues[entry.first] = entry.second.previousIsConst;
        } else {
            constValues.erase(entry.first);
        }
        if (entry.second.hadPreviousIntFold) {
            constIntFolds_[entry.first] = entry.second.previousIntFold;
        } else {
            constIntFolds_.erase(entry.first);
        }
        if (entry.second.hadPreviousFloatFold) {
            constFloatFolds_[entry.first] = entry.second.previousFloatFold;
        } else {
            constFloatFolds_.erase(entry.first);
        }
        if (entry.second.hadPreviousStringFold) {
            constStringFolds_[entry.first] = entry.second.previousStringFold;
        } else {
            constStringFolds_.erase(entry.first);
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
            ConstBinding binding{};
            if (existingConst == constValues.end()) {
                binding.wasPreviouslyDefined = false;
                binding.previousIsConst = false;
            } else {
                binding.wasPreviouslyDefined = true;
                binding.previousIsConst = existingConst->second;
            }
            // Save previous constIntFolds_ entry for this variable (for scope restoration).
            auto foldIt = constIntFolds_.find(name);
            if (foldIt != constIntFolds_.end()) {
                binding.hadPreviousIntFold = true;
                binding.previousIntFold = foldIt->second;
            }
            // Save previous constFloatFolds_ entry for this variable (for scope restoration).
            auto floatFoldIt = constFloatFolds_.find(name);
            if (floatFoldIt != constFloatFolds_.end()) {
                binding.hadPreviousFloatFold = true;
                binding.previousFloatFold = floatFoldIt->second;
            }
            // Save previous constStringFolds_ entry for this variable (for scope restoration).
            auto stringFoldIt = constStringFolds_.find(name);
            if (stringFoldIt != constStringFolds_.end()) {
                binding.hadPreviousStringFold = true;
                binding.previousStringFold = stringFoldIt->second;
            }
            constScope[name] = binding;
        }
    }
    namedValues[name] = value;
    constValues[name] = isConst;
    // If the variable is being rebound (not a const), remove its int/float/string fold entries
    // since the new binding may have a different value.
    if (!isConst) {
        constIntFolds_.erase(name);
        constFloatFolds_.erase(name);
        constStringFolds_.erase(name);
    }
}

void CodeGenerator::bindVariableAnnotated(const std::string& name, llvm::Value* value,
                                          const std::string& typeAnnot, bool isConst) {
    bindVariable(name, value, isConst);
    if (!typeAnnot.empty())
        varTypeAnnotations_[name] = typeAnnot;
    else
        varTypeAnnotations_.erase(name);
}

bool CodeGenerator::isUnsignedAnnot(const std::string& annot) {
    if (annot == "uint") return true;
    if (annot.size() >= 2 && annot[0] == 'u') {
        for (size_t j = 1; j < annot.size(); ++j)
            if (!std::isdigit(static_cast<unsigned char>(annot[j]))) return false;
        return true;
    }
    return false;
}

bool CodeGenerator::isUnsignedValue(llvm::Value* v) const {
    llvm::Value* base = v;
    // Trace through loads and zero-extending casts to find the alloca name.
    while (base) {
        if (auto* li = llvm::dyn_cast<llvm::LoadInst>(base)) {
            base = li->getPointerOperand();
            continue;
        }
        if (auto* cast = llvm::dyn_cast<llvm::CastInst>(base)) {
            if (cast->getOpcode() == llvm::Instruction::ZExt ||
                cast->getOpcode() == llvm::Instruction::BitCast) {
                base = cast->getOperand(0);
                continue;
            }
        }
        break;
    }
    if (!base) return false;
    const llvm::StringRef name = base->getName();
    if (name.empty()) return false;
    auto it = varTypeAnnotations_.find(name);
    if (it != varTypeAnnotations_.end())
        return isUnsignedAnnot(it->second);
    return false;
}

void CodeGenerator::checkConstModification(const std::string& name, const std::string& action) {
    auto constIt = constValues.find(name);
    if (constIt != constValues.end() && constIt->second) {
        // Give a more specific error message for frozen variables
        if (frozenVars_.count(name)) {
            throw DiagnosticError(
                Diagnostic{DiagnosticSeverity::Error, {"", 0, 0},
                           "Cannot " + action + " frozen variable '" + name +
                           "' — variable was frozen and is immutable for the rest of its lifetime"});
        }
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "Cannot " + action + " const variable: " + name});
    }
    // Block writes to the source variable of an active mutable borrow.
    auto* bs = getBorrowStateOpt(name);
    if (bs && bs->mutBorrowed) {
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0},
                       "Cannot " + action + " variable '" + name +
                       "' — it has an active mutable borrow (release the borrow first)"});
    }
    // Block writes to source while any immutable borrows are alive.
    if (bs && bs->immutBorrowCount > 0) {
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0},
                       "Cannot " + action + " variable '" + name +
                       "' — it has " + std::to_string(bs->immutBorrowCount) +
                       " active immutable borrow(s)"});
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

void CodeGenerator::attachLoopMetadataVec(llvm::BranchInst* backEdgeBr,
                                           unsigned interleaveCount) {
    if (optimizationLevel < OptimizationLevel::O1)
        return;
    llvm::SmallVector<llvm::Metadata*, 4> mds;
    mds.push_back(nullptr);
    mds.push_back(llvm::MDNode::get(*context,
        {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
    if (optimizationLevel >= OptimizationLevel::O2) {
        mds.push_back(llvm::MDNode::get(*context,
            {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
             llvm::ConstantAsMetadata::get(
                 llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        if (interleaveCount > 0) {
            mds.push_back(llvm::MDNode::get(*context,
                {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                 llvm::ConstantAsMetadata::get(
                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context),
                                            interleaveCount))}));
        }
    }
    llvm::MDNode* md = llvm::MDNode::get(*context, mds);
    md->replaceOperandWith(0, md);
    backEdgeBr->setMetadata(llvm::LLVMContext::MD_loop, md);
}

CodeGenerator::CountingLoopInfo CodeGenerator::emitCountingLoop(
        llvm::StringRef prefix,
        llvm::Value* limit,
        llvm::Value* start,
        unsigned interleaveCount,
        const std::function<void(llvm::PHINode*, llvm::BasicBlock*)>& bodyFn) {
    llvm::Function* function  = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* preheader = builder->GetInsertBlock();

    llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context,
        llvm::Twine(prefix) + ".loop", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context,
        llvm::Twine(prefix) + ".body", function);
    llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context,
        llvm::Twine(prefix) + ".done", function);

    // Jump from preheader into loop header.
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

    // Loop header: PHI + condition check.
    builder->SetInsertPoint(loopBB);
    llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2,
                                             llvm::Twine(prefix) + ".idx");
    idx->addIncoming(start, preheader);

    llvm::Value* cond = builder->CreateICmpULT(idx, limit,
                                                llvm::Twine(prefix) + ".cond");
    // Loops almost never execute zero iterations; mark the taken branch hot.
    if (optimizationLevel >= OptimizationLevel::O2) {
        llvm::MDNode* w = llvm::MDBuilder(*context).createBranchWeights(2000, 1);
        builder->CreateCondBr(cond, bodyBB, doneBB, w);
    } else {
        builder->CreateCondBr(cond, bodyBB, doneBB);
    }

    // Body: delegate to caller.  After bodyFn returns the insert point must
    // be inside bodyBB (the caller emits the body AND the back-edge increment
    // + branch using the idx PHI and loopBB pointer provided).
    builder->SetInsertPoint(bodyBB);
    bodyFn(idx, loopBB);

    // If the body didn't already jump somewhere (e.g. it always ends with a
    // CreateBr(loopBB)), leave the insert point in doneBB for the caller.
    builder->SetInsertPoint(doneBB);
    (void)interleaveCount; // interleaveCount is passed to bodyFn via capture
    return {idx, doneBB};
}

// ── IR emit helpers ───────────────────────────────────────────────────────────
// These helpers collapse the 3-4 line TBAA metadata + load/store/alloc
// patterns that appear 200+ times across the codegen files into single calls.

llvm::Value* CodeGenerator::emitLoadArrayLen(llvm::Value* arrPtr,
                                              const llvm::Twine& name) {
    auto* load = builder->CreateAlignedLoad(getDefaultType(), arrPtr,
                                             llvm::MaybeAlign(8), name);
    load->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
    load->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
    nonNegValues_.insert(load);
    return load;
}

llvm::LoadInst* CodeGenerator::emitLoadArrayElem(llvm::Value* elemPtr,
                                               const llvm::Twine& name) {
    auto* load = builder->CreateAlignedLoad(getDefaultType(), elemPtr,
                                             llvm::MaybeAlign(8), name);
    load->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
    return load;
}

void CodeGenerator::emitStoreArrayLen(llvm::Value* len, llvm::Value* arrPtr) {
    auto* st = builder->CreateStore(len, arrPtr);
    st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
}

llvm::StoreInst* CodeGenerator::emitStoreArrayElem(llvm::Value* val, llvm::Value* elemPtr) {
    auto* st = builder->CreateAlignedStore(val, elemPtr, llvm::MaybeAlign(8));
    st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
    return st;
}

llvm::Value* CodeGenerator::emitAllocArray(llvm::Value* len,
                                            const llvm::Twine& name) {
    llvm::Value* one   = llvm::ConstantInt::get(getDefaultType(), 1);
    llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
    llvm::Value* slots = builder->CreateAdd(len, one,   name + ".slots",
                                             /*NUW=*/true, /*NSW=*/true);
    llvm::Value* bytes = builder->CreateMul(slots, eight, name + ".bytes",
                                             /*NUW=*/true, /*NSW=*/true);
    llvm::Value* buf   = builder->CreateCall(getOrDeclareMalloc(), {bytes},
                                              name + ".buf");
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(
        llvm::Attribute::getWithDereferenceableBytes(*context, 8));
    emitStoreArrayLen(len, buf);
    return buf;
}

llvm::Value* CodeGenerator::emitToArrayPtr(llvm::Value* val,
                                            const llvm::Twine& name) {
    val = toDefaultType(val);
    return builder->CreateIntToPtr(val, llvm::PointerType::getUnqual(*context),
                                   name);
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
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    // allocsize(0): parameter 0 is the allocation size — enables LLVM to
    // reason about the returned buffer size for alias analysis and to
    // eliminate redundant bounds checks on the allocated memory.
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 0, std::nullopt));
    // allockind("alloc,uninitialized"): malloc returns freshly allocated
    // memory; LLVM treats it as an allocation function for AA and LICM.
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Alloc |
                                                             llvm::AllocFnKind::Uninitialized)));
    // inaccessiblememonly: malloc only touches allocator-internal state
    // (inaccessible to the caller) — allows LICM to hoist malloc calls out
    // of loops when the size argument is loop-invariant.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::inaccessibleMemOnly()));
    // align(16): malloc on 64-bit Linux/macOS (glibc, musl, Darwin) guarantees
    // at least 16-byte aligned returns (_Alignof(max_align_t) == 16).  Telling
    // LLVM about this alignment enables aligned vector loads/stores on every
    // heap-allocated buffer (arrays, strings, maps) without runtime checks.
    fn->addRetAttr(llvm::Attribute::getWithAlignment(*context, llvm::Align(16)));
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
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull); // OmScript assumes alloc always succeeds
    // allocsize(0, 1): allocation size is arg0 * arg1 — enables LLVM to
    // reason about the returned buffer size for alias analysis and to
    // eliminate redundant bounds checks on the allocated memory.
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 0, 1));
    // allockind("alloc,zeroed"): calloc returns zeroed freshly allocated memory.
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Alloc |
                                                             llvm::AllocFnKind::Zeroed)));
    // inaccessiblememonly: allows LICM to hoist calloc calls out of loops
    // when the size arguments are loop-invariant.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::inaccessibleMemOnly()));
    // align(16): calloc has the same alignment guarantee as malloc on 64-bit POSIX.
    fn->addRetAttr(llvm::Attribute::getWithAlignment(*context, llvm::Align(16)));
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
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);  // stream pointer is not captured
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
    fn->addFnAttr(llvm::Attribute::NoFree);
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
    // speculatable: toupper has no side effects, LLVM may hoist/CSE calls.
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareTolower() {
    if (auto* fn = module->getFunction("tolower"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("tolower", ty);
    // memory(none): tolower is a pure function — no reads or writes to memory.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareIsspace() {
    if (auto* fn = module->getFunction("isspace"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("isspace", ty);
    // memory(none): isspace is a pure function — no reads or writes to memory.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrtoll() {
    if (auto* fn = module->getFunction("strtoll"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy, ptrTy, llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("strtoll", ty);
    // memory(argmem: read): strtoll reads the string via param 0; param 1
    // (endptr) is always null at OmScript call sites so no write occurs.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    // param 1 (endptr) is always passed as null in OmScript — nocapture is
    // still valid (the pointer itself is never stored anywhere by strtoll).
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrtod() {
    if (auto* fn = module->getFunction("strtod"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getFloatType(), {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strtod", ty);
    // memory(argmem: read): strtod reads the string via param 0; param 1
    // (endptr) is always null at OmScript call sites so no write occurs.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
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
    llvm::Function* fn = declareExternalFn("qsort", ty);
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
    fn->addFnAttr(llvm::Attribute::NoSync);
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
    fn->addFnAttr(llvm::Attribute::NoSync);
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
    // memory(inaccessiblemem: read): time() reads the OS clock (inaccessible
    // to the LLVM optimizer) and optionally writes through its pointer arg
    // (always null in OmScript), so treat as read-only inaccessible memory.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context,
        llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref)));
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
    llvm::Function* fn = declareExternalFn("strchr", ty);
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
    // memory(argmem: read, inaccessiblemem: readwrite): strndup reads the source
    // string and writes to allocator-internal structures (same as strdup).
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects(
            llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref) |
            llvm::MemoryEffects::inaccessibleMemOnly())));
    // align(16): strndup allocates via malloc internally, inheriting the same
    // 16-byte alignment guarantee on 64-bit POSIX systems.
    fn->addRetAttr(llvm::Attribute::getWithAlignment(*context, llvm::Align(16)));
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
    fn->addRetAttr(llvm::Attribute::NonNull); // OmScript assumes alloc always succeeds
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
    // align(16): realloc returns memory with the same alignment guarantee as malloc.
    fn->addRetAttr(llvm::Attribute::getWithAlignment(*context, llvm::Align(16)));
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
    fn->addFnAttr(llvm::Attribute::NoFree);
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
    fn->addFnAttr(llvm::Attribute::NoFree);
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
    fn->addFnAttr(llvm::Attribute::NoFree);
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
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NoAlias);  // fopen returns a new FILE* (or null)
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
    fn->addFnAttr(llvm::Attribute::NoFree);
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
    fn->addFnAttr(llvm::Attribute::NoFree);
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
    fn->addFnAttr(llvm::Attribute::NoFree);
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
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
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
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_create", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);  // thread ID output pointer
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);  // attributes (usually null)
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadJoin() {
    if (auto* fn = module->getFunction("pthread_join"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // int pthread_join(pthread_t thread, void **retval)
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {getDefaultType(), ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_join", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 1);  // retval output pointer
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexInit() {
    if (auto* fn = module->getFunction("pthread_mutex_init"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_init", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexLock() {
    if (auto* fn = module->getFunction("pthread_mutex_lock"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_lock", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexUnlock() {
    if (auto* fn = module->getFunction("pthread_mutex_unlock"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_unlock", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexDestroy() {
    if (auto* fn = module->getFunction("pthread_mutex_destroy"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_destroy", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareGetenv() {
    if (auto* fn = module->getFunction("getenv"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "getenv", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareSetenv() {
    if (auto* fn = module->getFunction("setenv"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i32Ty = llvm::Type::getInt32Ty(*context);
    auto* ty = llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy, i32Ty}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "setenv", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}
// ── BigInt runtime helper declarations ──────────────────────────────────────
// All bigint* functions take/return opaque pointers to heap-allocated OmBigInt
// objects.  The convention matches bigint_runtime.h (C ABI).

namespace {
/// Shorthand: (ptr) -> ptr — the most common bigint binary-op signature.
static llvm::FunctionType* bigintBinaryTy(llvm::LLVMContext& ctx) {
    auto* p = llvm::PointerType::getUnqual(ctx);
    return llvm::FunctionType::get(p, {p, p}, false);
}
/// Shorthand: (ptr) -> ptr — the unary signature.
static llvm::FunctionType* bigintUnaryTy(llvm::LLVMContext& ctx) {
    auto* p = llvm::PointerType::getUnqual(ctx);
    return llvm::FunctionType::get(p, {p}, false);
}
/// Shorthand: (ptr, ptr) -> i32 — comparison returning int.
static llvm::FunctionType* bigintCmpTy(llvm::LLVMContext& ctx) {
    auto* p = llvm::PointerType::getUnqual(ctx);
    return llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx), {p, p}, false);
}
} // namespace

llvm::Function* CodeGenerator::getOrDeclareBigintNewI64() {
    if (auto* fn = module->getFunction("omsc_bigint_new_i64")) return fn;
    auto* i64Ty = llvm::Type::getInt64Ty(*context);
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
    auto* fn = declareExternalFn("omsc_bigint_new_i64", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree); // allocates
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareBigintNewStr() {
    if (auto* fn = module->getFunction("omsc_bigint_new_str")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* fn = declareExternalFn("omsc_bigint_new_str", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareBigintFree() {
    if (auto* fn = module->getFunction("omsc_bigint_free")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {ptrTy}, false);
    auto* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage,
                                      "omsc_bigint_free", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareBigintAdd() {
    if (auto* fn = module->getFunction("omsc_bigint_add")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_add", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintSub() {
    if (auto* fn = module->getFunction("omsc_bigint_sub")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_sub", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintMul() {
    if (auto* fn = module->getFunction("omsc_bigint_mul")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_mul", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintDiv() {
    if (auto* fn = module->getFunction("omsc_bigint_div")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_div", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintMod() {
    if (auto* fn = module->getFunction("omsc_bigint_mod")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_mod", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintNeg() {
    if (auto* fn = module->getFunction("omsc_bigint_neg")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_neg", bigintUnaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintAbs() {
    if (auto* fn = module->getFunction("omsc_bigint_abs")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_abs", bigintUnaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintPow() {
    if (auto* fn = module->getFunction("omsc_bigint_pow")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_pow", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintGcd() {
    if (auto* fn = module->getFunction("omsc_bigint_gcd")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_gcd", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintEq() {
    if (auto* fn = module->getFunction("omsc_bigint_eq")) return fn;
    return declareExternalFn("omsc_bigint_eq", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintLt() {
    if (auto* fn = module->getFunction("omsc_bigint_lt")) return fn;
    return declareExternalFn("omsc_bigint_lt", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintLe() {
    if (auto* fn = module->getFunction("omsc_bigint_le")) return fn;
    return declareExternalFn("omsc_bigint_le", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintGt() {
    if (auto* fn = module->getFunction("omsc_bigint_gt")) return fn;
    return declareExternalFn("omsc_bigint_gt", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintGe() {
    if (auto* fn = module->getFunction("omsc_bigint_ge")) return fn;
    return declareExternalFn("omsc_bigint_ge", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintCmp() {
    if (auto* fn = module->getFunction("omsc_bigint_cmp")) return fn;
    return declareExternalFn("omsc_bigint_cmp", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintTostring() {
    if (auto* fn = module->getFunction("omsc_bigint_tostring")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* fn = declareExternalFn("omsc_bigint_tostring", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintToI64() {
    if (auto* fn = module->getFunction("omsc_bigint_to_i64")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt64Ty(*context), {ptrTy}, false);
    return declareExternalFn("omsc_bigint_to_i64", ty);
}
llvm::Function* CodeGenerator::getOrDeclareBigintBitLength() {
    if (auto* fn = module->getFunction("omsc_bigint_bit_length")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt64Ty(*context), {ptrTy}, false);
    return declareExternalFn("omsc_bigint_bit_length", ty);
}
llvm::Function* CodeGenerator::getOrDeclareBigintIsZero() {
    if (auto* fn = module->getFunction("omsc_bigint_is_zero")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i32Ty = llvm::Type::getInt32Ty(*context);
    auto* ty = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    return declareExternalFn("omsc_bigint_is_zero", ty);
}
llvm::Function* CodeGenerator::getOrDeclareBigintIsNegative() {
    if (auto* fn = module->getFunction("omsc_bigint_is_negative")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i32Ty = llvm::Type::getInt32Ty(*context);
    auto* ty = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    return declareExternalFn("omsc_bigint_is_negative", ty);
}
llvm::Function* CodeGenerator::getOrDeclareBigintShl() {
    if (auto* fn = module->getFunction("omsc_bigint_shl")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = llvm::Type::getInt64Ty(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
    auto* fn = declareExternalFn("omsc_bigint_shl", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintShr() {
    if (auto* fn = module->getFunction("omsc_bigint_shr")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = llvm::Type::getInt64Ty(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
    auto* fn = declareExternalFn("omsc_bigint_shr", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
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
    // Layout: [len, cap, key0, val0, hash0, key1, val1, hash1, ...]
    //   2 header slots + capacity * 3 slots (key+val+hash) = 2 + 3*cap slots
    static constexpr int64_t kInitCapacity    = 8;
    static constexpr int64_t kSlotsPerBucket  = 3;    // key, value, hash
    static constexpr int64_t kHeaderSlots     = 2;    // length, capacity
    static constexpr int64_t kInitTotalSlots  = kHeaderSlots + kSlotsPerBucket * kInitCapacity;
    static constexpr int64_t kInitBytes       = kInitTotalSlots * 8LL;

    llvm::Value* cap = llvm::ConstantInt::get(i64Ty, kInitCapacity);
    llvm::Value* totalBytes = llvm::ConstantInt::get(i64Ty, kInitBytes);
    // Use calloc so all hash slots start as 0 (empty)
    llvm::Value* buf = builder->CreateCall(getOrDeclareCalloc(), {
        llvm::ConstantInt::get(i64Ty, 1), totalBytes
    }, "hmap.buf");
    // OmScript assumes allocations always succeed — annotate call-site result.
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(
        llvm::Attribute::getWithDereferenceableBytes(*context, 208));
    // Store capacity in slot 0
    {   auto* st = builder->CreateAlignedStore(cap, buf, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_); }
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
    capLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
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
    auto* sizeLoad = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    {   // Size is always in [0, 2^62).
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        sizeLoad->setMetadata(llvm::LLVMContext::MD_range,
                              llvm::MDNode::get(*context, rangeMD));
    }
    llvm::Value* size = sizeLoad;
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
    {   auto* st = builder->CreateAlignedStore(newSize, sizePtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_); }
    builder->CreateBr(doneBB);

    // Grow: double capacity, rehash all entries, then insert the new key
    builder->SetInsertPoint(growBB);
    llvm::Value* newCap = builder->CreateShl(cap, llvm::ConstantInt::get(i64Ty, 1), "newcap", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* newTotalSlots = builder->CreateAdd(
        builder->CreateMul(newCap, three, "cap3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "newtotalslots", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* newTotalBytes = builder->CreateMul(newTotalSlots, llvm::ConstantInt::get(i64Ty, 8), "newtotalbytes", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* newBuf = builder->CreateCall(getOrDeclareCalloc(), {
        llvm::ConstantInt::get(i64Ty, 1), newTotalBytes
    }, "newbuf");
    llvm::cast<llvm::CallInst>(newBuf)->addRetAttr(llvm::Attribute::NonNull);
    // Store new capacity and size+1
    {   auto* st = builder->CreateAlignedStore(newCap, newBuf, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_); }
    llvm::Value* newSizePtr = builder->CreateInBoundsGEP(i64Ty, newBuf, llvm::ConstantInt::get(i64Ty, 1));
    {   auto* st = builder->CreateAlignedStore(newSize, newSizePtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_); }
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
    capLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
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
    capLoad_has->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
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
    capLoad_rm->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
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
    auto* sizeLoad_rm = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad_rm->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* size = sizeLoad_rm;
    llvm::Value* newSize = builder->CreateSub(size, llvm::ConstantInt::get(i64Ty, 1), "newsize", /*HasNUW=*/true, /*HasNSW=*/true);
    auto* sizeStore_rm = builder->CreateAlignedStore(newSize, sizePtr, llvm::MaybeAlign(8));
    sizeStore_rm->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
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
    auto* capLoad_keys = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    capLoad_keys->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* cap = capLoad_keys;
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1));
    auto* sizeLoad_keys = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad_keys->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* size = sizeLoad_keys;

    // Allocate output array: (size + 1) * 8
    llvm::Value* eight = llvm::ConstantInt::get(i64Ty, 8);
    llvm::Value* arrSlots = builder->CreateAdd(size, llvm::ConstantInt::get(i64Ty, 1), "arrslots", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrBytes = builder->CreateMul(arrSlots, eight, "arrbytes", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrBytes}, "buf");
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(
        llvm::Attribute::getWithDereferenceableBytes(*context, 8));
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
    auto* capLoad_vals = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    capLoad_vals->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* cap = capLoad_vals;
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1));
    auto* sizeLoad_vals = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad_vals->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* size = sizeLoad_vals;

    llvm::Value* eight = llvm::ConstantInt::get(i64Ty, 8);
    llvm::Value* arrSlots = builder->CreateAdd(size, llvm::ConstantInt::get(i64Ty, 1), "arrslots", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrBytes = builder->CreateMul(arrSlots, eight, "arrbytes", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrBytes}, "buf");
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(
        llvm::Attribute::getWithDereferenceableBytes(*context, 8));
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
    auto* sizeLoad_sz = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad_sz->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    // Size is always non-negative.
    {   llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        sizeLoad_sz->setMetadata(llvm::LLVMContext::MD_range,
                                 llvm::MDNode::get(*context, rangeMD));
    }
    builder->CreateRet(sizeLoad_sz);

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

// Helper: true if a type annotation string looks like an array type (ends with []).
static bool isArrayAnnotation(const std::string& ann) {
    return ann.size() >= 2 && ann.compare(ann.size() - 2, 2, "[]") == 0;
}

// Helper: true if an expression at the pre-analysis stage looks like it
// produces an array value (annotated [], array literal, array builtins).
static bool isPreAnalysisArrayExpr(Expression* expr,
                                    const llvm::StringSet<>& arrayReturningFunctions,
                                    const std::unordered_map<std::string, std::unordered_set<size_t>>& funcParamArrayTypes,
                                    const FunctionDecl* func) {
    if (!expr) return false;
    if (expr->type == ASTNodeType::ARRAY_EXPR) return true;
    if (expr->type == ASTNodeType::CALL_EXPR) {
        auto* call = static_cast<CallExpr*>(expr);
        // Known array-returning builtins
        static const std::unordered_set<std::string> arrayBuiltins = {
            "array_fill", "array_concat", "array_copy", "array_map",
            "array_filter", "array_slice", "sort", "reverse",
            "str_split", "str_chars", "push", "pop", "array_remove"
        };
        if (arrayBuiltins.count(call->callee)) return true;
        if (arrayReturningFunctions.count(call->callee)) return true;
    }
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR && func) {
        auto* id = static_cast<IdentifierExpr*>(expr);
        auto it = funcParamArrayTypes.find(func->name);
        if (it != funcParamArrayTypes.end()) {
            for (size_t i = 0; i < func->parameters.size(); ++i) {
                if (func->parameters[i].name == id->name && it->second.count(i))
                    return true;
            }
        }
    }
    return false;
}

void CodeGenerator::preAnalyzeArrayTypes(Program* program) {
    // Seed from explicit [] type annotations.
    for (auto& func : program->functions) {
        if (isArrayAnnotation(func->returnType)) {
            arrayReturningFunctions_.insert(func->name);
        }
        for (size_t i = 0; i < func->parameters.size(); ++i) {
            if (isArrayAnnotation(func->parameters[i].typeName)) {
                funcParamArrayTypes_[func->name].insert(i);
            }
        }
    }

    // Iterative propagation: scan bodies for array-returning return stmts and
    // call sites that pass array arguments.
    auto scanStmt = [&](auto& self, Statement* s) -> bool {
        if (!s) return false;
        if (auto* ret = dynamic_cast<ReturnStmt*>(s)) {
            return ret->value && isPreAnalysisArrayExpr(
                ret->value.get(), arrayReturningFunctions_, funcParamArrayTypes_, nullptr);
        }
        if (auto* blk = dynamic_cast<BlockStmt*>(s)) {
            for (auto& st : blk->statements)
                if (self(self, st.get())) return true;
        } else if (auto* ifS = dynamic_cast<IfStmt*>(s)) {
            if (self(self, ifS->thenBranch.get())) return true;
            if (self(self, ifS->elseBranch.get())) return true;
        } else if (auto* wh = dynamic_cast<WhileStmt*>(s)) {
            if (self(self, wh->body.get())) return true;
        } else if (auto* dw = dynamic_cast<DoWhileStmt*>(s)) {
            if (self(self, dw->body.get())) return true;
        } else if (auto* forS = dynamic_cast<ForStmt*>(s)) {
            if (self(self, forS->body.get())) return true;
        } else if (auto* fe = dynamic_cast<ForEachStmt*>(s)) {
            if (self(self, fe->body.get())) return true;
        }
        return false;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& func : program->functions) {
            if (!arrayReturningFunctions_.count(func->name)) {
                if (scanStmt(scanStmt, func->body.get())) {
                    arrayReturningFunctions_.insert(func->name);
                    changed = true;
                }
            }
            // Scan call sites to propagate array-ness to callee parameters.
            const std::function<void(Statement*)> scanCallSites = [&](Statement* st) {
                if (!st) return;
                auto scanExpr = [&](auto& se, Expression* e) -> void {
                    if (!e) return;
                    if (e->type == ASTNodeType::CALL_EXPR) {
                        auto* call = static_cast<CallExpr*>(e);
                        for (size_t i = 0; i < call->arguments.size(); ++i) {
                            if (isPreAnalysisArrayExpr(call->arguments[i].get(),
                                                       arrayReturningFunctions_,
                                                       funcParamArrayTypes_, func.get())) {
                                if (funcParamArrayTypes_[call->callee].insert(i).second)
                                    changed = true;
                            }
                            se(se, call->arguments[i].get());
                        }
                    } else if (e->type == ASTNodeType::BINARY_EXPR) {
                        auto* bin = static_cast<BinaryExpr*>(e);
                        se(se, bin->left.get()); se(se, bin->right.get());
                    } else if (e->type == ASTNodeType::UNARY_EXPR) {
                        se(se, static_cast<UnaryExpr*>(e)->operand.get());
                    }
                };
                auto scanSt = [&](auto& ss, Statement* s) -> void {
                    if (!s) return;
                    if (auto* es = dynamic_cast<ExprStmt*>(s))
                        scanExpr(scanExpr, es->expression.get());
                    else if (auto* vd = dynamic_cast<VarDecl*>(s))
                        scanExpr(scanExpr, vd->initializer.get());
                    else if (auto* rs = dynamic_cast<ReturnStmt*>(s))
                        scanExpr(scanExpr, rs->value.get());
                    else if (auto* blk = dynamic_cast<BlockStmt*>(s))
                        for (auto& st2 : blk->statements) ss(ss, st2.get());
                    else if (auto* ifS = dynamic_cast<IfStmt*>(s))
                        { ss(ss, ifS->thenBranch.get()); ss(ss, ifS->elseBranch.get()); }
                    else if (auto* wh = dynamic_cast<WhileStmt*>(s))
                        ss(ss, wh->body.get());
                    else if (auto* dw = dynamic_cast<DoWhileStmt*>(s))
                        ss(ss, dw->body.get());
                    else if (auto* fo = dynamic_cast<ForStmt*>(s))
                        ss(ss, fo->body.get());
                    else if (auto* fe = dynamic_cast<ForEachStmt*>(s))
                        ss(ss, fe->body.get());
                };
                scanSt(scanSt, st);
            };
            scanCallSites(func->body.get());
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
        // Check the runtime string-variable tracker first (handles params and
        // vars assigned from string function returns).
        if (stringVars_.count(id->name) > 0)
            return true;
        // Check the LLVM alloca type: a pointer-typed alloca is a string ONLY
        // if the variable is not known to be another pointer-holding type
        // (array, struct, dict).  Since resolveAnnotatedType now returns ptr
        // for arrays/structs/dicts, we must exclude those to avoid false
        // positives that would misroute array indexing to strlen-based paths.
        if (arrayVars_.count(id->name) || dictVarNames_.count(id->name)
            || structVars_.count(id->name) || stringArrayVars_.count(id->name))
            return false;
        auto it = namedValues.find(id->name);
        if (it != namedValues.end() && it->second) {
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);
            if (alloca && alloca->getAllocatedType()->isPointerTy())
                return true;
        }
        return false;
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
    if (expr->type == ASTNodeType::CALL_EXPR) {
        auto* call = static_cast<CallExpr*>(expr);
        // If the callee is known to return an array, it is NOT a string.
        if (arrayReturningFunctions_.count(call->callee)) return false;
        return stringReturningFunctions_.count(call->callee) > 0;
    }
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
    optMaxFunctionConfigs_.clear();
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
    // Operator overload functions must be appended to program->functions
    // BEFORE this loop so they get forward-declared like any other function.
    for (auto& structDecl : program->structs) {
        for (auto& overload : structDecl->operators) {
            if (overload.impl) {
                const std::string key = structDecl->name + "::" + overload.op;
                operatorOverloads_[key] = overload.impl->name;
                program->functions.push_back(std::move(overload.impl));
            }
        }
    }

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
                if (isUnsignedAnnotation(tn))
                    function->addParamAttr(i, llvm::Attribute::ZExt);
                else
                    function->addParamAttr(i, llvm::Attribute::SExt);
            }
        }
        function->addRetAttr(llvm::Attribute::NoUndef);
        if (retType->isIntegerTy()) {
            if (isUnsignedAnnotation(func->returnType))
                function->addRetAttr(llvm::Attribute::ZExt);
            else
                function->addRetAttr(llvm::Attribute::SExt);
        }
        // For functions with pointer return types (-> string, -> int[], -> dict,
        // -> StructType): OmScript guarantees non-null (runtime aborts on
        // allocation failure) and at least 8 bytes dereferenceable (every
        // array/string has a valid header slot).  These attributes propagate
        // through the optimizer and caller context even at O0, helping the
        // inliner and GVN prove non-null access without the full optimization
        // pipeline.
        if (retType->isPointerTy() && optimizationLevel >= OptimizationLevel::O1) {
            function->addRetAttr(llvm::Attribute::NonNull);
            function->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(
                *context, 8));
        }
        functions[func->name] = function;
        functionDecls_[func->name] = func.get();
    }

    // Process enum declarations: store constant values for identifier resolution.
    for (auto& enumDecl : program->enums) {
        std::vector<std::string> memberNames;
        for (auto& [memberName, memberValue] : enumDecl->members) {
            const std::string fullName = enumDecl->name + "_" + memberName;
            enumConstants_[fullName] = memberValue;
            memberNames.push_back(memberName);
        }
        enumMembers_[enumDecl->name] = std::move(memberNames);
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

    // ── Optimization pre-pass sequence ────────────────────────────────────
    //
    // All pre-pass analyses and AST transforms are coordinated by the
    // OptimizationOrchestrator, which:
    //   1. Runs each pass in dependency order (string types → array types →
    //      constant returns → purity → effects → synthesis → CF-CTRE → egraph)
    //   2. Records which facts are valid in the OptimizationContext
    //   3. Syncs the per-function analysis results into optCtx_ so that
    //      codegen can query a single surface instead of multiple maps.
    //
    // Behavior is identical to the previous inline sequence; the Orchestrator
    // simply makes the dependency graph explicit and facts centrally available.
    optCtx_ = std::make_unique<OptimizationContext>();
    optCtx_->setCTEngine(ctEngine_.get());

    {
        OptimizationOrchestrator orch(optimizationLevel, verbose_, this);
        orch.runPrepasses(program, *optCtx_);
    }

    if (verbose_) {
        std::cout << "  [codegen] Generating LLVM IR for " << program->functions.size()
                  << " functions..." << '\n';
    }

    // Generate all function bodies
    for (auto& func : program->functions) {
        // Loop fusion pre-pass: merge adjacent @fuse-annotated loops
        if (func->body) {
            fuseLoops(func->body.get());
        }
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
    //
    // We run multiple fixpoint iterations so that memory effects propagate
    // interprocedurally: once a callee gets readnone/readonly from iteration N,
    // its callers can be promoted in iteration N+1.  Converges in O(call depth)
    // passes — typically 2-3 for real programs.
    if (optimizationLevel >= OptimizationLevel::O1) {
        bool memEffChanged = true;
        while (memEffChanged) {
        memEffChanged = false;
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
            // Helper: strip GEP/BitCast/IntToPtr/PtrToInt chains to find
            // the underlying pointer source.  OmScript uses i64 as the
            // default type, so values frequently pass through IntToPtr and
            // PtrToInt conversions.  Stripping these is essential for
            // accurate memory-effect inference.
            auto stripPointerCasts = [](llvm::Value* ptr) -> llvm::Value* {
                for (unsigned i = 0; i < 16; ++i) { // depth limit
                    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
                        ptr = gep->getPointerOperand();
                    } else if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(ptr)) {
                        ptr = bc->getOperand(0);
                    } else if (auto* i2p = llvm::dyn_cast<llvm::IntToPtrInst>(ptr)) {
                        ptr = i2p->getOperand(0);
                    } else if (auto* p2i = llvm::dyn_cast<llvm::PtrToIntInst>(ptr)) {
                        ptr = p2i->getOperand(0);
                    } else {
                        break;
                    }
                }
                return ptr;
            };
            auto isLocalAlloca = [&stripPointerCasts](llvm::Value* ptr) -> bool {
                return llvm::isa<llvm::AllocaInst>(stripPointerCasts(ptr));
            };
            // Helper: strip chains and check if the base is a function
            // argument or a load from a function argument (one level of
            // indirection covers OmScript's array-of-pointers pattern).
            auto isArgDerived = [&](llvm::Value* ptr) -> bool {
                ptr = stripPointerCasts(ptr);
                if (funcArgs.count(ptr))
                    return true;
                // One level of load indirection: load from arg-derived ptr.
                if (auto* li = llvm::dyn_cast<llvm::LoadInst>(ptr)) {
                    llvm::Value* loadPtr = stripPointerCasts(li->getPointerOperand());
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
                memEffChanged = true;
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
                memEffChanged = true;
            } else if (!hasUnknownSideEffect && !hasMemoryWrite) {
                // Function reads memory (possibly non-arg) → readonly.
                func.addFnAttr(llvm::Attribute::getWithMemoryEffects(
                    *context, llvm::MemoryEffects::readOnly()));
                memEffChanged = true;
            }
        } // end for each function
        } // end while memEffChanged

        // Speculatable inference: after memory effects have converged, any
        // user function that is memory(none) + willreturn + nounwind is by
        // definition speculatable — it has no observable side effects and
        // always returns.  Marking it speculatable allows LLVM to:
        //   - hoist calls out of loops (LICM treats speculatable calls as pure)
        //   - eliminate duplicate calls (CSE / GVN merge call results)
        //   - speculate calls past conditional branches (SimplifyCFG/InstCombine)
        //   - prove that hoisting the call from a taken branch to a join point
        //     does not change program behavior
        // We only apply this to non-recursive user functions (recursive +
        // speculatable would let LLVM speculate recursive calls past the
        // base-case branch, turning finite recursion into infinite recursion).
        // @pure functions already get Speculatable earlier; skip them here.
        for (auto& func : module->functions()) {
            if (func.isDeclaration() || func.getName() == "main")
                continue;
            // Skip if already speculatable (set by @pure annotation).
            if (func.hasFnAttribute(llvm::Attribute::Speculatable))
                continue;
            // Requires memory(none) — no memory operations at all.
            if (!func.doesNotAccessMemory())
                continue;
            // Requires willreturn — the function always terminates.
            if (!func.hasFnAttribute(llvm::Attribute::WillReturn))
                continue;
            // Requires nounwind — already set for all user functions, but verify.
            if (!func.doesNotThrow())
                continue;
            // Exclude recursive functions to prevent infinite-recursion bugs.
            // A function is self-recursive if it directly calls itself.
            bool isSelfRecursive = false;
            for (auto& BB : func) {
                for (auto& I : BB) {
                    if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                        if (CI->getCalledFunction() == &func) {
                            isSelfRecursive = true;
                            break;
                        }
                    }
                }
                if (isSelfRecursive) break;
            }
            if (isSelfRecursive) continue;
            func.addFnAttr(llvm::Attribute::Speculatable);
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
                        // Handle indirect calls through IntToPtr/PtrToInt chains:
                        // OmScript may cast function pointers through integer
                        // types, making getCalledFunction() return nullptr.
                        // Strip casts to recover the underlying function.
                        if (!calledFn) {
                            llvm::Value* calledVal = CI->getCalledOperand();
                            for (unsigned d = 0; d < 8; ++d) {
                                if (auto* i2p = llvm::dyn_cast<llvm::IntToPtrInst>(calledVal))
                                    calledVal = i2p->getOperand(0);
                                else if (auto* p2i = llvm::dyn_cast<llvm::PtrToIntInst>(calledVal))
                                    calledVal = p2i->getOperand(0);
                                else if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(calledVal))
                                    calledVal = bc->getOperand(0);
                                else
                                    break;
                            }
                            if (auto* gv = llvm::dyn_cast<llvm::Function>(calledVal)) {
                                if (gv->getName() == func.getName()) {
                                    callsSelf = true;
                                    break;
                                }
                            }
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
            std::cout << "  [opt] Running OPTMAX per-function optimization passes..." << '\n';
        }
        optimizeOptMaxFunctions();
    }

    // Mark all constant global variables with unnamed_addr at O2+.
    // unnamed_addr tells LLVM that the address of the global is not
    // significant — only its value matters — allowing:
    //   1. ConstantMerge to merge identical globals regardless of address.
    //   2. The linker to merge constants from different TUs (when LTO is used).
    //   3. Backend to place the constant in a read-only section closer to
    //      the code that uses it, improving I-cache locality.
    //
    // We apply this to all constant (isConstant=true) globals except those
    // already marked unnamed_addr or local_unnamed_addr, and except
    // externally-visible globals (which may be referenced by address from
    // other TUs, though OmScript programs are single-TU).
    //
    // This is a cheap pre-optimizer annotation pass — O(globals) time.
    if (optimizationLevel >= OptimizationLevel::O2) {
        for (auto& gv : module->globals()) {
            if (!gv.isConstant()) continue;
            if (gv.getUnnamedAddr() != llvm::GlobalValue::UnnamedAddr::None) continue;
            // Don't annotate externally-visible globals with addresses that
            // external code might rely on (e.g. exported string constants).
            if (gv.hasExternalLinkage()) continue;
            // Skip globals that are address-taken in non-load instructions
            // (i.e. their pointer escapes beyond simple loads).  A simple
            // heuristic: if all uses are loads or GEP-then-loads, unnamed_addr
            // is safe.  We check this conservatively: only mark if no use is
            // a store, call (with the global as a pointer arg), or phi.
            bool addrTakenAsValue = false;
            for (auto* user : gv.users()) {
                if (llvm::isa<llvm::LoadInst>(user)) continue;
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
                    // GEP is fine if all GEP users are loads.
                    bool gepAllLoads = true;
                    for (auto* gepUser : gep->users()) {
                        if (!llvm::isa<llvm::LoadInst>(gepUser)) {
                            gepAllLoads = false;
                            break;
                        }
                    }
                    if (gepAllLoads) continue;
                }
                if (auto* constExpr = llvm::dyn_cast<llvm::ConstantExpr>(user)) {
                    // GEP constant expr: also fine if only loaded from.
                    bool ceAllLoads = true;
                    for (auto* ceUser : constExpr->users()) {
                        if (!llvm::isa<llvm::LoadInst>(ceUser)) {
                            ceAllLoads = false;
                            break;
                        }
                    }
                    if (ceAllLoads) continue;
                }
                addrTakenAsValue = true;
                break;
            }
            if (!addrTakenAsValue) {
                gv.setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            }
        }
    }

    if (verbose_) {
        std::cout << "  [opt] Running LLVM optimization pipeline..." << '\n';
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

    // Print optimization statistics when verbose mode is enabled.
    if (verbose_) {
        optStats_.print();
    }
}

llvm::Function* CodeGenerator::generateFunction(FunctionDecl* func) {
    inOptMaxFunction = func->isOptMax;
    hasOptMaxFunctions = hasOptMaxFunctions || func->isOptMax;
    currentOptMaxConfig_ = func->optMaxConfig;
    if (func->isOptMax) {
        currentOptMaxConfig_.enabled = currentOptMaxConfig_.enabled || !func->optMaxConfig.enabled;
        // safety=off implies fastMath
        if (currentOptMaxConfig_.safety == SafetyLevel::Off) {
            currentOptMaxConfig_.fastMath = true;
        }
        // Store the resolved config for optimizeOptMaxFunctions() to consume later.
        optMaxFunctionConfigs_[func->name] = currentOptMaxConfig_;
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
        function->setDoesNotFreeMemory();
        // NoSync: the function does not communicate with other threads via
        // memory or synchronization primitives.  Required for LLVM to move
        // the call freely across other memory operations.
        function->setNoSync();
        // Speculatable + willreturn are only safe on non-recursive pure
        // functions.  For a recursive function, speculatable lets LLVM
        // hoist/speculate recursive calls past the base-case branch
        // (converting branches into selects), which turns finite recursion
        // into infinite recursion and a guaranteed stack overflow.
        if (!isSelfRecursive) {
            function->setWillReturn();
            function->addFnAttr(llvm::Attribute::Speculatable);
        }
    }
    if (func->hintConstEval) {
        // @const_eval: mark the function for compile-time evaluation when
        // called with all-constant arguments.  At the LLVM level, mark it
        // as pure + inline so that any fallback runtime call is efficient.
        constEvalFunctions_.insert(func->name);
        if (optCtx_) optCtx_->mutableFacts(func->name).isConstFoldable = true;
        function->addFnAttr(llvm::Attribute::InlineHint);
        function->setOnlyReadsMemory();
        function->setDoesNotThrow();
        function->setWillReturn();
    }
    // Auto-apply LLVM memory-effect attributes based on inferred effects.
    // Only applied when @pure is NOT already set (which sets readonly explicitly).
    // Query the unified OptimizationContext so IR emission uses the single
    // canonical fact surface (populated by syncFactsToContext before codegen).
    if (!func->hintPure && optCtx_) {
        const FunctionEffects& fx = optCtx_->effects(func->name);
        if (fx.isReadNone()) {
            // No memory access, no I/O: readnone + nosync + willreturn
            function->setDoesNotAccessMemory();
            function->setNoSync();
            if (!isSelfRecursive) {
                function->setWillReturn();
                function->addFnAttr(llvm::Attribute::Speculatable);
            }
        } else if (fx.isReadOnly()) {
            // Reads memory but does not write or do I/O: readonly + nosync
            function->setOnlyReadsMemory();
            function->setNoSync();
            if (!isSelfRecursive)
                function->setWillReturn();
        } else if (fx.isNoSync()) {
            // Has writes but no I/O: nosync
            function->setNoSync();
        }
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

    // @allocator(size=N) / @allocator(size=N, count=M): mark as allocator wrapper.
    // Adds allocsize, noalias return (only for pointer returns), WillReturn,
    // and NoUnwind so LLVM's alias analysis can track allocation sizes.
    if (func->allocatorSizeParam >= 0) {
        const unsigned sizeIdx  = static_cast<unsigned>(func->allocatorSizeParam);
        if (func->allocatorCountParam >= 0) {
            const unsigned countIdx = static_cast<unsigned>(func->allocatorCountParam);
            function->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, sizeIdx, countIdx));
        } else {
            function->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, sizeIdx, std::nullopt));
        }
        // noalias is only valid on pointer return types.
        if (function->getReturnType()->isPointerTy()) {
            function->addRetAttr(llvm::Attribute::NoAlias);
        }
        function->addFnAttr(llvm::Attribute::WillReturn);
        function->addFnAttr(llvm::Attribute::NoUnwind);
        optStats_.allocatorFuncs++;
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
                // OmScript arrays/strings are allocated via calloc/malloc which
                // guarantees at least 16-byte alignment on 64-bit Linux.  The
                // align(16) attribute communicates this to LLVM's vectorizer,
                // enabling aligned vector load/store instructions (movdqa, vmovdqa)
                // and better SLP vectorization on heap-allocated buffers.
                function->addParamAttr(i, llvm::Attribute::getWithAlignment(
                    *context, llvm::Align(16)));
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
                function->addParamAttr(i, llvm::Attribute::getWithAlignment(
                    *context, llvm::Align(16)));
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
                // OmScript arrays/strings are allocated by calloc/malloc which
                // guarantees at least 16-byte alignment on 64-bit Linux/macOS.
                // align(16) communicates the actual guarantee (not just the
                // conservative lower bound), enabling LLVM to use aligned
                // vector instructions on all heap pointer parameters.
                function->addParamAttr(i, llvm::Attribute::getWithAlignment(
                    *context, llvm::Align(16)));
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
    currentFuncDecl_ = func;

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
    const llvm::FastMathFlags savedFMF = builder->getFastMathFlags();
    if (inOptMaxFunction && !useFastMath_ && currentOptMaxConfig_.fastMath) {
        // fast_math=true (or safety=off, which implies it): enable all fast-math
        // optimizations — FMA fusion, reassociation, reciprocal approximations,
        // relaxed NaN/Inf semantics, and no-signed-zeros.
        llvm::FastMathFlags FMF;
        FMF.setFast();
        builder->setFastMathFlags(FMF);
        // Also set function-level string attributes so LTO and the back-end
        // can see the fast-math contract without inspecting every instruction.
        function->addFnAttr("unsafe-fp-math", "true");
        function->addFnAttr("no-nans-fp-math", "true");
        function->addFnAttr("no-infs-fp-math", "true");
        function->addFnAttr("no-signed-zeros-fp-math", "true");
        function->addFnAttr("approx-func-fp-math", "true");
        function->addFnAttr("no-trapping-math", "true");
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
    varBorrowStates_.clear();
    borrowMap_.clear();
    borrowScopeStack_.clear();
    frozenVars_.clear();
    prefetchedParams_.clear();
    prefetchedVars_.clear();
    prefetchedImmutVars_.clear();
    registerVars_.clear();
    simdVars_.clear();
    dictVarNames_.clear();
    nonNegValues_.clear();
    constIntFolds_.clear();
    constFloatFolds_.clear();
    stackAllocatedArrays_.clear();
    pendingArrayStackAlloc_ = false;
    scopeComptimeInts_.clear();
    catchTable_.clear();
    catchDefaultBB_ = nullptr;

    // Pre-populate stringVars_ for parameters known to receive string arguments.
    auto paramStrIt = funcParamStringTypes_.find(func->name);
    auto paramArrIt = funcParamArrayTypes_.find(func->name);
    auto argIt = function->arg_begin();
    for (size_t paramIdx = 0; paramIdx < func->parameters.size(); ++paramIdx) {
        auto& param = func->parameters[paramIdx];
        argIt->setName(param.name);

        // Use the parameter's actual LLVM type (respects type annotations).
        llvm::AllocaInst* alloca = createEntryBlockAlloca(function, param.name, argIt->getType());
        builder->CreateStore(&(*argIt), alloca);
        bindVariable(param.name, alloca);
        // Annotate parameter with its declared type for signed/unsigned tracking.
        if (!param.typeName.empty())
            varTypeAnnotations_[param.name] = param.typeName;

        if (paramStrIt != funcParamStringTypes_.end() && paramStrIt->second.count(paramIdx))
            stringVars_.insert(param.name);
        // Pre-populate arrayVars_ for parameters known to receive array arguments.
        // This ensures !nonnull and !range annotations propagate to loads of
        // array parameter variables inside the function body.
        if (paramArrIt != funcParamArrayTypes_.end() && paramArrIt->second.count(paramIdx))
            arrayVars_.insert(param.name);

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

    // @optmax assumes=[...]: emit llvm.assume intrinsics for each assertion
    // in the config.  Assumptions help LLVM prove range/sign/non-null facts
    // that the programmer guarantees but the compiler cannot derive.
    // Supported patterns: "var > N", "var >= N", "var != N", "var == N",
    // "var < N", "var <= N" where var is a parameter name and N is an integer.
    if (inOptMaxFunction && !currentOptMaxConfig_.assumes.empty()) {
        llvm::Function* assumeIntr = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume);
        for (const auto& assumeStr : currentOptMaxConfig_.assumes) {
            // Tokenise: split on whitespace into [var, op, literal]
            std::istringstream ss(assumeStr);
            std::string varName, op, litStr;
            if (!(ss >> varName >> op >> litStr)) continue;
            int64_t litVal = 0;
            try { litVal = std::stoll(litStr); } catch (...) { continue; }
            // Look up the variable in namedValues (covers all parameters just stored).
            auto nv = namedValues.find(varName);
            if (nv == namedValues.end()) continue;
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(nv->second);
            if (!alloca) continue;
            llvm::Value* loaded = builder->CreateLoad(
                alloca->getAllocatedType(), alloca, varName + ".assume.load");
            // Cast to i64 for comparison (handles both int and float params).
            llvm::Value* cmpVal = loaded;
            if (cmpVal->getType()->isDoubleTy())
                cmpVal = builder->CreateFPToSI(cmpVal, getDefaultType(), varName + ".assume.fptoi");
            else if (cmpVal->getType() != getDefaultType())
                cmpVal = builder->CreateSExtOrBitCast(cmpVal, getDefaultType(), varName + ".assume.cast");
            llvm::Value* lit = llvm::ConstantInt::get(getDefaultType(), litVal, /*isSigned=*/true);
            llvm::Value* cond = nullptr;
            if      (op == ">")  cond = builder->CreateICmpSGT(cmpVal, lit, varName + ".assume.cond");
            else if (op == ">=") cond = builder->CreateICmpSGE(cmpVal, lit, varName + ".assume.cond");
            else if (op == "<")  cond = builder->CreateICmpSLT(cmpVal, lit, varName + ".assume.cond");
            else if (op == "<=") cond = builder->CreateICmpSLE(cmpVal, lit, varName + ".assume.cond");
            else if (op == "!=") cond = builder->CreateICmpNE (cmpVal, lit, varName + ".assume.cond");
            else if (op == "==") cond = builder->CreateICmpEQ (cmpVal, lit, varName + ".assume.cond");
            if (cond) {
                builder->CreateCall(assumeIntr, {cond});
                nonNegValues_.insert(cmpVal);  // help downstream range analysis
            }
        }
    }

    // @optmax memory.prefetch=true: auto-prefetch all pointer-type parameters
    // at function entry.  This hints the CPU to load the pointed-to data into
    // the L1/L2 cache before the first access, hiding memory latency for
    // array-heavy numeric functions.
    if (inOptMaxFunction && currentOptMaxConfig_.memory.prefetch) {
        llvm::Function* prefetchFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::prefetch,
            {llvm::PointerType::getUnqual(*context)});
        for (size_t i = 0; i < func->parameters.size(); ++i) {
            const auto& param = func->parameters[i];
            if (prefetchedParams_.count(param.name)) continue;  // already prefetched
            auto it = namedValues.find(param.name);
            if (it == namedValues.end()) continue;
            auto* al = llvm::dyn_cast<llvm::AllocaInst>(it->second);
            if (!al) continue;
            llvm::Value* val = builder->CreateLoad(al->getAllocatedType(), al,
                                                   param.name + ".autopf.load");
            // Treat as a pointer (i64 → ptr cast) and prefetch the target memory.
            llvm::Value* ptr = val->getType()->isPointerTy()
                ? val
                : builder->CreateIntToPtr(val, llvm::PointerType::getUnqual(*context),
                                          param.name + ".autopf.ptr");
            builder->CreateCall(prefetchFn, {
                ptr,
                builder->getInt32(0),  // read prefetch
                builder->getInt32(3),  // high temporal locality — keep in all levels
                builder->getInt32(1)   // data cache
            });
            prefetchedParams_.insert(param.name);
        }
    }

    // Pre-pass: collect all catch(code) blocks in this function body,
    // assign integer IDs to string codes, and create BasicBlocks for each.
    // Must happen before generating the body so throw sites can reference BBs.
    buildCatchTable(func->body->statements, function);

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
    case ASTNodeType::CATCH_STMT:
        generateCatch(static_cast<CatchStmt*>(stmt));
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
    case ASTNodeType::FREEZE_STMT:
        generateFreeze(static_cast<FreezeStmt*>(stmt));
        break;
    case ASTNodeType::PREFETCH_STMT:
        generatePrefetch(static_cast<PrefetchStmt*>(stmt));
        break;
    case ASTNodeType::ASSUME_STMT:
        generateAssume(static_cast<AssumeStmt*>(stmt));
        break;
    case ASTNodeType::DEFER_STMT:
        // Defer outside a block: just execute the body immediately
        // (normal defer semantics are handled in generateBlock)
        generateStatement(static_cast<DeferStmt*>(stmt)->body.get());
        break;
    case ASTNodeType::PIPELINE_STMT:
        generatePipeline(static_cast<PipelineStmt*>(stmt));
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
        callExpr->fromStdNamespace = true; // pipe-forward desugaring
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
    case ASTNodeType::REBORROW_EXPR:
        return generateReborrowExpr(static_cast<ReborrowExpr*>(expr));
    case ASTNodeType::DICT_EXPR:
        return generateDict(static_cast<DictExpr*>(expr));
    case ASTNodeType::SCOPE_RESOLUTION_EXPR:
        return generateScopeResolution(static_cast<ScopeResolutionExpr*>(expr));
    case ASTNodeType::COMPTIME_EXPR: {
        // comptime { ... } — evaluate the block at compile time.
        // Primary: use CF-CTRE engine (richer cross-function evaluation + memoisation).
        // Fallback: existing tryConstEvalFull (maintains backward compatibility).
        auto* ct = static_cast<ComptimeExpr*>(expr);

        // Try CF-CTRE first (available after runCFCTRE has been called).
        if (ctEngine_) {
            auto ctResult = ctEngine_->evalComptimeBlock(ct->body.get(),
                                                         buildComptimeEnv());
            if (ctResult) {
                // Stash the CT value so the VarDecl handler can register it
                // under the variable name, making it visible to subsequent
                // comptime blocks in the same function scope.
                lastComptimeCtResult_ = *ctResult;
                if (ctResult->isInt()) {
                    return llvm::ConstantInt::get(getDefaultType(), ctResult->asI64(), /*isSigned=*/true);
                }
                if (ctResult->isString()) {
                    llvm::GlobalVariable* gv = internString(ctResult->asStr());
                    return llvm::ConstantExpr::getInBoundsGetElementPtr(
                        gv->getValueType(), gv,
                        llvm::ArrayRef<llvm::Constant*>{
                            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
                }
                if (ctResult->isArray()) {
                    auto cv = ctValueToConstValue(*ctResult);
                    return emitComptimeArray(cv.arrVal);
                }
            }
        }

        // Fallback: legacy tryConstEvalFull evaluator.
        static const std::unordered_map<std::string, ConstValue> emptyEnv;
        auto result = tryConstEvalFull(ct->body.get(), emptyEnv);
        if (!result) {
            codegenError("comptime block could not be evaluated at compile time", expr);
        }
        if (result->kind == ConstValue::Kind::Integer) {
            return llvm::ConstantInt::get(getDefaultType(), result->intVal, /*isSigned=*/true);
        }
        if (result->kind == ConstValue::Kind::String) {
            llvm::GlobalVariable* gv = internString(result->strVal);
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                gv->getValueType(), gv,
                llvm::ArrayRef<llvm::Constant*>{
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
        if (result->kind == ConstValue::Kind::Array) {
            return emitComptimeArray(result->arrVal);
        }
        codegenError("comptime block returned an unsupported type (only int/string/array supported)", expr);
    }
    default:
        codegenError("Unknown expression type", expr);
    }
}

} // namespace omscript

// ---------------------------------------------------------------------------
// Compile-time function evaluation for @const_eval
// ---------------------------------------------------------------------------
// Simple AST-level interpreter for integer-only functions.  Supports:
//   - Integer arithmetic (+, -, *, /, %, **, &, |, ^, <<, >>)
//   - Comparisons (==, !=, <, <=, >, >=)
//   - Logical operators (&&, ||, !)
//   - Unary operators (-, ~)
//   - If/else statements
//   - Return statements
//   - Variable declarations and assignments
//   - Recursive calls to @const_eval functions
//
// Returns std::nullopt for unsupported constructs, falling back to runtime.
namespace omscript {

std::optional<int64_t> CodeGenerator::tryConstEval(
    const FunctionDecl* func,
    const std::vector<int64_t>& argVals) {

    if (!func || !func->body || func->parameters.size() != argVals.size())
        return std::nullopt;

    // Environment: variable name → current integer value
    std::unordered_map<std::string, int64_t> env;
    for (size_t i = 0; i < argVals.size(); ++i) {
        env[func->parameters[i].name] = argVals[i];
    }

    // Limit recursion depth to prevent infinite loops at compile time
    static thread_local int depth = 0;
    if (++depth > 100) { --depth; return std::nullopt; }

    struct DepthGuard { ~DepthGuard() { --depth; } } const guard;

    // Return value (set by a ReturnStmt)
    std::optional<int64_t> retVal;

    // Forward declarations for mutual recursion
    std::function<std::optional<int64_t>(Expression*)> evalExpr;
    std::function<bool(Statement*)> evalStmt;

    evalExpr = [&](Expression* e) -> std::optional<int64_t> {
        if (!e) return std::nullopt;
        switch (e->type) {
        case ASTNodeType::LITERAL_EXPR: {
            auto* lit = static_cast<LiteralExpr*>(e);
            if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
                return lit->intValue;
            // Booleans are represented as integers in OmScript (0 or 1).
            // Float literals are not supported in const_eval context.
            return std::nullopt;
        }
        case ASTNodeType::IDENTIFIER_EXPR: {
            auto* id = static_cast<IdentifierExpr*>(e);
            auto it = env.find(id->name);
            if (it != env.end()) return it->second;
            // Check enum constants
            auto eit = enumConstants_.find(id->name);
            if (eit != enumConstants_.end()) return static_cast<int64_t>(eit->second);
            return std::nullopt;
        }
        case ASTNodeType::BINARY_EXPR: {
            auto* bin = static_cast<BinaryExpr*>(e);
            auto lv = evalExpr(bin->left.get());
            // Short-circuit for logical operators
            if (bin->op == "&&") {
                if (!lv) return std::nullopt;
                if (*lv == 0) return int64_t(0);
                auto rv = evalExpr(bin->right.get());
                return rv ? std::optional<int64_t>(*rv != 0 ? 1 : 0) : std::nullopt;
            }
            if (bin->op == "||") {
                if (!lv) return std::nullopt;
                if (*lv != 0) return int64_t(1);
                auto rv = evalExpr(bin->right.get());
                return rv ? std::optional<int64_t>(*rv != 0 ? 1 : 0) : std::nullopt;
            }
            auto rv = evalExpr(bin->right.get());
            if (!lv || !rv) return std::nullopt;
            const int64_t a = *lv, b = *rv;
            // Wrapping arithmetic helper: cast to uint64_t to avoid signed
            // overflow UB, then cast the bit-pattern back to int64_t.
            auto wrap = [](int64_t x, int64_t y, char op) -> int64_t {
                const auto ux = static_cast<uint64_t>(x);
                const auto uy = static_cast<uint64_t>(y);
                switch (op) {
                case '+': return static_cast<int64_t>(ux + uy);
                case '-': return static_cast<int64_t>(ux - uy);
                case '*': return static_cast<int64_t>(ux * uy);
                default:  return 0;
                }
            };
            // Use wrapping arithmetic for +, -, * to match i64 runtime semantics.
            if (bin->op == "+") return wrap(a, b, '+');
            if (bin->op == "-") return wrap(a, b, '-');
            if (bin->op == "*") return wrap(a, b, '*');
            // Guard against /0 and the INT64_MIN/-1 trap.
            if (bin->op == "/" && b != 0) {
                if (a == std::numeric_limits<int64_t>::min() && b == -1) return std::nullopt;
                return a / b;
            }
            if (bin->op == "%" && b != 0) {
                if (a == std::numeric_limits<int64_t>::min() && b == -1) return int64_t(0);
                return a % b;
            }
            if (bin->op == "&") return a & b;
            if (bin->op == "|") return a | b;
            if (bin->op == "^") return a ^ b;
            if (bin->op == "<<" && b >= 0 && b < 64) return a << b;
            if (bin->op == ">>" && b >= 0 && b < 64) return a >> b;
            if (bin->op == "==") return int64_t(a == b ? 1 : 0);
            if (bin->op == "!=") return int64_t(a != b ? 1 : 0);
            if (bin->op == "<")  return int64_t(a < b ? 1 : 0);
            if (bin->op == "<=") return int64_t(a <= b ? 1 : 0);
            if (bin->op == ">")  return int64_t(a > b ? 1 : 0);
            if (bin->op == ">=") return int64_t(a >= b ? 1 : 0);
            if (bin->op == "**") {
                if (b < 0) return (a == 1) ? int64_t(1) : (a == -1 ? (b & 1 ? int64_t(-1) : int64_t(1)) : int64_t(0));
                // Binary exponentiation O(log n) — avoids pathological compile times
                // for expressions like 2**1000000 appearing in const-eval contexts.
                uint64_t result = 1, base = static_cast<uint64_t>(a);
                int64_t exp = b;
                while (exp > 0) {
                    if (exp & 1) result *= base;
                    base *= base;
                    exp >>= 1;
                }
                return static_cast<int64_t>(result);
            }
            return std::nullopt;
        }
        case ASTNodeType::UNARY_EXPR: {
            auto* un = static_cast<UnaryExpr*>(e);
            auto v = evalExpr(un->operand.get());
            if (!v) return std::nullopt;
            if (un->op == "-") return -*v;
            if (un->op == "~") return ~*v;
            if (un->op == "!") return int64_t(*v == 0 ? 1 : 0);
            return std::nullopt;
        }
        case ASTNodeType::TERNARY_EXPR: {
            auto* tern = static_cast<TernaryExpr*>(e);
            auto cond = evalExpr(tern->condition.get());
            if (!cond) return std::nullopt;
            return *cond != 0 ? evalExpr(tern->thenExpr.get())
                              : evalExpr(tern->elseExpr.get());
        }
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<CallExpr*>(e);
            // Zero-arg constant-returning functions (classified by pre-pass).
            // Prefer the unified OptimizationContext; fall back to private map
            // when optCtx_ is not yet available (called during analysis passes).
            if (call->arguments.empty()) {
                if (optCtx_) {
                    if (auto v = optCtx_->constIntReturn(call->callee)) return *v;
                } else {
                    auto it = constIntReturnFunctions_.find(call->callee);
                    if (it != constIntReturnFunctions_.end()) return it->second;
                }
            }
            // Recursive @const_eval call
            const bool isConstEval = optCtx_
                ? optCtx_->isConstFoldable(call->callee)
                : (constEvalFunctions_.count(call->callee) > 0);
            if (!isConstEval)
                return std::nullopt;
            auto declIt = functionDecls_.find(call->callee);
            if (declIt == functionDecls_.end()) return std::nullopt;
            std::vector<int64_t> callArgs;
            callArgs.reserve(call->arguments.size());
            for (auto& arg : call->arguments) {
                auto av = evalExpr(arg.get());
                if (!av) return std::nullopt;
                callArgs.push_back(*av);
            }
            return tryConstEval(declIt->second, callArgs);
        }
        case ASTNodeType::SCOPE_RESOLUTION_EXPR: {
            auto* sr = static_cast<ScopeResolutionExpr*>(e);
            const std::string fullName = sr->scopeName + "_" + sr->memberName;
            auto eit = enumConstants_.find(fullName);
            if (eit != enumConstants_.end()) return static_cast<int64_t>(eit->second);
            return std::nullopt;
        }
        default:
            return std::nullopt;
        }
    };

    evalStmt = [&](Statement* s) -> bool {
        if (!s || retVal) return true;  // already returned
        switch (s->type) {
        case ASTNodeType::RETURN_STMT: {
            auto* ret = static_cast<ReturnStmt*>(s);
            if (ret->value) {
                auto v = evalExpr(ret->value.get());
                if (!v) return false;
                retVal = v;
            } else {
                retVal = 0;
            }
            return true;
        }
        case ASTNodeType::VAR_DECL: {
            auto* decl = static_cast<VarDecl*>(s);
            if (decl->initializer) {
                auto v = evalExpr(decl->initializer.get());
                if (!v) return false;
                env[decl->name] = *v;
            } else {
                env[decl->name] = 0;
            }
            return true;
        }
        case ASTNodeType::EXPR_STMT: {
            auto* es = static_cast<ExprStmt*>(s);
            if (es->expression->type == ASTNodeType::ASSIGN_EXPR) {
                auto* assign = static_cast<AssignExpr*>(es->expression.get());
                auto v = evalExpr(assign->value.get());
                if (!v) return false;
                env[assign->name] = *v;
                return true;
            }
            // Other expression statements: just evaluate for side effects
            auto v = evalExpr(es->expression.get());
            return v.has_value();
        }
        case ASTNodeType::IF_STMT: {
            auto* ifs = static_cast<IfStmt*>(s);
            auto cond = evalExpr(ifs->condition.get());
            if (!cond) return false;
            if (*cond != 0) {
                return evalStmt(ifs->thenBranch.get());
            } else if (ifs->elseBranch) {
                return evalStmt(ifs->elseBranch.get());
            }
            return true;
        }
        case ASTNodeType::BLOCK: {
            auto* block = static_cast<BlockStmt*>(s);
            for (auto& stmt : block->statements) {
                if (!evalStmt(stmt.get())) return false;
                if (retVal) return true;
            }
            return true;
        }
        default:
            return false;  // Unsupported statement type
        }
    };

    // Evaluate the function body
    for (auto& stmt : func->body->statements) {
        if (!evalStmt(stmt.get())) return std::nullopt;
        if (retVal) return retVal;
    }
    return retVal.value_or(0);
}

// ── Unified Compile-Time Expression & Function Evaluator ─────────────────
//
// evalConstBuiltin: shared helper called by both tryFoldExprToConst and
// tryConstEvalFull.  Given an already-evaluated argument list (ConstValues),
// applies the named built-in pure function and returns the result.
// Returns nullopt if the builtin is unknown, impure, or the args are the
// wrong type/count.
// ─────────────────────────────────────────────────────────────────────────────
std::optional<CodeGenerator::ConstValue> CodeGenerator::evalConstBuiltin(
    const std::string& name,
    const std::vector<CodeGenerator::ConstValue>& args)
{
    using CV = CodeGenerator::ConstValue;

    // ── Fast reject: skip entire if-chain for unknown builtin names ──────
    static const std::unordered_set<std::string> kKnownBuiltins = {
        "len","str_len","abs","min","max","sign","clamp","pow","sqrt","gcd",
        "lcm","log","log2","log10","exp2","is_even","is_odd","floor","ceil","round",
        "to_char","is_alpha","is_digit","to_string","number_to_string",
        "to_int","string_to_number","str_to_int","char_code","str_find",
        "str_index_of","str_contains","str_starts_with","startswith",
        "str_ends_with","endswith","str_substr","str_upper","str_lower",
        "str_repeat","str_trim","str_reverse","str_count","str_replace",
        "str_pad_left","str_pad_right","str_eq","str_concat","char_at",
        "is_power_of_2","popcount","clz","ctz","bitreverse","bswap",
        "rotate_left","rotate_right","saturating_add","saturating_sub",
        "str_chars","typeof","array_fill","array_concat","array_slice",
        "array_contains","index_of","array_find","array_min","array_max",
        "array_last","array_product","sum",
        "is_upper","is_lower","is_space","is_alnum",
        "fast_add","fast_sub","fast_mul","fast_div",
        "precise_add","precise_sub","precise_mul","precise_div",
        "sin","cos","tan","asin","acos","atan","atan2","cbrt","hypot",
        "fma","copysign","min_float","max_float",
        "reverse","sort","array_remove","array_insert",
        "array_any","array_every","array_count",
        // int/uint/bool kept here; iN/uN are handled by isIntWidthCastName below
        "int","uint","bool"
    };
    // Check if name is in the known-builtins set OR is an iN/uN type-cast name.
    auto isIntWidthCastName = [](const std::string& nm) -> bool {
        if (nm.size() < 2 || (nm[0] != 'i' && nm[0] != 'u')) return false;
        int bw = 0;
        for (size_t j = 1; j < nm.size(); ++j) {
            if (!std::isdigit(static_cast<unsigned char>(nm[j]))) return false;
            bw = bw * 10 + (nm[j] - '0');
            if (bw > 256) return false;
        }
        return bw >= 1 && bw <= 256;
    };
    if (kKnownBuiltins.find(name) == kKnownBuiltins.end() && !isIntWidthCastName(name))
        return std::nullopt;

    const size_t n = args.size();
    auto intArg = [&](size_t i) -> std::optional<int64_t> {
        if (i < n && args[i].kind == CV::Kind::Integer) return args[i].intVal;
        return std::nullopt;
    };
    // strArg returns a pointer into args[i].strVal — valid for the duration of
    // this call since `args` is a const-ref to the caller's vector.
    auto strArg = [&](size_t i) -> const std::string* {
        if (i < n && args[i].kind == CV::Kind::String) return &args[i].strVal;
        return nullptr;
    };

    // ── len(x) ─────────────────────────────────────────────────────────────
    if (name == "len" && n == 1) {
        if (auto s = strArg(0)) return CV::fromInt(static_cast<int64_t>(s->size()));
        if (n == 1 && args[0].kind == CV::Kind::Array)
            return CV::fromInt(static_cast<int64_t>(args[0].arrVal.size()));
        return std::nullopt;
    }
    // ── abs(x) ─────────────────────────────────────────────────────────────
    if (name == "abs" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt(*v < 0 ? -*v : *v);
        return std::nullopt;
    }
    // ── min/max ────────────────────────────────────────────────────────────
    if (name == "min" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) return CV::fromInt(std::min(*a, *b));
        return std::nullopt;
    }
    if (name == "max" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) return CV::fromInt(std::max(*a, *b));
        return std::nullopt;
    }
    // ── sign ───────────────────────────────────────────────────────────────
    if (name == "sign" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt(*v > 0 ? 1 : (*v < 0 ? -1 : 0));
        return std::nullopt;
    }
    // ── clamp ──────────────────────────────────────────────────────────────
    if (name == "clamp" && n == 3) {
        auto v = intArg(0), lo = intArg(1), hi = intArg(2);
        if (v && lo && hi) return CV::fromInt(std::max(*lo, std::min(*v, *hi)));
        return std::nullopt;
    }
    // ── pow ────────────────────────────────────────────────────────────────
    if (name == "pow" && n == 2) {
        auto b = intArg(0), e = intArg(1);
        if (b && e) {
            int64_t base = *b, exp = *e;
            if (exp < 0) return (base == 1) ? std::optional<CV>(CV::fromInt(1)) : std::nullopt;
            int64_t r = 1;
            while (exp > 0) { if (exp & 1) r *= base; base *= base; exp >>= 1; }
            return CV::fromInt(r);
        }
        return std::nullopt;
    }
    // ── is_even / is_odd ───────────────────────────────────────────────────
    if (name == "is_even" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt((*v & 1) == 0 ? 1 : 0);
        return std::nullopt;
    }
    if (name == "is_odd" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt((*v & 1) ? 1 : 0);
        return std::nullopt;
    }
    // ── gcd ────────────────────────────────────────────────────────────────
    if (name == "gcd" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) {
            uint64_t ua = static_cast<uint64_t>(std::abs(*a));
            uint64_t ub = static_cast<uint64_t>(std::abs(*b));
            while (ub) { uint64_t t = ub; ub = ua % ub; ua = t; }
            return CV::fromInt(static_cast<int64_t>(ua));
        }
        return std::nullopt;
    }
    // ── lcm ────────────────────────────────────────────────────────────────
    if (name == "lcm" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) {
            const uint64_t ua = static_cast<uint64_t>(std::abs(*a));
            const uint64_t ub = static_cast<uint64_t>(std::abs(*b));
            if (ua == 0 || ub == 0) return CV::fromInt(0);
            uint64_t g = ua, tb = ub;
            while (tb) { const uint64_t t = tb; tb = g % tb; g = t; }
            return CV::fromInt(static_cast<int64_t>(ua / g * ub));
        }
        return std::nullopt;
    }
    // ── char_at ────────────────────────────────────────────────────────────
    if (name == "char_at" && n == 2) {
        auto s = strArg(0); auto i = intArg(1);
        if (s && i && *i >= 0 && *i < static_cast<int64_t>(s->size()))
            return CV::fromInt(static_cast<unsigned char>((*s)[static_cast<size_t>(*i)]));
        return std::nullopt;
    }
    // ── str_len ────────────────────────────────────────────────────────────
    if (name == "str_len" && n == 1) {
        if (auto s = strArg(0)) return CV::fromInt(static_cast<int64_t>(s->size()));
        return std::nullopt;
    }
    // ── str_eq ─────────────────────────────────────────────────────────────
    if (name == "str_eq" && n == 2) {
        auto a = strArg(0), b = strArg(1);
        if (a && b) return CV::fromInt(*a == *b ? 1 : 0);
        return std::nullopt;
    }
    // ── str_concat ─────────────────────────────────────────────────────────
    if (name == "str_concat" && n == 2) {
        auto a = strArg(0), b = strArg(1);
        if (a && b) return CV::fromStr(*a + *b);
        return std::nullopt;
    }
    // ── to_char(n) → single char string ────────────────────────────────────
    if (name == "to_char" && n == 1) {
        if (auto v = intArg(0)) {
            const char c = static_cast<char>(static_cast<uint8_t>(*v & 0xFF));
            return CV::fromStr(std::string(1, c));
        }
        return std::nullopt;
    }
    // ── is_alpha / is_digit ────────────────────────────────────────────────
    if (name == "is_alpha" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt((*v >= 0 && *v <= 127 && std::isalpha(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    if (name == "is_digit" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt((*v >= 0 && *v <= 127 && std::isdigit(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    // ── to_string / number_to_string ───────────────────────────────────────
    if ((name == "to_string" || name == "number_to_string") && n == 1) {
        if (auto v = intArg(0)) return CV::fromStr(std::to_string(*v));
        return std::nullopt;
    }
    // ── to_int / string_to_number ──────────────────────────────────────────
    if ((name == "to_int" || name == "string_to_number") && n == 1) {
        if (auto s = strArg(0)) {
            try { return CV::fromInt(static_cast<int64_t>(std::stoll(*s))); }
            catch (...) {} // NOLINT(bugprone-empty-catch)
        }
        return std::nullopt;
    }
    // ── char_code(s) → ASCII of first char ─────────────────────────────────
    if (name == "char_code" && n == 1) {
        if (auto s = strArg(0))
            if (!s->empty()) return CV::fromInt(static_cast<unsigned char>((*s)[0]));
        return std::nullopt;
    }
    // ── str_find(haystack, needle) ─────────────────────────────────────────
    if (name == "str_find" && n == 2) {
        auto hay = strArg(0), needle = strArg(1);
        if (hay && needle) {
            auto pos = hay->find(*needle);
            return CV::fromInt(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
        }
        return std::nullopt;
    }
    // ── str_index_of (alias for str_find) ──────────────────────────────────
    if (name == "str_index_of" && n == 2) {
        auto hay = strArg(0), needle = strArg(1);
        if (hay && needle) {
            auto pos = hay->find(*needle);
            return CV::fromInt(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
        }
        return std::nullopt;
    }
    // ── str_contains ───────────────────────────────────────────────────────
    if (name == "str_contains" && n == 2) {
        auto hay = strArg(0), needle = strArg(1);
        if (hay && needle) return CV::fromInt(hay->find(*needle) != std::string::npos ? 1 : 0);
        return std::nullopt;
    }
    // ── str_starts_with ────────────────────────────────────────────────────
    if (name == "str_starts_with" && n == 2) {
        auto s = strArg(0), prefix = strArg(1);
        if (s && prefix) {
            const bool result = s->size() >= prefix->size() &&
                          s->compare(0, prefix->size(), *prefix) == 0;
            return CV::fromInt(result ? 1 : 0);
        }
        return std::nullopt;
    }
    // ── str_ends_with ──────────────────────────────────────────────────────
    if (name == "str_ends_with" && n == 2) {
        auto s = strArg(0), suffix = strArg(1);
        if (s && suffix) {
            const bool result = s->size() >= suffix->size() &&
                          s->compare(s->size() - suffix->size(), suffix->size(), *suffix) == 0;
            return CV::fromInt(result ? 1 : 0);
        }
        return std::nullopt;
    }
    // ── str_substr(s, start, len) ──────────────────────────────────────────
    if (name == "str_substr" && n == 3) {
        auto s = strArg(0); auto start = intArg(1); auto slen = intArg(2);
        if (s && start && slen) {
            const int64_t sz = static_cast<int64_t>(s->size());
            const int64_t st = std::max(int64_t(0), std::min(*start, sz));
            const int64_t ln = std::max(int64_t(0), std::min(*slen, sz - st));
            return CV::fromStr(s->substr(static_cast<size_t>(st), static_cast<size_t>(ln)));
        }
        return std::nullopt;
    }
    // ── str_upper / str_lower ──────────────────────────────────────────────
    if (name == "str_upper" && n == 1) {
        if (auto s = strArg(0)) {
            std::string r = *s;
            for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return CV::fromStr(std::move(r));
        }
        return std::nullopt;
    }
    if (name == "str_lower" && n == 1) {
        if (auto s = strArg(0)) {
            std::string r = *s;
            for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return CV::fromStr(std::move(r));
        }
        return std::nullopt;
    }
    // ── str_repeat(s, n) ───────────────────────────────────────────────────
    if (name == "str_repeat" && n == 2) {
        auto s = strArg(0); auto cnt = intArg(1);
        if (s && cnt) {
            if (*cnt <= 0) return CV::fromStr("");
            if (*cnt > 1000) return std::nullopt;  // guard against huge strings
            std::string r;
            r.reserve(s->size() * static_cast<size_t>(*cnt));
            for (int64_t i = 0; i < *cnt; ++i) r += *s;
            return CV::fromStr(std::move(r));
        }
        return std::nullopt;
    }
    // ── str_trim(s) ────────────────────────────────────────────────────────
    if (name == "str_trim" && n == 1) {
        if (auto s = strArg(0)) {
            const std::string& sv = *s;
            size_t start = 0, end = sv.size();
            while (start < end && std::isspace(static_cast<unsigned char>(sv[start]))) ++start;
            while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) --end;
            return CV::fromStr(sv.substr(start, end - start));
        }
        return std::nullopt;
    }
    // ── str_reverse(s) ─────────────────────────────────────────────────────
    if (name == "str_reverse" && n == 1) {
        if (auto s = strArg(0)) {
            std::string r = *s;
            std::reverse(r.begin(), r.end());
            return CV::fromStr(std::move(r));
        }
        return std::nullopt;
    }
    // ── str_count(s, sub) ──────────────────────────────────────────────────
    if (name == "str_count" && n == 2) {
        auto s = strArg(0), sub = strArg(1);
        if (s && sub && !sub->empty()) {
            int64_t count = 0;
            size_t pos = 0;
            while ((pos = s->find(*sub, pos)) != std::string::npos) {
                ++count;
                pos += sub->size();
            }
            return CV::fromInt(count);
        }
        return std::nullopt;
    }
    // ── str_replace(s, old, new) ───────────────────────────────────────────
    if (name == "str_replace" && n == 3) {
        auto s = strArg(0), old_sub = strArg(1), new_sub = strArg(2);
        if (s && old_sub && new_sub) {
            if (old_sub->empty()) return CV::fromStr(*s);
            std::string r;
            r.reserve(s->size());
            size_t pos = 0, prev = 0;
            while ((pos = s->find(*old_sub, prev)) != std::string::npos) {
                r.append(*s, prev, pos - prev);
                r += *new_sub;
                prev = pos + old_sub->size();
            }
            r.append(*s, prev, std::string::npos);
            return CV::fromStr(std::move(r));
        }
        return std::nullopt;
    }
    // ── log2(n) ────────────────────────────────────────────────────────────
    if (name == "log2" && n == 1) {
        if (auto v = intArg(0)) {
            if (*v <= 0) return std::nullopt;
            int64_t x = *v, r = 0;
            while (x > 1) { x >>= 1; r++; }
            return CV::fromInt(r);
        }
        return std::nullopt;
    }
    // ── is_power_of_2 ──────────────────────────────────────────────────────
    if (name == "is_power_of_2" && n == 1) {
        if (auto v = intArg(0))
            return CV::fromInt((*v > 0 && (*v & (*v - 1)) == 0) ? 1 : 0);
        return std::nullopt;
    }
    // ── popcount ───────────────────────────────────────────────────────────
    if (name == "popcount" && n == 1) {
        if (auto v = intArg(0))
            return CV::fromInt(static_cast<int64_t>(
                __builtin_popcountll(static_cast<uint64_t>(*v))));
        return std::nullopt;
    }
    // ── clz (count leading zeros) ──────────────────────────────────────────
    if (name == "clz" && n == 1) {
        if (auto v = intArg(0))
            if (*v != 0) return CV::fromInt(static_cast<int64_t>(
                __builtin_clzll(static_cast<uint64_t>(*v))));
        return std::nullopt;
    }
    // ── ctz (count trailing zeros) ─────────────────────────────────────────
    if (name == "ctz" && n == 1) {
        if (auto v = intArg(0))
            if (*v != 0) return CV::fromInt(static_cast<int64_t>(
                __builtin_ctzll(static_cast<uint64_t>(*v))));
        return std::nullopt;
    }
    // ── bswap ──────────────────────────────────────────────────────────────
    if (name == "bswap" && n == 1) {
        if (auto v = intArg(0))
            return CV::fromInt(static_cast<int64_t>(
                __builtin_bswap64(static_cast<uint64_t>(*v))));
        return std::nullopt;
    }
    // ── bitreverse ─────────────────────────────────────────────────────────
    if (name == "bitreverse" && n == 1) {
        if (auto v = intArg(0)) {
            uint64_t x = static_cast<uint64_t>(*v);
            x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
            x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
            x = ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
            x = __builtin_bswap64(x);
            return CV::fromInt(static_cast<int64_t>(x));
        }
        return std::nullopt;
    }
    // ── rotate_left / rotate_right ─────────────────────────────────────────
    if (name == "rotate_left" && n == 2) {
        auto v = intArg(0), k = intArg(1);
        if (v && k) {
            const uint64_t x = static_cast<uint64_t>(*v);
            const int sh = static_cast<int>(*k) & 63;
            return CV::fromInt(static_cast<int64_t>((x << sh) | (x >> (64 - sh))));
        }
        return std::nullopt;
    }
    if (name == "rotate_right" && n == 2) {
        auto v = intArg(0), k = intArg(1);
        if (v && k) {
            const uint64_t x = static_cast<uint64_t>(*v);
            const int sh = static_cast<int>(*k) & 63;
            return CV::fromInt(static_cast<int64_t>((x >> sh) | (x << (64 - sh))));
        }
        return std::nullopt;
    }
    // ── saturating_add / saturating_sub ────────────────────────────────────
    if (name == "saturating_add" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) {
            int64_t r;
            if (__builtin_add_overflow(*a, *b, &r))
                r = (*a > 0) ? INT64_MAX : INT64_MIN;
            return CV::fromInt(r);
        }
        return std::nullopt;
    }
    if (name == "saturating_sub" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) {
            int64_t r;
            if (__builtin_sub_overflow(*a, *b, &r))
                r = (*a > 0) ? INT64_MAX : INT64_MIN;
            return CV::fromInt(r);
        }
        return std::nullopt;
    }
    // ── sqrt(x) for integer args ────────────────────────────────────────────
    if (name == "sqrt" && n == 1) {
        if (auto v = intArg(0)) {
            if (*v >= 0) return CV::fromInt(static_cast<int64_t>(std::sqrt(static_cast<double>(*v))));
        }
        return std::nullopt;
    }
    // ── floor(x), ceil(x), round(x) for integer args ───────────────────────
    if (name == "floor" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt(*v);  // floor of integer is itself
        return std::nullopt;
    }
    if (name == "ceil" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt(*v);  // ceil of integer is itself
        return std::nullopt;
    }
    if (name == "round" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt(*v);  // round of integer is itself
        return std::nullopt;
    }
    // ── log2(x) / log(x) / log10(x) integer floor ─────────────────────────
    if (name == "log" && n == 1) {
        if (auto v = intArg(0)) {
            if (*v > 0) return CV::fromInt(static_cast<int64_t>(std::log(static_cast<double>(*v))));
        }
        return std::nullopt;
    }
    if (name == "log10" && n == 1) {
        if (auto v = intArg(0)) {
            if (*v > 0) return CV::fromInt(static_cast<int64_t>(std::log10(static_cast<double>(*v))));
        }
        return std::nullopt;
    }
    // ── exp2(n) for small non-negative integer exponents ───────────────────
    if (name == "exp2" && n == 1) {
        if (auto v = intArg(0)) {
            if (*v >= 0 && *v < 63)
                return CV::fromInt(int64_t(1) << static_cast<int>(*v));
        }
        return std::nullopt;
    }
    // ── str_to_int(s) ──────────────────────────────────────────────────────
    if (name == "str_to_int" && n == 1) {
        if (auto s = strArg(0)) {
            try { return CV::fromInt(static_cast<int64_t>(std::stoll(*s))); }
            catch (...) {} // NOLINT(bugprone-empty-catch)
        }
        return std::nullopt;
    }
    // ── str_pad_left(s, width, fill) ───────────────────────────────────────
    if (name == "str_pad_left" && n == 3) {
        auto s = strArg(0); auto w = intArg(1); auto fill = strArg(2);
        if (s && w && fill && !fill->empty()) {
            const int64_t slen = static_cast<int64_t>(s->size());
            if (*w <= slen) return CV::fromStr(*s);
            if (*w > 65536) return std::nullopt; // guard against huge strings
            std::string r(*w - slen, (*fill)[0]);
            r += *s;
            return CV::fromStr(std::move(r));
        }
        return std::nullopt;
    }
    // ── str_pad_right(s, width, fill) ──────────────────────────────────────
    if (name == "str_pad_right" && n == 3) {
        auto s = strArg(0); auto w = intArg(1); auto fill = strArg(2);
        if (s && w && fill && !fill->empty()) {
            const int64_t slen = static_cast<int64_t>(s->size());
            if (*w <= slen) return CV::fromStr(*s);
            if (*w > 65536) return std::nullopt; // guard against huge strings
            std::string r(*s);
            r.append(*w - slen, (*fill)[0]);
            return CV::fromStr(std::move(r));
        }
        return std::nullopt;
    }
    // ── sum(arr) ───────────────────────────────────────────────────────────
    if (name == "sum" && n == 1) {
        if (args[0].kind == CV::Kind::Array) {
            int64_t total = 0;
            for (const auto& elem : args[0].arrVal) {
                if (elem.kind != CV::Kind::Integer) return std::nullopt;
                total += elem.intVal;
            }
            return CV::fromInt(total);
        }
        return std::nullopt;
    }
    // ── array_product(arr) ─────────────────────────────────────────────────
    if (name == "array_product" && n == 1) {
        if (args[0].kind == CV::Kind::Array) {
            int64_t product = 1;
            for (const auto& elem : args[0].arrVal) {
                if (elem.kind != CV::Kind::Integer) return std::nullopt;
                product *= elem.intVal;
            }
            return CV::fromInt(product);
        }
        return std::nullopt;
    }
    // ── array_last(arr) ────────────────────────────────────────────────────
    if (name == "array_last" && n == 1) {
        if (args[0].kind == CV::Kind::Array && !args[0].arrVal.empty())
            return args[0].arrVal.back();
        return std::nullopt;
    }
    // ── array_min(arr) ─────────────────────────────────────────────────────
    if (name == "array_min" && n == 1) {
        if (args[0].kind == CV::Kind::Array) {
            if (args[0].arrVal.empty()) return CV::fromInt(0);
            int64_t minVal = INT64_MAX;
            for (const auto& elem : args[0].arrVal) {
                if (elem.kind != CV::Kind::Integer) return std::nullopt;
                if (elem.intVal < minVal) minVal = elem.intVal;
            }
            return CV::fromInt(minVal);
        }
        return std::nullopt;
    }
    // ── array_max(arr) ─────────────────────────────────────────────────────
    if (name == "array_max" && n == 1) {
        if (args[0].kind == CV::Kind::Array) {
            if (args[0].arrVal.empty()) return CV::fromInt(0);
            int64_t maxVal = INT64_MIN;
            for (const auto& elem : args[0].arrVal) {
                if (elem.kind != CV::Kind::Integer) return std::nullopt;
                if (elem.intVal > maxVal) maxVal = elem.intVal;
            }
            return CV::fromInt(maxVal);
        }
        return std::nullopt;
    }
    // ── array_contains(arr, val) ───────────────────────────────────────────
    if (name == "array_contains" && n == 2) {
        if (args[0].kind == CV::Kind::Array && args[1].kind == CV::Kind::Integer) {
            for (const auto& elem : args[0].arrVal) {
                if (elem.kind == CV::Kind::Integer && elem.intVal == args[1].intVal)
                    return CV::fromInt(1);
            }
            return CV::fromInt(0);
        }
        return std::nullopt;
    }
    // ── index_of(arr, val) ────────────────────────────────────────────────
    if (name == "index_of" && n == 2) {
        if (args[0].kind == CV::Kind::Array && args[1].kind == CV::Kind::Integer) {
            for (int64_t i = 0; i < static_cast<int64_t>(args[0].arrVal.size()); ++i) {
                if (args[0].arrVal[static_cast<size_t>(i)].kind == CV::Kind::Integer &&
                    args[0].arrVal[static_cast<size_t>(i)].intVal == args[1].intVal)
                    return CV::fromInt(i);
            }
            return CV::fromInt(-1);
        }
        return std::nullopt;
    }
    // ── array_find(arr, val) (alias for index_of — returns 0-based index or -1) ──
    if (name == "array_find" && n == 2) {
        if (args[0].kind == CV::Kind::Array && args[1].kind == CV::Kind::Integer) {
            for (int64_t i = 0; i < static_cast<int64_t>(args[0].arrVal.size()); ++i) {
                if (args[0].arrVal[static_cast<size_t>(i)].kind == CV::Kind::Integer &&
                    args[0].arrVal[static_cast<size_t>(i)].intVal == args[1].intVal)
                    return CV::fromInt(i);
            }
            return CV::fromInt(-1);
        }
        return std::nullopt;
    }
    // ── startswith / endswith (OmScript aliases for str_starts_with / str_ends_with) ─
    if (name == "startswith" && n == 2) {
        auto s = strArg(0), prefix = strArg(1);
        if (s && prefix) {
            const bool result = s->size() >= prefix->size() &&
                          s->compare(0, prefix->size(), *prefix) == 0;
            return CV::fromInt(result ? 1 : 0);
        }
        return std::nullopt;
    }
    if (name == "endswith" && n == 2) {
        auto s = strArg(0), suffix = strArg(1);
        if (s && suffix) {
            const bool result = s->size() >= suffix->size() &&
                          s->compare(s->size() - suffix->size(), suffix->size(), *suffix) == 0;
            return CV::fromInt(result ? 1 : 0);
        }
        return std::nullopt;
    }
    // ── array_fill(n, val) → array with n copies of val ────────────────────
    if (name == "array_fill" && n == 2) {
        if (args[0].kind == CV::Kind::Integer) {
            int64_t count = args[0].intVal;
            if (count < 0) count = 0;
            if (count > 65536) return std::nullopt;  // guard against huge allocations
            std::vector<CV> arr(static_cast<size_t>(count), args[1]);
            return CV::fromArr(std::move(arr));
        }
        return std::nullopt;
    }
    // ── array_concat(arr1, arr2) ───────────────────────────────────────────
    if (name == "array_concat" && n == 2) {
        if (args[0].kind == CV::Kind::Array && args[1].kind == CV::Kind::Array) {
            std::vector<CV> result;
            result.reserve(args[0].arrVal.size() + args[1].arrVal.size());
            result.insert(result.end(), args[0].arrVal.begin(), args[0].arrVal.end());
            result.insert(result.end(), args[1].arrVal.begin(), args[1].arrVal.end());
            return CV::fromArr(std::move(result));
        }
        return std::nullopt;
    }
    // ── array_slice(arr, start, end) ───────────────────────────────────────
    if (name == "array_slice" && n == 3) {
        if (args[0].kind == CV::Kind::Array &&
            args[1].kind == CV::Kind::Integer &&
            args[2].kind == CV::Kind::Integer) {
            int64_t sz = static_cast<int64_t>(args[0].arrVal.size());
            int64_t st = std::max(int64_t(0), std::min(args[1].intVal, sz));
            int64_t en = std::max(st, std::min(args[2].intVal, sz));
            std::vector<CV> slice(
                args[0].arrVal.begin() + st,
                args[0].arrVal.begin() + en);
            return CV::fromArr(std::move(slice));
        }
        return std::nullopt;
    }
    // ── typeof(x) ─────────────────────────────────────────────────────────
    if (name == "typeof" && n == 1) {
        if (args[0].kind == CV::Kind::Integer) return CV::fromInt(1);   // integer
        if (args[0].kind == CV::Kind::String)  return CV::fromInt(3);   // string
        return CV::fromInt(1);  // arrays encoded as i64 at runtime → type 1
    }
    // ── str_chars(s) → array of char codes ────────────────────────────────
    if (name == "str_chars" && n == 1) {
        if (auto s = strArg(0)) {
            std::vector<CV> arr;
            arr.reserve(s->size());
            for (unsigned char c : *s)
                arr.push_back(CV::fromInt(static_cast<int64_t>(c)));
            return CV::fromArr(std::move(arr));
        }
        return std::nullopt;
    }
    // ── Character classification predicates ─────────────────────────────
    // C <cctype> functions require the argument to be in [0, UCHAR_MAX] or EOF.
    // OmScript passes character code points as int64_t; out-of-range → false.
    if (name == "is_upper" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt((*v >= 0 && *v <= 127 && std::isupper(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    if (name == "is_lower" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt((*v >= 0 && *v <= 127 && std::islower(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    if (name == "is_space" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt((*v >= 0 && *v <= 127 && std::isspace(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    if (name == "is_alnum" && n == 1) {
        if (auto v = intArg(0)) return CV::fromInt((*v >= 0 && *v <= 127 && std::isalnum(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    // ── Unchecked integer arithmetic (fast_* / precise_*) ────────────────
    if (name == "fast_add" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(*a + *b); return std::nullopt; }
    if (name == "fast_sub" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(*a - *b); return std::nullopt; }
    if (name == "fast_mul" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(*a * *b); return std::nullopt; }
    if (name == "fast_div" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b && *b != 0) return CV::fromInt(*a / *b); return std::nullopt; }
    if (name == "precise_add" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(*a + *b); return std::nullopt; }
    if (name == "precise_sub" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(*a - *b); return std::nullopt; }
    if (name == "precise_mul" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(*a * *b); return std::nullopt; }
    if (name == "precise_div" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b && *b != 0) return CV::fromInt(*a / *b); return std::nullopt; }
    // ── Trigonometric / math (integer floor) ─────────────────────────────
    if (name == "sin" && n == 1) { if (auto v = intArg(0)) return CV::fromInt(static_cast<int64_t>(std::sin(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "cos" && n == 1) { if (auto v = intArg(0)) return CV::fromInt(static_cast<int64_t>(std::cos(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "tan" && n == 1) { if (auto v = intArg(0)) return CV::fromInt(static_cast<int64_t>(std::tan(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "asin" && n == 1) { if (auto v = intArg(0)) return CV::fromInt(static_cast<int64_t>(std::asin(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "acos" && n == 1) { if (auto v = intArg(0)) return CV::fromInt(static_cast<int64_t>(std::acos(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "atan" && n == 1) { if (auto v = intArg(0)) return CV::fromInt(static_cast<int64_t>(std::atan(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "atan2" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(static_cast<int64_t>(std::atan2(static_cast<double>(*a), static_cast<double>(*b)))); return std::nullopt; }
    if (name == "cbrt" && n == 1) { if (auto v = intArg(0)) return CV::fromInt(static_cast<int64_t>(std::cbrt(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "hypot" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(static_cast<int64_t>(std::hypot(static_cast<double>(*a), static_cast<double>(*b)))); return std::nullopt; }
    if (name == "fma" && n == 3) { auto a = intArg(0), b = intArg(1), c = intArg(2); if (a && b && c) return CV::fromInt(*a * *b + *c); return std::nullopt; }
    if (name == "copysign" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) { int64_t mag = *a < 0 ? -*a : *a; return CV::fromInt(*b >= 0 ? mag : -mag); } return std::nullopt; }
    if (name == "min_float" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(std::min(*a, *b)); return std::nullopt; }
    if (name == "max_float" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CV::fromInt(std::max(*a, *b)); return std::nullopt; }
    // ── Array: reverse, sort, remove, insert ─────────────────────────────
    if (name == "reverse" && n == 1) {
        if (args[0].kind == CV::Kind::Array) {
            auto arr = args[0].arrVal;
            std::reverse(arr.begin(), arr.end());
            return CV::fromArr(std::move(arr));
        }
        return std::nullopt;
    }
    if (name == "sort" && n == 1) {
        if (args[0].kind == CV::Kind::Array) {
            auto arr = args[0].arrVal;
            for (auto& e : arr) if (e.kind != CV::Kind::Integer) return std::nullopt;
            std::sort(arr.begin(), arr.end(), [](const CV& a, const CV& b) { return a.intVal < b.intVal; });
            return CV::fromArr(std::move(arr));
        }
        return std::nullopt;
    }
    if (name == "array_remove" && n == 2) {
        if (args[0].kind == CV::Kind::Array && args[1].kind == CV::Kind::Integer) {
            auto arr = args[0].arrVal;
            int64_t idx = args[1].intVal;
            if (idx < 0 || idx >= static_cast<int64_t>(arr.size())) return std::nullopt;
            arr.erase(arr.begin() + idx);
            return CV::fromArr(std::move(arr));
        }
        return std::nullopt;
    }
    if (name == "array_insert" && n == 3) {
        if (args[0].kind == CV::Kind::Array && args[1].kind == CV::Kind::Integer) {
            auto arr = args[0].arrVal;
            int64_t idx = args[1].intVal;
            if (idx < 0 || idx > static_cast<int64_t>(arr.size())) return std::nullopt;
            arr.insert(arr.begin() + idx, args[2]);
            return CV::fromArr(std::move(arr));
        }
        return std::nullopt;
    }
    if (name == "array_any" && n == 1) {
        if (args[0].kind == CV::Kind::Array) {
            for (auto& e : args[0].arrVal)
                if (e.kind == CV::Kind::Integer && e.intVal != 0) return CV::fromInt(1);
            return CV::fromInt(0);
        }
        return std::nullopt;
    }
    if (name == "array_every" && n == 1) {
        if (args[0].kind == CV::Kind::Array) {
            for (auto& e : args[0].arrVal) {
                if (e.kind != CV::Kind::Integer) return std::nullopt;
                if (e.intVal == 0) return CV::fromInt(0);
            }
            return CV::fromInt(1);
        }
        return std::nullopt;
    }
    if (name == "array_count" && n == 2) {
        if (args[0].kind == CV::Kind::Array && args[1].kind == CV::Kind::Integer) {
            int64_t count = 0;
            for (auto& e : args[0].arrVal)
                if (e.kind == CV::Kind::Integer && e.intVal == args[1].intVal) ++count;
            return CV::fromInt(count);
        }
        return std::nullopt;
    }
    // ── String: str_split (returns array of strings), str_join ───────────
    // str_split not easily representable in ConstValue (array of strings)
    // str_join requires array of strings — skip for ConstValue evaluator.
    // ── General iN/uN constant cast (for comptime evaluation) ─────────────────
    // u64(x), i64(x), int(x), uint(x) — identity; all OmScript integers are i64.
    // u32(x), i32(x) — truncate to 32 bits (zero/sign extend back to i64).
    // u16(x), i16(x) — truncate to 16 bits.
    // u8(x),  i8(x)  — truncate to 8 bits.
    // bool(x)        — normalise to 0 or 1.
    // Arbitrary uN(x)/iN(x) for N in [1..256] — mask/sign-extend as appropriate.
    // For N > 64, ConstValue only holds i64, so upper bits are truncated.
    {
        const std::string& nm = name;
        unsigned castBits = 0; bool castUnsigned = false;
        if (nm == "int")  { castBits = 64; castUnsigned = false; }
        else if (nm == "uint") { castBits = 64; castUnsigned = true; }
        else if (nm == "bool") { castBits = 1;  castUnsigned = true; }
        else if (nm.size() >= 2 && (nm[0] == 'i' || nm[0] == 'u')) {
            bool allDigits = true; int bw = 0;
            for (size_t j = 1; j < nm.size(); ++j) {
                if (!std::isdigit(static_cast<unsigned char>(nm[j]))) { allDigits = false; break; }
                bw = bw * 10 + (nm[j] - '0'); if (bw > 256) { allDigits = false; break; }
            }
            if (allDigits && bw >= 1 && bw <= 256) { castBits = static_cast<unsigned>(bw); castUnsigned = (nm[0] == 'u'); }
        }
        if (castBits >= 1 && n == 1 && args[0].kind == CV::Kind::Integer) {
            const int64_t v = args[0].intVal;
            if (castBits == 1) return CV::fromInt(v != 0 ? 1 : 0);
            if (castBits >= 64) return CV::fromInt(v); // identity for i64/u64/wider
            const uint64_t mask = (UINT64_C(1) << castBits) - 1u;
            if (castUnsigned) return CV::fromInt(static_cast<int64_t>(static_cast<uint64_t>(v) & mask));
            // Signed: mask then sign-extend
            uint64_t uv = static_cast<uint64_t>(v) & mask;
            const uint64_t signBit = UINT64_C(1) << (castBits - 1);
            if (uv & signBit) uv |= ~mask; // sign-extend
            return CV::fromInt(static_cast<int64_t>(uv));
        }
    }
    return std::nullopt;
}
// using all statically-known information (constIntFolds_, constStringFolds_,
// constIntReturnFunctions_, constStringReturnFunctions_, enumConstants_).
// This is the "call-site evaluator" — it knows about constants in the CURRENT
// function's scope and delegates to tryConstEvalFull for nested calls.
//
// tryConstEvalFull: evaluate a function body with a known argument environment.
// This is the "body evaluator" — it keeps its own local int+string+array env
// seeded from the caller-provided argEnv, and handles all common statement/
// expression forms. Returns nullopt if any step requires runtime information.
// ─────────────────────────────────────────────────────────────────────────────

std::optional<CodeGenerator::ConstValue>
CodeGenerator::tryFoldExprToConst(Expression* expr, int depth) const {
    if (!expr || depth > 64) return std::nullopt;
    switch (expr->type) {
    case ASTNodeType::LITERAL_EXPR: {
        auto* lit = static_cast<LiteralExpr*>(expr);
        if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
            return ConstValue::fromInt(static_cast<int64_t>(lit->intValue));
        if (lit->literalType == LiteralExpr::LiteralType::STRING)
            return ConstValue::fromStr(lit->stringValue);
        return std::nullopt;
    }
    case ASTNodeType::IDENTIFIER_EXPR: {
        auto* id = static_cast<IdentifierExpr*>(expr);
        auto iit = constIntFolds_.find(id->name);
        if (iit != constIntFolds_.end()) return ConstValue::fromInt(iit->second);
        auto sit = constStringFolds_.find(id->name);
        if (sit != constStringFolds_.end()) return ConstValue::fromStr(sit->second);
        auto ait = constArrayFolds_.find(id->name);
        if (ait != constArrayFolds_.end()) return ConstValue::fromArr(ait->second);
        auto eit = enumConstants_.find(id->name);
        if (eit != enumConstants_.end())
            return ConstValue::fromInt(static_cast<int64_t>(eit->second));
        return std::nullopt;
    }
    case ASTNodeType::ARRAY_EXPR: {
        auto* ae = static_cast<ArrayExpr*>(expr);
        std::vector<ConstValue> elems;
        elems.reserve(ae->elements.size());
        for (auto& el : ae->elements) {
            auto v = tryFoldExprToConst(el.get(), depth + 1);
            if (!v) return std::nullopt;
            elems.push_back(std::move(*v));
        }
        return ConstValue::fromArr(std::move(elems));
    }
    case ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<IndexExpr*>(expr);
        auto arrVal = tryFoldExprToConst(idx->array.get(), depth + 1);
        auto idxVal = tryFoldExprToConst(idx->index.get(), depth + 1);
        if (!arrVal || !idxVal || idxVal->kind != ConstValue::Kind::Integer)
            return std::nullopt;
        int64_t i = idxVal->intVal;
        if (arrVal->kind == ConstValue::Kind::String) {
            const std::string& s = arrVal->strVal;
            if (i < 0 || i >= static_cast<int64_t>(s.size())) return std::nullopt;
            return ConstValue::fromInt(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
        }
        if (arrVal->kind == ConstValue::Kind::Array) {
            if (i < 0 || i >= static_cast<int64_t>(arrVal->arrVal.size())) return std::nullopt;
            return arrVal->arrVal[static_cast<size_t>(i)];
        }
        return std::nullopt;
    }
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<BinaryExpr*>(expr);
        if (bin->op == "&&") {
            auto lv = tryFoldExprToConst(bin->left.get(), depth + 1);
            if (!lv || lv->kind != ConstValue::Kind::Integer) return std::nullopt;
            if (lv->intVal == 0) return ConstValue::fromInt(0);
            auto rv = tryFoldExprToConst(bin->right.get(), depth + 1);
            if (!rv || rv->kind != ConstValue::Kind::Integer) return std::nullopt;
            return ConstValue::fromInt(rv->intVal != 0 ? 1 : 0);
        }
        if (bin->op == "||") {
            auto lv = tryFoldExprToConst(bin->left.get(), depth + 1);
            if (!lv || lv->kind != ConstValue::Kind::Integer) return std::nullopt;
            if (lv->intVal != 0) return ConstValue::fromInt(1);
            auto rv = tryFoldExprToConst(bin->right.get(), depth + 1);
            if (!rv || rv->kind != ConstValue::Kind::Integer) return std::nullopt;
            return ConstValue::fromInt(rv->intVal != 0 ? 1 : 0);
        }
        // Null-coalescing (??)
        if (bin->op == "??") {
            auto lv = tryFoldExprToConst(bin->left.get(), depth + 1);
            if (lv && lv->kind == ConstValue::Kind::Integer && lv->intVal != 0) return lv;
            return tryFoldExprToConst(bin->right.get(), depth + 1);
        }
        auto lv = tryFoldExprToConst(bin->left.get(), depth + 1);
        auto rv = tryFoldExprToConst(bin->right.get(), depth + 1);
        if (!lv || !rv) return std::nullopt;
        // String concatenation
        if (bin->op == "+" && lv->kind == ConstValue::Kind::String &&
            rv->kind == ConstValue::Kind::String)
            return ConstValue::fromStr(lv->strVal + rv->strVal);
        // Array concatenation
        if (bin->op == "+" && lv->kind == ConstValue::Kind::Array &&
            rv->kind == ConstValue::Kind::Array) {
            std::vector<ConstValue> out;
            out.reserve(lv->arrVal.size() + rv->arrVal.size());
            out.insert(out.end(), lv->arrVal.begin(), lv->arrVal.end());
            out.insert(out.end(), rv->arrVal.begin(), rv->arrVal.end());
            return ConstValue::fromArr(std::move(out));
        }
        // String == / != comparison
        if ((bin->op == "==" || bin->op == "!=") &&
            lv->kind == ConstValue::Kind::String &&
            rv->kind == ConstValue::Kind::String) {
            bool eq = (lv->strVal == rv->strVal);
            return ConstValue::fromInt(bin->op == "==" ? (eq ? 1 : 0) : (eq ? 0 : 1));
        }
        // Integer arithmetic / comparisons
        if (lv->kind != ConstValue::Kind::Integer ||
            rv->kind != ConstValue::Kind::Integer)
            return std::nullopt;
        int64_t a = lv->intVal, b = rv->intVal;
        // Use wrapping arithmetic for +, -, * so compile-time evaluation
        // matches the i64 two's-complement wrapping semantics at runtime.
        if (bin->op == "+")  return ConstValue::fromInt(static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b)));
        if (bin->op == "-")  return ConstValue::fromInt(static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b)));
        if (bin->op == "*")  return ConstValue::fromInt(static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b)));
        // Division/modulo: guard against division-by-zero AND the i64 overflow
        // case INT64_MIN / -1 (which traps on x86 IDIV).  Return nullopt for
        // both so the compiler falls back to runtime evaluation.
        if (bin->op == "/" && b != 0) {
            if (a == std::numeric_limits<int64_t>::min() && b == -1) return std::nullopt;
            return ConstValue::fromInt(a / b);
        }
        if (bin->op == "%" && b != 0) {
            if (a == std::numeric_limits<int64_t>::min() && b == -1) return ConstValue::fromInt(0);
            return ConstValue::fromInt(a % b);
        }
        if (bin->op == "&")  return ConstValue::fromInt(a & b);
        if (bin->op == "|")  return ConstValue::fromInt(a | b);
        if (bin->op == "^")  return ConstValue::fromInt(a ^ b);
        if (bin->op == "<<" && b >= 0 && b < 64) return ConstValue::fromInt(a << b);
        if (bin->op == ">>" && b >= 0 && b < 64) return ConstValue::fromInt(a >> b);
        if (bin->op == ">>>") {
            int sh = static_cast<int>(b & 63);
            return ConstValue::fromInt(static_cast<int64_t>(static_cast<uint64_t>(a) >> sh));
        }
        if (bin->op == "**") {
            if (b < 0) return (a == 1) ? std::optional<ConstValue>(ConstValue::fromInt(1)) : std::nullopt;
            int64_t r = 1; int64_t cur = a; int64_t rem = b;
            while (rem > 0) { if (rem & 1) r *= cur; cur *= cur; rem >>= 1; }
            return ConstValue::fromInt(r);
        }
        if (bin->op == "==") return ConstValue::fromInt(int64_t(a == b));
        if (bin->op == "!=") return ConstValue::fromInt(int64_t(a != b));
        if (bin->op == "<")  return ConstValue::fromInt(int64_t(a < b));
        if (bin->op == "<=") return ConstValue::fromInt(int64_t(a <= b));
        if (bin->op == ">")  return ConstValue::fromInt(int64_t(a > b));
        if (bin->op == ">=") return ConstValue::fromInt(int64_t(a >= b));
        return std::nullopt;
    }
    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<UnaryExpr*>(expr);
        auto v = tryFoldExprToConst(un->operand.get(), depth + 1);
        if (!v || v->kind != ConstValue::Kind::Integer) return std::nullopt;
        if (un->op == "-") return ConstValue::fromInt(-v->intVal);
        if (un->op == "~") return ConstValue::fromInt(~v->intVal);
        if (un->op == "!") return ConstValue::fromInt(v->intVal == 0 ? 1 : 0);
        return std::nullopt;
    }
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<TernaryExpr*>(expr);
        auto cond = tryFoldExprToConst(tern->condition.get(), depth + 1);
        if (!cond || cond->kind != ConstValue::Kind::Integer) return std::nullopt;
        return cond->intVal != 0
                   ? tryFoldExprToConst(tern->thenExpr.get(), depth + 1)
                   : tryFoldExprToConst(tern->elseExpr.get(), depth + 1);
    }
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<CallExpr*>(expr);
        // Zero-arg: pre-classified constant-returning functions.
        // Prefer the unified OptimizationContext; fall back to private maps
        // when optCtx_ is not yet available (called during analysis passes).
        if (call->arguments.empty()) {
            if (optCtx_) {
                if (auto v = optCtx_->constIntReturn(call->callee))
                    return ConstValue::fromInt(*v);
                if (auto v = optCtx_->constStringReturn(call->callee))
                    return ConstValue::fromStr(*v);
            } else {
                auto iit = constIntReturnFunctions_.find(call->callee);
                if (iit != constIntReturnFunctions_.end())
                    return ConstValue::fromInt(iit->second);
                auto sit = constStringReturnFunctions_.find(call->callee);
                if (sit != constStringReturnFunctions_.end())
                    return ConstValue::fromStr(sit->second);
            }
        }
        // Try to fold all args to constants.
        std::vector<ConstValue> foldedArgs;
        bool allConst = true;
        foldedArgs.reserve(call->arguments.size());
        for (auto& arg : call->arguments) {
            auto av = tryFoldExprToConst(arg.get(), depth + 1);
            if (!av) { allConst = false; break; }
            foldedArgs.push_back(std::move(*av));
        }
        if (allConst) {
            // Try built-in pure functions first.
            if (auto bv = evalConstBuiltin(call->callee, foldedArgs))
                return bv;
            // Then try user-defined functions via body evaluation.
            auto declIt = functionDecls_.find(call->callee);
            if (declIt != functionDecls_.end() && declIt->second->body &&
                call->arguments.size() == declIt->second->parameters.size()) {
                std::unordered_map<std::string, ConstValue> callArgEnv;
                for (size_t i = 0; i < foldedArgs.size(); ++i)
                    callArgEnv[declIt->second->parameters[i].name] = foldedArgs[i];
                return tryConstEvalFull(declIt->second, callArgEnv, depth + 1);
            }
        }
        return std::nullopt;
    }
    case ASTNodeType::SCOPE_RESOLUTION_EXPR: {
        auto* sr = static_cast<ScopeResolutionExpr*>(expr);
        auto eit = enumConstants_.find(sr->scopeName + "_" + sr->memberName);
        if (eit != enumConstants_.end())
            return ConstValue::fromInt(static_cast<int64_t>(eit->second));
        return std::nullopt;
    }
    case ASTNodeType::COMPTIME_EXPR: {
        // A comptime {} block is always a compile-time constant — evaluate it.
        auto* ct = static_cast<ComptimeExpr*>(expr);
        static const std::unordered_map<std::string, ConstValue> emptyEnv;
        return tryConstEvalFull(ct->body.get(), emptyEnv);
    }
    case ASTNodeType::PIPE_EXPR: {
        auto* pipe = static_cast<PipeExpr*>(expr);
        auto lv = tryFoldExprToConst(pipe->left.get(), depth + 1);
        if (!lv) return std::nullopt;
        std::vector<ConstValue> args = {std::move(*lv)};
        if (auto bv = evalConstBuiltin(pipe->functionName, args))
            return bv;
        auto declIt = functionDecls_.find(pipe->functionName);
        if (declIt != functionDecls_.end() && declIt->second->body &&
            declIt->second->parameters.size() == 1) {
            std::unordered_map<std::string, ConstValue> callEnv;
            callEnv[declIt->second->parameters[0].name] = args[0];
            return tryConstEvalFull(declIt->second, callEnv, depth + 1);
        }
        return std::nullopt;
    }
    default: return std::nullopt;
    }
}

// tryFoldInt / tryFoldStr: convenience wrappers used by generateBuiltin.
// Evaluate an expression to an integer or string constant using all
// currently-available compile-time information.
std::optional<int64_t> CodeGenerator::tryFoldInt(Expression* e) const {
    if (!e) return std::nullopt;
    if (auto cv = tryFoldExprToConst(e))
        if (cv->kind == ConstValue::Kind::Integer) return cv->intVal;
    return std::nullopt;
}
std::optional<std::string> CodeGenerator::tryFoldStr(Expression* e) const {
    if (!e) return std::nullopt;
    if (auto cv = tryFoldExprToConst(e))
        if (cv->kind == ConstValue::Kind::String) return cv->strVal;
    return std::nullopt;
}

// tryConstEvalFull: Aggressively evaluate a function body at compile time.
// Handles the full statement/expression repertoire needed to constant-fold
// non-trivial pure functions (hash functions, lookup tables, etc.):
//   Expressions: literals, identifiers, binary/unary/ternary ops, string
//     indexing (s[i] → ASCII int), postfix/prefix ++/--, function calls (both
//     builtins and recursive user functions), enum/scope-resolution.
//   Statements: return, var decl (const + non-const), assignment, if/else,
//     for-range loops, while, do-while, foreach over strings, switch, break,
//     continue, blocks.
//   Fuel limit: each loop iteration counts against a per-call fuel counter
//   (kFuelLimit = 10 000); if exceeded the function returns nullopt so the
//   compiler falls back to runtime codegen.
std::optional<CodeGenerator::ConstValue>
CodeGenerator::tryConstEvalFull(
    const FunctionDecl* func,
    const std::unordered_map<std::string, ConstValue>& argEnv,
    int depth) const {

    if (!func || !func->body || depth > 48) return std::nullopt;

    // Compile-time fuel: counts loop iterations across the whole evaluation
    // to prevent runaway execution of large or infinite loops.
    static constexpr int64_t kFuelLimit = 10000;
    int64_t fuel = 0;

    std::unordered_map<std::string, ConstValue> env(argEnv);
    std::optional<ConstValue> retVal;
    // Last bare expression value — used to implicitly return the result of a
    // final expression statement when there is no explicit `return` keyword.
    // This enables `comptime { str_to_u64_fast("hello"); }` without `return`.
    std::optional<ConstValue> lastBareExprVal;
    // Signals set by break/continue statements, cleared by loop handlers.
    bool breakSeen    = false;
    bool continueSeen = false;

    std::function<std::optional<ConstValue>(Expression*)> evalE;
    std::function<bool(Statement*)>                        evalS;

    evalE = [&](Expression* e) -> std::optional<ConstValue> {
        if (!e) return std::nullopt;

        switch (e->type) {

        // ── Literals ─────────────────────────────────────────────────────
        case ASTNodeType::LITERAL_EXPR: {
            auto* lit = static_cast<LiteralExpr*>(e);
            if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
                return ConstValue::fromInt(static_cast<int64_t>(lit->intValue));
            if (lit->literalType == LiteralExpr::LiteralType::STRING)
                return ConstValue::fromStr(lit->stringValue);
            return std::nullopt;
        }

        // ── Identifiers ──────────────────────────────────────────────────
        case ASTNodeType::IDENTIFIER_EXPR: {
            auto* id = static_cast<IdentifierExpr*>(e);
            auto it = env.find(id->name);
            if (it != env.end()) return it->second;
            auto eit = enumConstants_.find(id->name);
            if (eit != enumConstants_.end())
                return ConstValue::fromInt(static_cast<int64_t>(eit->second));
            // Prefer the unified OptimizationContext; fall back to private maps
            // when optCtx_ is not yet available (called during analysis passes).
            if (optCtx_) {
                if (auto v = optCtx_->constIntReturn(id->name))
                    return ConstValue::fromInt(*v);
                if (auto v = optCtx_->constStringReturn(id->name))
                    return ConstValue::fromStr(*v);
            } else {
                auto iit = constIntReturnFunctions_.find(id->name);
                if (iit != constIntReturnFunctions_.end()) return ConstValue::fromInt(iit->second);
                auto sit = constStringReturnFunctions_.find(id->name);
                if (sit != constStringReturnFunctions_.end()) return ConstValue::fromStr(sit->second);
            }
            // Global const variables (constIntFolds_ / constStringFolds_):
            // functions may reference file-level constants not in their argEnv.
            auto git = constIntFolds_.find(id->name);
            if (git != constIntFolds_.end()) return ConstValue::fromInt(git->second);
            auto gst = constStringFolds_.find(id->name);
            if (gst != constStringFolds_.end()) return ConstValue::fromStr(gst->second);
            auto gat = constArrayFolds_.find(id->name);
            if (gat != constArrayFolds_.end()) return ConstValue::fromArr(gat->second);
            return std::nullopt;
        }

        // ── Binary ops ───────────────────────────────────────────────────
        case ASTNodeType::BINARY_EXPR: {
            auto* bin = static_cast<BinaryExpr*>(e);
            // Short-circuit logical ops
            if (bin->op == "&&") {
                auto lv = evalE(bin->left.get());
                if (!lv || lv->kind != ConstValue::Kind::Integer) return std::nullopt;
                if (lv->intVal == 0) return ConstValue::fromInt(0);
                auto rv = evalE(bin->right.get());
                if (!rv || rv->kind != ConstValue::Kind::Integer) return std::nullopt;
                return ConstValue::fromInt(rv->intVal != 0 ? 1 : 0);
            }
            if (bin->op == "||") {
                auto lv = evalE(bin->left.get());
                if (!lv || lv->kind != ConstValue::Kind::Integer) return std::nullopt;
                if (lv->intVal != 0) return ConstValue::fromInt(1);
                auto rv = evalE(bin->right.get());
                if (!rv || rv->kind != ConstValue::Kind::Integer) return std::nullopt;
                return ConstValue::fromInt(rv->intVal != 0 ? 1 : 0);
            }
            // Null-coalescing (??)
            if (bin->op == "??") {
                auto lv = evalE(bin->left.get());
                if (lv && lv->kind == ConstValue::Kind::Integer && lv->intVal != 0) return lv;
                return evalE(bin->right.get());
            }
            auto lv = evalE(bin->left.get());
            auto rv = evalE(bin->right.get());
            if (!lv || !rv) return std::nullopt;
            // String concat
            if (bin->op == "+" && lv->kind == ConstValue::Kind::String &&
                rv->kind == ConstValue::Kind::String)
                return ConstValue::fromStr(lv->strVal + rv->strVal);
            // Array concat
            if (bin->op == "+" && lv->kind == ConstValue::Kind::Array &&
                rv->kind == ConstValue::Kind::Array) {
                std::vector<ConstValue> out;
                out.reserve(lv->arrVal.size() + rv->arrVal.size());
                out.insert(out.end(), lv->arrVal.begin(), lv->arrVal.end());
                out.insert(out.end(), rv->arrVal.begin(), rv->arrVal.end());
                return ConstValue::fromArr(std::move(out));
            }
            // String == / !=
            if ((bin->op == "==" || bin->op == "!=") &&
                lv->kind == ConstValue::Kind::String &&
                rv->kind == ConstValue::Kind::String) {
                bool eq = (lv->strVal == rv->strVal);
                return ConstValue::fromInt(bin->op == "==" ? (eq ? 1 : 0) : (eq ? 0 : 1));
            }
            if (lv->kind != ConstValue::Kind::Integer || rv->kind != ConstValue::Kind::Integer)
                return std::nullopt;
            int64_t a = lv->intVal, b = rv->intVal;
            if (bin->op == "+")  return ConstValue::fromInt(a + b);
            if (bin->op == "-")  return ConstValue::fromInt(a - b);
            if (bin->op == "*")  return ConstValue::fromInt(a * b);
            if (bin->op == "/" && b != 0) return ConstValue::fromInt(a / b);
            if (bin->op == "%" && b != 0) return ConstValue::fromInt(a % b);
            if (bin->op == "&")  return ConstValue::fromInt(a & b);
            if (bin->op == "|")  return ConstValue::fromInt(a | b);
            if (bin->op == "^")  return ConstValue::fromInt(a ^ b);
            if (bin->op == "<<" && b >= 0 && b < 64) return ConstValue::fromInt(a << b);
            if (bin->op == ">>" && b >= 0 && b < 64) return ConstValue::fromInt(a >> b);
            if (bin->op == ">>>") {
                int sh = static_cast<int>(b & 63);
                return ConstValue::fromInt(static_cast<int64_t>(static_cast<uint64_t>(a) >> sh));
            }
            if (bin->op == "**") {
                if (b < 0) return (a == 1) ? std::optional<ConstValue>(ConstValue::fromInt(1)) : std::nullopt;
                if (b > 63) return std::nullopt; // cap to prevent signed overflow in squaring loop
                int64_t r = 1, base = a, rem = b;
                while (rem > 0) { if (rem & 1) r *= base; base *= base; rem >>= 1; }
                return ConstValue::fromInt(r);
            }
            if (bin->op == "==") return ConstValue::fromInt(int64_t(a == b));
            if (bin->op == "!=") return ConstValue::fromInt(int64_t(a != b));
            if (bin->op == "<")  return ConstValue::fromInt(int64_t(a < b));
            if (bin->op == "<=") return ConstValue::fromInt(int64_t(a <= b));
            if (bin->op == ">")  return ConstValue::fromInt(int64_t(a > b));
            if (bin->op == ">=") return ConstValue::fromInt(int64_t(a >= b));
            return std::nullopt;
        }

        // ── Unary ops ────────────────────────────────────────────────────
        case ASTNodeType::UNARY_EXPR: {
            auto* un = static_cast<UnaryExpr*>(e);
            auto v = evalE(un->operand.get());
            if (!v || v->kind != ConstValue::Kind::Integer) return std::nullopt;
            if (un->op == "-") return ConstValue::fromInt(-v->intVal);
            if (un->op == "~") return ConstValue::fromInt(~v->intVal);
            if (un->op == "!") return ConstValue::fromInt(v->intVal == 0 ? 1 : 0);
            return std::nullopt;
        }

        // ── Postfix ++ / -- ──────────────────────────────────────────────
        case ASTNodeType::POSTFIX_EXPR: {
            auto* pfx = static_cast<PostfixExpr*>(e);
            if (pfx->operand->type != ASTNodeType::IDENTIFIER_EXPR) return std::nullopt;
            auto* id = static_cast<IdentifierExpr*>(pfx->operand.get());
            auto it = env.find(id->name);
            if (it == env.end() || it->second.kind != ConstValue::Kind::Integer)
                return std::nullopt;
            int64_t old = it->second.intVal;
            it->second.intVal += (pfx->op == "++" ? 1 : -1);
            return ConstValue::fromInt(old);  // postfix returns old value
        }

        // ── Prefix ++ / -- ───────────────────────────────────────────────
        case ASTNodeType::PREFIX_EXPR: {
            auto* pfx = static_cast<PrefixExpr*>(e);
            if (pfx->op != "++" && pfx->op != "--") return std::nullopt;
            if (pfx->operand->type != ASTNodeType::IDENTIFIER_EXPR) return std::nullopt;
            auto* id = static_cast<IdentifierExpr*>(pfx->operand.get());
            auto it = env.find(id->name);
            if (it == env.end() || it->second.kind != ConstValue::Kind::Integer)
                return std::nullopt;
            it->second.intVal += (pfx->op == "++" ? 1 : -1);
            return it->second;  // prefix returns new value
        }

        // ── Ternary ──────────────────────────────────────────────────────
        case ASTNodeType::TERNARY_EXPR: {
            auto* tern = static_cast<TernaryExpr*>(e);
            auto cond = evalE(tern->condition.get());
            if (!cond || cond->kind != ConstValue::Kind::Integer) return std::nullopt;
            return cond->intVal != 0 ? evalE(tern->thenExpr.get())
                                     : evalE(tern->elseExpr.get());
        }

        // ── Array literal ─────────────────────────────────────────────────
        case ASTNodeType::ARRAY_EXPR: {
            auto* ae = static_cast<ArrayExpr*>(e);
            std::vector<ConstValue> elems;
            elems.reserve(ae->elements.size());
            for (auto& el : ae->elements) {
                auto v = evalE(el.get());
                if (!v) return std::nullopt;
                elems.push_back(std::move(*v));
            }
            return ConstValue::fromArr(std::move(elems));
        }

        // ── String / array indexing ───────────────────────────────────────
        case ASTNodeType::INDEX_EXPR: {
            auto* idx = static_cast<IndexExpr*>(e);
            auto arrVal = evalE(idx->array.get());
            auto idxVal = evalE(idx->index.get());
            if (!arrVal || !idxVal || idxVal->kind != ConstValue::Kind::Integer)
                return std::nullopt;
            int64_t i = idxVal->intVal;
            if (arrVal->kind == ConstValue::Kind::String) {
                const std::string& s = arrVal->strVal;
                if (i < 0 || i >= static_cast<int64_t>(s.size())) return std::nullopt;
                return ConstValue::fromInt(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
            }
            if (arrVal->kind == ConstValue::Kind::Array) {
                if (i < 0 || i >= static_cast<int64_t>(arrVal->arrVal.size())) return std::nullopt;
                return arrVal->arrVal[static_cast<size_t>(i)];
            }
            // Try looking up named array from env via the array expression
            if (idx->array->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* idE = static_cast<IdentifierExpr*>(idx->array.get());
                auto ait = env.find(idE->name);
                if (ait != env.end() && ait->second.kind == ConstValue::Kind::Array) {
                    if (i < 0 || i >= static_cast<int64_t>(ait->second.arrVal.size()))
                        return std::nullopt;
                    return ait->second.arrVal[static_cast<size_t>(i)];
                }
            }
            return std::nullopt;
        }

        // ── Enum / scope resolution ──────────────────────────────────────
        case ASTNodeType::SCOPE_RESOLUTION_EXPR: {
            auto* sr = static_cast<ScopeResolutionExpr*>(e);
            auto eit = enumConstants_.find(sr->scopeName + "_" + sr->memberName);
            if (eit != enumConstants_.end())
                return ConstValue::fromInt(static_cast<int64_t>(eit->second));
            return std::nullopt;
        }

        // ── Nested comptime block ────────────────────────────────────────
        case ASTNodeType::COMPTIME_EXPR: {
            auto* ct = static_cast<ComptimeExpr*>(e);
            std::unordered_map<std::string, ConstValue> nestedEnv;
            return tryConstEvalFull(ct->body.get(), nestedEnv, depth + 1);
        }

        // ── Function calls ───────────────────────────────────────────────
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<CallExpr*>(e);

            // Zero-arg pre-classified functions.
            // Prefer the unified OptimizationContext; fall back to private maps
            // when optCtx_ is not yet available (called during analysis passes).
            if (call->arguments.empty()) {
                if (optCtx_) {
                    if (auto v = optCtx_->constIntReturn(call->callee))
                        return ConstValue::fromInt(*v);
                    if (auto v = optCtx_->constStringReturn(call->callee))
                        return ConstValue::fromStr(*v);
                } else {
                    auto iit = constIntReturnFunctions_.find(call->callee);
                    if (iit != constIntReturnFunctions_.end())
                        return ConstValue::fromInt(iit->second);
                    auto sit = constStringReturnFunctions_.find(call->callee);
                    if (sit != constStringReturnFunctions_.end())
                        return ConstValue::fromStr(sit->second);
                }
            }

            // Evaluate all arguments.
            std::vector<ConstValue> foldedArgs;
            foldedArgs.reserve(call->arguments.size());
            bool allConst = true;
            for (auto& arg : call->arguments) {
                auto av = evalE(arg.get());
                if (!av) { allConst = false; break; }
                foldedArgs.push_back(std::move(*av));
            }
            if (allConst) {
                // Try built-in pure functions first.
                if (auto bv = evalConstBuiltin(call->callee, foldedArgs))
                    return bv;
                // Try user-defined functions via body evaluation.
                auto declIt = functionDecls_.find(call->callee);
                if (declIt != functionDecls_.end() && declIt->second->body &&
                    call->arguments.size() == declIt->second->parameters.size()) {
                    std::unordered_map<std::string, ConstValue> callEnv;
                    for (size_t i = 0; i < foldedArgs.size(); ++i)
                        callEnv[declIt->second->parameters[i].name] = foldedArgs[i];
                    return tryConstEvalFull(declIt->second, callEnv, depth + 1);
                }
            }
            return std::nullopt;
        }

        // ── Pipe expression: x |> f → f(x) ──────────────────────────────
        case ASTNodeType::PIPE_EXPR: {
            auto* pipe = static_cast<PipeExpr*>(e);
            auto lv = evalE(pipe->left.get());
            if (!lv) return std::nullopt;
            // Desugar to f(x)
            std::vector<ConstValue> args = {std::move(*lv)};
            if (auto bv = evalConstBuiltin(pipe->functionName, args))
                return bv;
            auto declIt = functionDecls_.find(pipe->functionName);
            if (declIt != functionDecls_.end() && declIt->second->body &&
                declIt->second->parameters.size() == 1) {
                std::unordered_map<std::string, ConstValue> callEnv;
                callEnv[declIt->second->parameters[0].name] = args[0];
                return tryConstEvalFull(declIt->second, callEnv, depth + 1);
            }
            return std::nullopt;
        }

        default: return std::nullopt;
        }
    };

    evalS = [&](Statement* s) -> bool {
        if (!s || retVal || breakSeen || continueSeen) return true;
        switch (s->type) {

        // ── Return ───────────────────────────────────────────────────────
        case ASTNodeType::RETURN_STMT: {
            auto* ret = static_cast<ReturnStmt*>(s);
            if (!ret->value) { retVal = ConstValue::fromInt(0); return true; }
            auto v = evalE(ret->value.get());
            if (!v) return false;
            retVal = *v;
            return true;
        }

        // ── Variable declaration (const and non-const) ───────────────────
        case ASTNodeType::VAR_DECL: {
            auto* decl = static_cast<VarDecl*>(s);
            if (decl->initializer) {
                auto v = evalE(decl->initializer.get());
                if (!v) return false;
                env[decl->name] = *v;
            } else {
                env[decl->name] = ConstValue::fromInt(0);
            }
            return true;
        }

        // ── MoveDecl: treat like VarDecl ─────────────────────────────────
        case ASTNodeType::MOVE_DECL: {
            auto* md = static_cast<MoveDecl*>(s);
            if (md->initializer) {
                auto v = evalE(md->initializer.get());
                if (!v) return false;
                env[md->name] = *v;
            } else {
                env[md->name] = ConstValue::fromInt(0);
            }
            return true;
        }

        // ── Expression statement (assignment, ++/--, etc.) ────────────────
        case ASTNodeType::EXPR_STMT: {
            auto* es = static_cast<ExprStmt*>(s);
            // Named assignment: x = expr
            if (es->expression->type == ASTNodeType::ASSIGN_EXPR) {
                auto* assign = static_cast<AssignExpr*>(es->expression.get());
                auto v = evalE(assign->value.get());
                if (!v) return false;
                env[assign->name] = *v;
                return true;
            }
            // Array element assignment: arr[i] = val
            if (es->expression->type == ASTNodeType::INDEX_ASSIGN_EXPR) {
                auto* ia = static_cast<IndexAssignExpr*>(es->expression.get());
                if (ia->array->type == ASTNodeType::IDENTIFIER_EXPR) {
                    auto* idE = static_cast<IdentifierExpr*>(ia->array.get());
                    auto ait = env.find(idE->name);
                    if (ait != env.end() && ait->second.kind == ConstValue::Kind::Array) {
                        auto idxv = evalE(ia->index.get());
                        auto valv = evalE(ia->value.get());
                        if (idxv && valv && idxv->kind == ConstValue::Kind::Integer) {
                            int64_t i = idxv->intVal;
                            auto& arr = ait->second.arrVal;
                            if (i >= 0 && i < static_cast<int64_t>(arr.size())) {
                                arr[static_cast<size_t>(i)] = std::move(*valv);
                                return true;
                            }
                        }
                    }
                }
                // Can't model the mutation — give up.
                return false;
            }
            // Any other expr (x++, f(), etc.): evaluate for side effects.
            // If the expression is not foldable (e.g. it calls a function with
            // I/O or other side effects) evalE returns nullopt → give up.
            // Also record the result as the implicit return candidate so that
            // `comptime { expr; }` without an explicit `return` still works.
            auto v = evalE(es->expression.get());
            if (v) lastBareExprVal = *v;
            return v.has_value();
        }

        // ── If / else ────────────────────────────────────────────────────
        case ASTNodeType::IF_STMT: {
            auto* ifs = static_cast<IfStmt*>(s);
            auto cond = evalE(ifs->condition.get());
            if (!cond || cond->kind != ConstValue::Kind::Integer) return false;
            if (cond->intVal != 0) return evalS(ifs->thenBranch.get());
            if (ifs->elseBranch)   return evalS(ifs->elseBranch.get());
            return true;
        }

        // ── For-range loop: for (i in start...end[...step]) ──────────────
        case ASTNodeType::FOR_STMT: {
            auto* fs = static_cast<ForStmt*>(s);
            auto sv = evalE(fs->start.get());
            auto ev = evalE(fs->end.get());
            if (!sv || !ev || sv->kind != ConstValue::Kind::Integer ||
                ev->kind != ConstValue::Kind::Integer)
                return false;
            int64_t step = (sv->intVal <= ev->intVal) ? 1 : -1;
            if (fs->step) {
                auto stv = evalE(fs->step.get());
                if (!stv || stv->kind != ConstValue::Kind::Integer) return false;
                step = stv->intVal;
            }
            if (step == 0) return false;  // infinite loop — reject
            int64_t cur = sv->intVal, end = ev->intVal;
            while ((step > 0 ? cur < end : cur > end)) {
                if (++fuel > kFuelLimit) return false;
                env[fs->iteratorVar] = ConstValue::fromInt(cur);
                if (!evalS(fs->body.get())) return false;
                if (retVal) { env.erase(fs->iteratorVar); return true; }
                if (breakSeen) { breakSeen = false; break; }
                continueSeen = false;
                cur += step;
            }
            env.erase(fs->iteratorVar);
            return true;
        }

        // ── ForEach loop: for (c in collection) ──────────────────────────
        case ASTNodeType::FOR_EACH_STMT: {
            auto* fes = static_cast<ForEachStmt*>(s);
            auto collVal = evalE(fes->collection.get());
            if (!collVal) return false;
            if (collVal->kind == ConstValue::Kind::String) {
                // Iterate over characters (as integer char codes)
                const std::string str = collVal->strVal;  // copy in case env mutates
                for (size_t i = 0; i < str.size(); ++i) {
                    if (++fuel > kFuelLimit) return false;
                    env[fes->iteratorVar] =
                        ConstValue::fromInt(static_cast<unsigned char>(str[i]));
                    if (!evalS(fes->body.get())) return false;
                    if (retVal) { env.erase(fes->iteratorVar); return true; }
                    if (breakSeen) { breakSeen = false; break; }
                    continueSeen = false;
                }
            } else if (collVal->kind == ConstValue::Kind::Array) {
                // Iterate over array elements
                const auto arr = collVal->arrVal;  // copy in case env mutates
                for (size_t i = 0; i < arr.size(); ++i) {
                    if (++fuel > kFuelLimit) return false;
                    env[fes->iteratorVar] = arr[i];
                    if (!evalS(fes->body.get())) return false;
                    if (retVal) { env.erase(fes->iteratorVar); return true; }
                    if (breakSeen) { breakSeen = false; break; }
                    continueSeen = false;
                }
            } else {
                return false;
            }
            env.erase(fes->iteratorVar);
            return true;
        }

        // ── While loop ───────────────────────────────────────────────────
        case ASTNodeType::WHILE_STMT: {
            auto* ws = static_cast<WhileStmt*>(s);
            while (true) {
                if (++fuel > kFuelLimit) return false;
                auto cond = evalE(ws->condition.get());
                if (!cond || cond->kind != ConstValue::Kind::Integer) return false;
                if (cond->intVal == 0) break;
                if (!evalS(ws->body.get())) return false;
                if (retVal) return true;
                if (breakSeen) { breakSeen = false; break; }
                continueSeen = false;
            }
            return true;
        }

        // ── Do-while loop ────────────────────────────────────────────────
        case ASTNodeType::DO_WHILE_STMT: {
            auto* dw = static_cast<DoWhileStmt*>(s);
            do {
                if (++fuel > kFuelLimit) return false;
                if (!evalS(dw->body.get())) return false;
                if (retVal) return true;
                if (breakSeen) { breakSeen = false; break; }
                continueSeen = false;
                auto cond = evalE(dw->condition.get());
                if (!cond || cond->kind != ConstValue::Kind::Integer) return false;
                if (cond->intVal == 0) break;
            } while (true);
            return true;
        }

        // ── Switch ───────────────────────────────────────────────────────
        case ASTNodeType::SWITCH_STMT: {
            auto* sw = static_cast<SwitchStmt*>(s);
            auto cond = evalE(sw->condition.get());
            if (!cond || cond->kind != ConstValue::Kind::Integer) return false;
            int64_t condVal = cond->intVal;
            const SwitchCase* matched = nullptr;
            const SwitchCase* defaultCase = nullptr;
            for (auto& c : sw->cases) {
                bool isDefault = !c.value && c.values.empty();
                if (isDefault) { defaultCase = &c; continue; }
                // Check primary value
                if (c.value) {
                    auto cv = evalE(c.value.get());
                    if (!cv || cv->kind != ConstValue::Kind::Integer) return false;
                    if (cv->intVal == condVal) { matched = &c; break; }
                }
                // Check multi-value arms
                bool found = false;
                for (auto& vx : c.values) {
                    auto cv = evalE(vx.get());
                    if (!cv || cv->kind != ConstValue::Kind::Integer) return false;
                    if (cv->intVal == condVal) { found = true; break; }
                }
                if (found) { matched = &c; break; }
            }
            const SwitchCase* target = matched ? matched : defaultCase;
            if (!target) return true;
            for (auto& stmt : target->body) {
                if (!evalS(stmt.get())) return false;
                if (retVal) return true;
                if (breakSeen) { breakSeen = false; return true; }
                if (continueSeen) return true;
            }
            return true;
        }

        // ── Break / Continue ─────────────────────────────────────────────
        case ASTNodeType::BREAK_STMT:
            breakSeen = true;
            return true;
        case ASTNodeType::CONTINUE_STMT:
            continueSeen = true;
            return true;

        // ── Block ────────────────────────────────────────────────────────
        case ASTNodeType::BLOCK: {
            auto* blk = static_cast<BlockStmt*>(s);
            // Track variables declared in this block so they can be removed
            // (or their shadowed values restored) when the block exits.
            // This correctly handles shadowing: if inner `var x` shadows outer
            // `x`, we save the outer value and restore it on exit.
            std::vector<std::pair<std::string, std::optional<ConstValue>>> scopeGuard;
            for (auto& stmt : blk->statements) {
                if (stmt->type == ASTNodeType::VAR_DECL) {
                    auto* decl = static_cast<VarDecl*>(stmt.get());
                    auto it = env.find(decl->name);
                    if (it != env.end())
                        scopeGuard.emplace_back(decl->name, it->second);
                    else
                        scopeGuard.emplace_back(decl->name, std::nullopt);
                } else if (stmt->type == ASTNodeType::MOVE_DECL) {
                    auto* md = static_cast<MoveDecl*>(stmt.get());
                    auto it = env.find(md->name);
                    if (it != env.end())
                        scopeGuard.emplace_back(md->name, it->second);
                    else
                        scopeGuard.emplace_back(md->name, std::nullopt);
                }
                if (!evalS(stmt.get())) {
                    // Restore scope on failure
                    for (auto& [nm, val] : scopeGuard) {
                        if (val) env[nm] = *val; else env.erase(nm);
                    }
                    return false;
                }
                if (retVal || breakSeen || continueSeen) {
                    for (auto& [nm, val] : scopeGuard) {
                        if (val) env[nm] = *val; else env.erase(nm);
                    }
                    return true;
                }
            }
            // Normal exit: restore scope
            for (auto& [nm, val] : scopeGuard) {
                if (val) env[nm] = *val; else env.erase(nm);
            }
            return true;
        }

        default:
            // Prefetch, assume, freeze, invalidate, defer — safe no-ops at CT.
            if (s->type == ASTNodeType::PREFETCH_STMT ||
                s->type == ASTNodeType::ASSUME_STMT ||
                s->type == ASTNodeType::FREEZE_STMT ||
                s->type == ASTNodeType::INVALIDATE_STMT ||
                s->type == ASTNodeType::DEFER_STMT)
                return true;
            // try/catch, I/O, etc. — not safe to fold.
            return false;
        }
    };

    for (auto& stmt : func->body->statements) {
        if (!evalS(stmt.get())) return std::nullopt;
        if (retVal) return retVal;
    }
    // No explicit `return` — use the last bare expression value if available.
    // This enables `comptime { expr; }` as an implicit-return expression block.
    if (!retVal && lastBareExprVal) return lastBareExprVal;
    return retVal;
}

// BlockStmt overload: evaluate a standalone block at compile time.
// Used by comptime {} expressions that aren't attached to a FunctionDecl.
std::optional<CodeGenerator::ConstValue>
CodeGenerator::tryConstEvalFull(
    const BlockStmt* body,
    const std::unordered_map<std::string, ConstValue>& argEnv,
    int depth) const {
    if (!body || depth > 48) return std::nullopt;
    // Synthesize a minimal FunctionDecl pointing at the block.
    // We use a raw pointer without ownership transfer — the block is owned
    // by the ComptimeExpr AST node that called us.
    FunctionDecl synFn("__comptime__", {}, {}, nullptr);
    // Temporarily assign the body pointer for the duration of evaluation.
    // We must restore it (set to nullptr) before the synFn destructor runs
    // to avoid double-free since synFn doesn't own the body.
    synFn.body.reset(const_cast<BlockStmt*>(body));
    auto result = tryConstEvalFull(&synFn, argEnv, depth);
    synFn.body.release(); // release without deleting — body is not owned here
    return result;
}
// Pre-pass over the program AST that identifies zero-parameter, pure functions
// whose return value is always the same compile-time constant.
//
// Uses tryConstEvalFull with an empty argument environment to evaluate each
// zero-parameter function body.  The analysis runs as a fixed-point loop
// because a function may depend on another not yet classified:
//   fn a() { return "hello"; }          // classified in iteration 1
//   fn b() { return a() + " world"; }   // classified in iteration 2 (uses a)
//
// Results are stored in constStringReturnFunctions_ and constIntReturnFunctions_.
void CodeGenerator::analyzeConstantReturnValues(Program* program) {
    if (optimizationLevel < OptimizationLevel::O1) return;

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& func : program->functions) {
            const std::string& fname = func->name;
            if (!func->body || !func->parameters.empty()) continue;
            if (constStringReturnFunctions_.count(fname) ||
                constIntReturnFunctions_.count(fname)) continue;

            // Evaluate with empty arg environment (zero-parameter function).
            static const std::unordered_map<std::string, ConstValue> emptyEnv;
            auto result = tryConstEvalFull(func.get(), emptyEnv);
            if (!result) continue;

            if (result->kind == ConstValue::Kind::Integer) {
                constIntReturnFunctions_[fname] = result->intVal;
                if (optCtx_) optCtx_->mutableFacts(fname).constIntReturn = result->intVal;
            } else if (result->kind == ConstValue::Kind::String) {
                constStringReturnFunctions_[fname] = result->strVal;
                if (optCtx_) optCtx_->mutableFacts(fname).constStringReturn = result->strVal;
            } else {
                // Array or other compound type — not representable as a simple
                // constant return value; skip without registering.
                continue;
            }
            changed = true;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runCFCTRE: Cross-Function Compile-Time Reasoning Engine pipeline phase.
//
// Initialises or resets the CTEngine, registers all program functions and
// enum/global constants, then runs the full CF-CTRE analysis pass.  After
// this call:
//   - ctEngine_->isPure(fnName)        — fast O(1) purity query
//   - ctEngine_->executeFunction(...)  — memoised CT evaluation
//   - ctEngine_->evalComptimeBlock(...)— block-level CT evaluation
//
// Also propagates CT results back into the CodeGenerator's legacy fold maps
// (constIntReturnFunctions_, constStringReturnFunctions_) so that the
// existing tryFoldExprToConst / tryConstEvalFull machinery benefits from
// CF-CTRE's richer analysis.
// ─────────────────────────────────────────────────────────────────────────────
void CodeGenerator::runCFCTRE(Program* program) {
    if (!program) return;

    // Initialise CTEngine (may be called multiple times for repl/incremental).
    ctEngine_ = std::make_unique<CTEngine>();

    // Register global integer / string constants gathered by earlier passes.
    for (auto& [name, val] : constIntFolds_)
        ctEngine_->registerGlobalConst(name.str(), CTValue::fromI64(val));
    for (auto& [name, val] : constStringFolds_)
        ctEngine_->registerGlobalConst(name.str(), CTValue::fromString(val));
    for (auto& [name, val] : constArrayFolds_) {
        ConstValue cv = ConstValue::fromArr(val);
        ctEngine_->registerGlobalConst(name.str(), constValueToCTValue(cv));
    }

    // Register enum constants.
    for (auto& [name, val] : enumConstants_)
        ctEngine_->registerEnumConst(name.str(), static_cast<int64_t>(val));

    // Run the main CF-CTRE pass (registers all functions, computes purity,
    // pre-evaluates zero-arg pure functions, builds the call graph).
    ctEngine_->runPass(program);

    // Back-propagate CF-CTRE results into the legacy fold tables so the
    // existing tryFoldExprToConst / tryConstEvalFull helpers remain effective.
    for (auto& fn : program->functions) {
        if (!fn->parameters.empty()) continue;
        if (!ctEngine_->isPure(fn->name)) continue;
        // Query the memoised zero-arg result.
        auto result = ctEngine_->executeFunction(fn->name, {});
        if (!result) continue;
        if (result->isInt() &&
            !constIntReturnFunctions_.count(fn->name)) {
            constIntReturnFunctions_[fn->name] = result->asI64();
            if (optCtx_) optCtx_->mutableFacts(fn->name).constIntReturn = result->asI64();
        } else if (result->isString() &&
                   !constStringReturnFunctions_.count(fn->name)) {
            constStringReturnFunctions_[fn->name] = result->asStr();
            if (optCtx_) optCtx_->mutableFacts(fn->name).constStringReturn = result->asStr();
        }
    }

    // Back-propagate Phase 7 (uniform return values) — functions with parameters
    // that always return the same constant, proven by symbolic argument evaluation.
    // These are added to the same fold tables as zero-arg functions above.
    for (auto& [name, ctVal] : ctEngine_->uniformReturnValues()) {
        if (ctVal.isInt() && !constIntReturnFunctions_.count(name)) {
            constIntReturnFunctions_[name] = ctVal.asI64();
            if (optCtx_) optCtx_->mutableFacts(name).constIntReturn = ctVal.asI64();
        } else if (ctVal.isString() && !constStringReturnFunctions_.count(name)) {
            constStringReturnFunctions_[name] = ctVal.asStr();
            if (optCtx_) optCtx_->mutableFacts(name).constStringReturn = ctVal.asStr();
        }
    }

    // Apply Phase 8 dead-function hints: mark unreachable functions as cold
    // so LLVM's HotColdSplitting / GlobalDCE can eliminate them.  We use
    // hintCold rather than removing the function body so that the LLVM IR
    // is still valid; GlobalDCE removes the body after DCE confirms no callers.
    // Explicitly @noinline so the inliner doesn't try to inline dead code.
    if (!ctEngine_->deadFunctions().empty()) {
        const auto& dead = ctEngine_->deadFunctions();
        for (auto& fn : program->functions) {
            if (!dead.count(fn->name)) continue;
            if (fn->hintCold) continue;   // user already annotated
            fn->hintCold    = true;
            fn->hintNoInline = true;
        }
    }

    if (verbose_) {
        const auto& s = ctEngine_->stats();
        std::cout << "  [cfctre] Pass complete: "
                  << s.functionsRegistered   << " functions registered, "
                  << s.pureFunctionsDetected << " pure, "
                  << s.functionCallsMemoized << " calls memoised, "
                  << s.arraysAllocated       << " arrays allocated, "
                  << s.loopsReasoned         << " loops reasoned, "
                  << s.branchMerges          << " branch merges, "
                  << s.ternaryMerges         << " ternary merges, "
                  << s.uniformReturnFunctionsFound << " uniform-return, "
                  << s.deadFunctionsDetected << " dead\n"
                  << "  [cfctre/absinterp] "
                  << s.deadBranchesEliminated << " dead branches, "
                  << s.safeArrayAccesses      << " safe array accesses, "
                  << s.safeDivisions          << " safe divisions, "
                  << s.cheaperRewritesFound   << " cheaper rewrites" << '\n';
    }

    // ── CF-CTRE-guided inline hints ────────────────────────────────────────
    // Functions that CF-CTRE successfully evaluated (with concrete arguments)
    // are prime candidates for inlining at the remaining runtime call sites:
    // once inlined, LLVM's IPSCCP pass can propagate the same constants and
    // produce the same constant-folded output for statically-known arguments.
    //
    // We apply InlineHint (not AlwaysInline) so LLVM's cost model still gates
    // excessively-large callees — the hint is advisory, not mandatory.
    //
    // Skipped for: @noinline functions (explicit user opt-out), zero-parameter
    // functions (already folded to global constants above), and functions not
    // in the foldable set (no evidence that inlining would expose folding).
    if (ctEngine_ && optimizationLevel >= OptimizationLevel::O2) {
        const auto& foldable = ctEngine_->foldableCallees();
        for (auto& fn : program->functions) {
            if (fn->hintNoInline) continue;
            if (fn->parameters.empty()) continue;   // zero-arg already folded
            if (!foldable.count(fn->name)) continue;
            // Only upgrade to hintInline if the function isn't already more
            // aggressively annotated (@flatten / alwaysInline).
            if (!fn->hintInline)
                fn->hintInline = true;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runSynthesisPass — public wrapper (delegates to the free function in
// synthesize.h so the Orchestrator can call it without include gymnastics).
// ─────────────────────────────────────────────────────────────────────────────

void CodeGenerator::runSynthesisPass(Program* program, bool verbose) {
    ::omscript::runSynthesisPass(program, verbose);
}

// ─────────────────────────────────────────────────────────────────────────────
// runEGraphPass — conditional wrapper called by the Orchestrator.
//
// 1. Checks the optimization level and enableEGraph_ flag.
// 2. Configures the EGraphSubsystem in the OptimizationContext based on the
//    current optimization level (higher levels = more liberal node limits).
// 3. Delegates to ctx.egraph().optimizeProgram() so all e-graph work goes
//    through the subsystem (config + stats are tracked there).
// ─────────────────────────────────────────────────────────────────────────────

void CodeGenerator::runEGraphPass(Program* program, OptimizationContext& ctx) {
    if (!enableEGraph_ || optimizationLevel < OptimizationLevel::O2) {
        if (verbose_ && optimizationLevel < OptimizationLevel::O2) {
            std::cout << "  [opt] E-graph optimization skipped (requires O2+)" << '\n';
        }
        return;
    }

    // Set per-level configuration on the subsystem before running.
    // O3 / OPTMAX get higher limits; O2 gets the conservative defaults.
    EGraphConfig cfg;
    if (optimizationLevel >= OptimizationLevel::O3) {
        cfg.maxNodes      = 50000;
        cfg.maxIterations = 30;
    } else {
        cfg.maxNodes      = 10000;
        cfg.maxIterations = 15;
    }
    cfg.enableConstFolding = true;
    ctx.egraph().setConfig(cfg);

    if (verbose_) {
        std::cout << "  [opt] Running e-graph equality saturation on AST ("
                  << program->functions.size() << " functions, maxNodes="
                  << cfg.maxNodes << ", maxIter=" << cfg.maxIterations
                  << ")..." << '\n';
    }

    // All work goes through the subsystem.
    ctx.egraph().optimizeProgram(program);

    if (verbose_) {
        const auto& s = ctx.egraph().stats();
        std::cout << "  [opt] E-graph saturation complete: "
                  << s.expressionsSimplified << "/" << s.expressionsAttempted
                  << " expressions simplified, "
                  << s.functionsChanged << " functions changed, "
                  << s.expressionsSkipped << " skipped (not representable)\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ctValueToConstValue / constValueToCTValue — bridge between the two
// value representations used inside CodeGenerator.
// ─────────────────────────────────────────────────────────────────────────────

CodeGenerator::ConstValue CodeGenerator::ctValueToConstValue(const CTValue& v) const {
    switch (v.kind) {
    case CTValueKind::CONCRETE_I64:
    case CTValueKind::CONCRETE_U64:
    case CTValueKind::CONCRETE_BOOL:
        return ConstValue::fromInt(v.asI64());
    case CTValueKind::CONCRETE_STRING:
        return ConstValue::fromStr(v.asStr());
    case CTValueKind::CONCRETE_ARRAY: {
        if (!ctEngine_) return ConstValue{};
        auto elems = ctEngine_->extractArray(v.asArr());
        std::vector<ConstValue> cvElems;
        cvElems.reserve(elems.size());
        for (const auto& e : elems)
            cvElems.push_back(ctValueToConstValue(e));
        return ConstValue::fromArr(std::move(cvElems));
    }
    default:
        return ConstValue{};
    }
}

CTValue CodeGenerator::constValueToCTValue(const ConstValue& v) const {
    switch (v.kind) {
    case ConstValue::Kind::Integer: return CTValue::fromI64(v.intVal);
    case ConstValue::Kind::String:  return CTValue::fromString(v.strVal);
    case ConstValue::Kind::Array: {
        if (!ctEngine_) return CTValue::uninit();
        CTArrayHandle h = ctEngine_->heap().nextHandle();
        // Use the non-const heap via ctEngine_ (cast is safe — evaluation already done).
        // We create a snapshot of the inline array into the CT heap.
        // Accessing ctEngine_'s heap requires a const_cast here because
        // CTHeap::alloc/store are non-const operations.
        CTHeap& hp = const_cast<CTHeap&>(ctEngine_->heap());
        h = hp.alloc(static_cast<uint64_t>(v.arrVal.size()));
        for (size_t i = 0; i < v.arrVal.size(); ++i)
            hp.store(h, static_cast<int64_t>(i),
                     constValueToCTValue(v.arrVal[i]));
        return CTValue::fromArray(h);
    }
    default:
        return CTValue::uninit();
    }
}

// buildComptimeEnv: snapshot all compile-time-known local variables into a
// CTValue map that can be passed as the 'env' parameter to evalComptimeBlock.
// This lets comptime{} blocks reference variables declared earlier in the
// same function body (e.g. a 'var base = comptime {...}' array that is
// referenced in a later 'var transformed = comptime { multi_stage(base); }').
//
// Sources:
//   • constIntFolds_   — integer vars whose current value is compile-time known
//   • constStringFolds_ — string vars similarly known
//   • constArrayFolds_  — array vars folded to a vector<ConstValue>; each is
//                          converted to a fresh CT heap array handle via
//                          constValueToCTValue so the CF-CTRE evaluator can
//                          index into it.
std::unordered_map<std::string, CTValue>
CodeGenerator::buildComptimeEnv() const {
    std::unordered_map<std::string, CTValue> env;
    if (!ctEngine_) return env;
    for (const auto& e : constIntFolds_)
        env[e.first().str()] = CTValue::fromI64(e.second);
    for (const auto& e : scopeComptimeInts_)
        env[e.first().str()] = CTValue::fromI64(e.second);
    for (const auto& e : constStringFolds_)
        env[e.first().str()] = CTValue::fromString(e.second);
    for (const auto& e : constArrayFolds_) {
        auto ctv = constValueToCTValue(ConstValue::fromArr(e.second));
        if (ctv.isKnown())
            env[e.first().str()] = std::move(ctv);
    }
    return env;
}

// autoDetectConstEvalFunctions: identify user-defined functions with parameters
// that are "pure" (no I/O, no global mutations, arithmetic/logic/conditional
// bodies only) and register them in constEvalFunctions_.  This enables the
// existing tryConstEval/tryConstEvalFull machinery to fold calls to these
// functions at compile time when all arguments are constants, without requiring
// explicit @const_eval annotations.
//
// Purity is detected via a fixed-point analysis:
//   - A function is pure if its body contains only pure statements/expressions
//   - A call expression is pure only if the callee is a known-pure function
//   - The fixed-point loop handles mutual recursion (A calls B calls A) by
//     conservatively marking mutually-recursive functions as not pure
//
// Runs at O1+ after analyzeConstantReturnValues so that zero-arg const funcs
// are already in constIntReturnFunctions_ and can be recognized as pure calls.
void CodeGenerator::autoDetectConstEvalFunctions(Program* program) {
    if (!program || optimizationLevel < OptimizationLevel::O1) return;

    // Purity queries now delegated to the unified BuiltinEffectTable.
    // BuiltinEffectTable::isPure(name)   replaces kPureBuiltins.count(name)
    // BuiltinEffectTable::isImpure(name) replaces kImpureBuiltins.count(name)

    // Build index of all user function declarations for O(1) lookup.
    std::unordered_map<std::string, const FunctionDecl*> allFuncs;
    for (const auto& func : program->functions) {
        allFuncs[func->name] = func.get();
    }

    // Track which user functions are currently known-pure.
    std::unordered_set<std::string> knownPure;

    // Seed with functions already in constEvalFunctions_ (explicit @const_eval).
    for (auto it = constEvalFunctions_.begin(); it != constEvalFunctions_.end(); ++it) {
        knownPure.insert(it->getKey().str());
    }
    // Seed with zero-arg functions already analyzed as const-return.
    // Query both the private maps (written during pre-passes) and optCtx_ (may
    // have been written directly by earlier passes in this compilation).
    for (const auto& kv : constIntReturnFunctions_) {
        knownPure.insert(kv.first().str());
    }
    for (const auto& kv : constStringReturnFunctions_) {
        knownPure.insert(kv.first().str());
    }
    if (optCtx_) {
        for (const auto& [name, ff] : optCtx_->allFacts()) {
            if (ff.constIntReturn || ff.constStringReturn || ff.isConstFoldable)
                knownPure.insert(name);
        }
    }

    // Helpers to check purity of AST nodes (forward-declared via lambdas).
    std::function<bool(const Expression*)> isExprPure;
    std::function<bool(const Statement*)> isStmtPure;

    isExprPure = [&](const Expression* expr) -> bool {
        if (!expr) return true;
        switch (expr->type) {
        case ASTNodeType::LITERAL_EXPR:
        case ASTNodeType::IDENTIFIER_EXPR:
            return true;

        case ASTNodeType::BINARY_EXPR: {
            auto* bin = static_cast<const BinaryExpr*>(expr);
            return isExprPure(bin->left.get()) && isExprPure(bin->right.get());
        }
        case ASTNodeType::UNARY_EXPR: {
            auto* un = static_cast<const UnaryExpr*>(expr);
            return isExprPure(un->operand.get());
        }
        case ASTNodeType::TERNARY_EXPR: {
            auto* tern = static_cast<const TernaryExpr*>(expr);
            return isExprPure(tern->condition.get()) &&
                   isExprPure(tern->thenExpr.get()) &&
                   isExprPure(tern->elseExpr.get());
        }
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<const CallExpr*>(expr);
            // Impure builtins: not pure (uses unified BuiltinEffectTable).
            if (BuiltinEffectTable::isImpure(call->callee)) return false;
            // Known-pure builtins: OK (uses unified BuiltinEffectTable).
            if (BuiltinEffectTable::isPure(call->callee)) {
                for (const auto& arg : call->arguments) {
                    if (!isExprPure(arg.get())) return false;
                }
                return true;
            }
            // User-defined function: pure only if we already know it's pure.
            if (knownPure.count(call->callee)) {
                for (const auto& arg : call->arguments) {
                    if (!isExprPure(arg.get())) return false;
                }
                return true;
            }
            // Unknown/impure function.
            return false;
        }
        case ASTNodeType::MOVE_EXPR: {
            auto* mv = static_cast<const MoveExpr*>(expr);
            return isExprPure(mv->source.get());
        }
        case ASTNodeType::BORROW_EXPR: {
            auto* bw = static_cast<const BorrowExpr*>(expr);
            return isExprPure(bw->source.get());
        }
        case ASTNodeType::COMPTIME_EXPR:
            // A comptime {} block is always a compile-time constant — always pure.
            return true;
        case ASTNodeType::POSTFIX_EXPR:
            // ++/-- are mutations — not pure for const-eval.
            return false;
        case ASTNodeType::PREFIX_EXPR: {
            auto* pre = static_cast<const PrefixExpr*>(expr);
            if (pre->op == "!" || pre->op == "-" || pre->op == "+")
                return isExprPure(pre->operand.get());
            return false;  // ++/-- are mutations
        }
        default:
            // IndexExpr, AssignExpr, ArrayLiteral, etc. — conservative: not pure
            // for const-eval purposes (may allocate or mutate).
            return false;
        }
    };

    isStmtPure = [&](const Statement* stmt) -> bool {
        if (!stmt) return true;
        switch (stmt->type) {
        case ASTNodeType::RETURN_STMT: {
            auto* ret = static_cast<const ReturnStmt*>(stmt);
            return !ret->value || isExprPure(ret->value.get());
        }
        case ASTNodeType::VAR_DECL: {
            auto* vd = static_cast<const VarDecl*>(stmt);
            return !vd->initializer || isExprPure(vd->initializer.get());
        }
        case ASTNodeType::MOVE_DECL: {
            auto* md = static_cast<const MoveDecl*>(stmt);
            return !md->initializer || isExprPure(md->initializer.get());
        }
        case ASTNodeType::BLOCK: {
            auto* block = static_cast<const BlockStmt*>(stmt);
            for (const auto& s : block->statements) {
                if (!isStmtPure(s.get())) return false;
            }
            return true;
        }
        case ASTNodeType::IF_STMT: {
            auto* ifS = static_cast<const IfStmt*>(stmt);
            return isExprPure(ifS->condition.get()) &&
                   isStmtPure(ifS->thenBranch.get()) &&
                   isStmtPure(ifS->elseBranch.get());
        }
        case ASTNodeType::WHILE_STMT: {
            auto* ws = static_cast<const WhileStmt*>(stmt);
            return isExprPure(ws->condition.get()) && isStmtPure(ws->body.get());
        }
        case ASTNodeType::DO_WHILE_STMT: {
            auto* dws = static_cast<const DoWhileStmt*>(stmt);
            return isStmtPure(dws->body.get()) && isExprPure(dws->condition.get());
        }
        case ASTNodeType::FOR_STMT: {
            auto* fs = static_cast<const ForStmt*>(stmt);
            return isExprPure(fs->start.get()) &&
                   isExprPure(fs->end.get()) &&
                   (!fs->step || isExprPure(fs->step.get())) &&
                   isStmtPure(fs->body.get());
        }
        case ASTNodeType::EXPR_STMT: {
            // Allow pure expression statements (e.g. function calls used for
            // their return value stored in a variable).
            auto* es = static_cast<const ExprStmt*>(stmt);
            return isExprPure(es->expression.get());
        }
        case ASTNodeType::INVALIDATE_STMT:
            // invalidate frees memory — conservative: not pure.
            return false;
        case ASTNodeType::BREAK_STMT:
        case ASTNodeType::CONTINUE_STMT:
            return true;
        case ASTNodeType::SWITCH_STMT: {
            auto* sw = static_cast<const SwitchStmt*>(stmt);
            if (!isExprPure(sw->condition.get())) return false;
            for (const auto& c : sw->cases) {
                if (c.value && !isExprPure(c.value.get())) return false;
                for (const auto& v : c.values)
                    if (!isExprPure(v.get())) return false;
                for (const auto& s : c.body)
                    if (!isStmtPure(s.get())) return false;
            }
            return true;
        }
        default:
            return false;
        }
    };

    // Fixed-point iteration: keep adding functions to knownPure until no more
    // can be classified.  This handles call chains A→B→C where B and C must
    // be proven pure before A can be classified.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& func : program->functions) {
            const std::string& fname = func->name;
            // Skip functions already known pure or already in constEvalFunctions_.
            if (knownPure.count(fname)) continue;
            // Must have a body to analyze.
            if (!func->body) continue;
            // Must have at least one parameter (zero-arg handled separately).
            if (func->parameters.empty()) continue;
            // Skip functions annotated @const_eval already (handled at registration time).
            if (func->hintConstEval) continue;
            // Skip functions with @optnone or @noinline.
            if (func->hintOptNone) continue;

            // Check if the entire body is pure.
            bool bodyIsPure = true;
            for (const auto& stmt : func->body->statements) {
                if (!isStmtPure(stmt.get())) {
                    bodyIsPure = false;
                    break;
                }
            }

            if (bodyIsPure) {
                knownPure.insert(fname);
                constEvalFunctions_.insert(fname);
                if (optCtx_) optCtx_->mutableFacts(fname).isConstFoldable = true;
                changed = true;
            }
        }
    }
}

// inferFunctionEffects: lightweight AST-level side-effect analysis.
//
// For every user function in the program, computes a FunctionEffects summary:
//   readsMemory  — function loads from an array element, string subscript,
//                  struct field, or calls a read-only builtin with pointer args.
//   writesMemory — function writes to an array element (arr[i] = ...), struct
//                  field, or calls a mutating builtin (push/pop/sort/reverse…).
//   hasIO        — function calls any I/O builtin (print/input/file_*/sleep…).
//   hasMutation  — function calls a mutating builtin or performs a compound
//                  assignment on a parameter-derived value.
//
// The analysis is a conservative fixed-point over the call graph: a function
// inherits the effects of every callee.  Mutual recursion is handled by
// initialising all unknown functions as "no effects" and iterating to a
// fixed point; a self-recursive call conservatively marks writesMemory.
//
// Results are used in function codegen to:
//   1. Automatically add LLVM readonly/readnone/nosync attributes.
//   2. Emit a compiler warning when @pure is used on a function whose body
//      contains detectable I/O or mutation.
void CodeGenerator::inferFunctionEffects(Program* program) {
    if (!program) return;

    // Effect queries now delegated to the unified BuiltinEffectTable.
    // BuiltinEffectTable::isIO(name)       replaces kIOBuiltins.count(name)
    // BuiltinEffectTable::isMutating(name) replaces kMutatingBuiltins.count(name)
    // BuiltinEffectTable::isReadOnly(name) replaces kReadBuiltins.count(name)

    // Build function index.
    std::unordered_map<std::string, const FunctionDecl*> allFuncs;
    for (const auto& f : program->functions)
        allFuncs[f->name] = f.get();

    // Helper: classify one expression's effects into a FunctionEffects.
    // Forward-declared as std::function so it can recurse.
    std::function<FunctionEffects(const Expression*, const std::string&)> exprEffects;
    std::function<FunctionEffects(const Statement*,   const std::string&)> stmtEffects;

    exprEffects = [&](const Expression* expr, const std::string& selfName) -> FunctionEffects {
        FunctionEffects fx;
        if (!expr) return fx;
        switch (expr->type) {
        case ASTNodeType::INDEX_EXPR:
            fx.readsMemory = true;
            break;
        case ASTNodeType::INDEX_ASSIGN_EXPR:
            fx.writesMemory = true;
            fx.hasMutation  = true;
            break;
        case ASTNodeType::FIELD_ACCESS_EXPR:
            fx.readsMemory = true;
            break;
        case ASTNodeType::FIELD_ASSIGN_EXPR:
            fx.writesMemory = true;
            fx.hasMutation  = true;
            break;
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<const CallExpr*>(expr);
            if (BuiltinEffectTable::isIO(call->callee)) {
                fx.hasIO = true;
            } else if (BuiltinEffectTable::isMutating(call->callee)) {
                fx.writesMemory = true;
                fx.hasMutation  = true;
            } else if (BuiltinEffectTable::isReadOnly(call->callee)) {
                fx.readsMemory = true;
            } else if (call->callee == selfName) {
                // Self-recursive call: conservatively mark memory access
                fx.readsMemory  = true;
                fx.writesMemory = true;
            } else {
                // Callee effects propagated from functionEffects_ (fixed-point)
                auto it = functionEffects_.find(call->callee);
                if (it != functionEffects_.end()) {
                    fx.readsMemory  = fx.readsMemory  || it->second.readsMemory;
                    fx.writesMemory = fx.writesMemory || it->second.writesMemory;
                    fx.hasIO        = fx.hasIO        || it->second.hasIO;
                    fx.hasMutation  = fx.hasMutation  || it->second.hasMutation;
                }
            }
            for (const auto& arg : call->arguments) {
                auto a = exprEffects(arg.get(), selfName);
                fx.readsMemory  = fx.readsMemory  || a.readsMemory;
                fx.writesMemory = fx.writesMemory || a.writesMemory;
                fx.hasIO        = fx.hasIO        || a.hasIO;
                fx.hasMutation  = fx.hasMutation  || a.hasMutation;
            }
            break;
        }
        case ASTNodeType::BINARY_EXPR: {
            auto* b = static_cast<const BinaryExpr*>(expr);
            auto l = exprEffects(b->left.get(), selfName);
            auto r = exprEffects(b->right.get(), selfName);
            fx.readsMemory  = l.readsMemory  || r.readsMemory;
            fx.writesMemory = l.writesMemory || r.writesMemory;
            fx.hasIO        = l.hasIO        || r.hasIO;
            fx.hasMutation  = l.hasMutation  || r.hasMutation;
            // Division and modulo by a non-constant (or zero) divisor emit a
            // runtime div-by-zero check in codegen that calls puts() + exit(1).
            // Mark hasIO so the function is NOT classified as pure/readnone,
            // preventing incorrect memory(none)+speculatable+willreturn
            // attributes that allow the optimizer to miscompile the check.
            if (b->op == "/" || b->op == "%") {
                auto* lit = dynamic_cast<const LiteralExpr*>(b->right.get());
                bool divisorIsNonZeroConst = lit &&
                    lit->literalType == LiteralExpr::LiteralType::INTEGER &&
                    lit->intValue != 0;
                if (!divisorIsNonZeroConst)
                    fx.hasIO = true;
            }
            break;
        }
        case ASTNodeType::ASSIGN_EXPR: {
            auto* a = static_cast<const AssignExpr*>(expr);
            auto v = exprEffects(a->value.get(), selfName);
            // Compound assignment to a variable is a mutation.
            fx.writesMemory = true;
            fx.hasMutation  = true;
            fx.readsMemory  = v.readsMemory;
            fx.hasIO        = v.hasIO;
            break;
        }
        case ASTNodeType::UNARY_EXPR: {
            auto* u = static_cast<const UnaryExpr*>(expr);
            fx = exprEffects(u->operand.get(), selfName);
            break;
        }
        case ASTNodeType::POSTFIX_EXPR:
        case ASTNodeType::PREFIX_EXPR:
            // ++/-- are mutations
            fx.writesMemory = true;
            fx.hasMutation  = true;
            break;
        case ASTNodeType::TERNARY_EXPR: {
            auto* t = static_cast<const TernaryExpr*>(expr);
            auto c = exprEffects(t->condition.get(), selfName);
            auto th = exprEffects(t->thenExpr.get(), selfName);
            auto el = exprEffects(t->elseExpr.get(), selfName);
            fx.readsMemory  = c.readsMemory  || th.readsMemory  || el.readsMemory;
            fx.writesMemory = c.writesMemory || th.writesMemory || el.writesMemory;
            fx.hasIO        = c.hasIO        || th.hasIO        || el.hasIO;
            fx.hasMutation  = c.hasMutation  || th.hasMutation  || el.hasMutation;
            break;
        }
        case ASTNodeType::ARRAY_EXPR: {
            auto* arr = static_cast<const ArrayExpr*>(expr);
            for (const auto& el : arr->elements) {
                auto e = exprEffects(el.get(), selfName);
                fx.readsMemory  = fx.readsMemory  || e.readsMemory;
                fx.writesMemory = fx.writesMemory || e.writesMemory;
                fx.hasIO        = fx.hasIO        || e.hasIO;
                fx.hasMutation  = fx.hasMutation  || e.hasMutation;
            }
            break;
        }
        default:
            // Literals, identifiers, move/borrow/freeze — no effects.
            break;
        }
        return fx;
    };

    stmtEffects = [&](const Statement* stmt, const std::string& selfName) -> FunctionEffects {
        FunctionEffects fx;
        if (!stmt) return fx;

        auto merge = [&](const FunctionEffects& other) {
            fx.readsMemory  = fx.readsMemory  || other.readsMemory;
            fx.writesMemory = fx.writesMemory || other.writesMemory;
            fx.hasIO        = fx.hasIO        || other.hasIO;
            fx.hasMutation  = fx.hasMutation  || other.hasMutation;
        };

        switch (stmt->type) {
        case ASTNodeType::EXPR_STMT:
            merge(exprEffects(static_cast<const ExprStmt*>(stmt)->expression.get(), selfName));
            break;
        case ASTNodeType::VAR_DECL: {
            auto* vd = static_cast<const VarDecl*>(stmt);
            if (vd->initializer) merge(exprEffects(vd->initializer.get(), selfName));
            break;
        }
        case ASTNodeType::MOVE_DECL: {
            auto* md = static_cast<const MoveDecl*>(stmt);
            if (md->initializer) merge(exprEffects(md->initializer.get(), selfName));
            break;
        }
        case ASTNodeType::RETURN_STMT: {
            auto* ret = static_cast<const ReturnStmt*>(stmt);
            if (ret->value) merge(exprEffects(ret->value.get(), selfName));
            break;
        }
        case ASTNodeType::BLOCK: {
            for (const auto& s : static_cast<const BlockStmt*>(stmt)->statements)
                merge(stmtEffects(s.get(), selfName));
            break;
        }
        case ASTNodeType::IF_STMT: {
            auto* ifs = static_cast<const IfStmt*>(stmt);
            merge(exprEffects(ifs->condition.get(), selfName));
            merge(stmtEffects(ifs->thenBranch.get(), selfName));
            merge(stmtEffects(ifs->elseBranch.get(), selfName));
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            auto* ws = static_cast<const WhileStmt*>(stmt);
            merge(exprEffects(ws->condition.get(), selfName));
            merge(stmtEffects(ws->body.get(), selfName));
            break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            auto* dw = static_cast<const DoWhileStmt*>(stmt);
            merge(stmtEffects(dw->body.get(), selfName));
            merge(exprEffects(dw->condition.get(), selfName));
            break;
        }
        case ASTNodeType::FOR_STMT: {
            auto* fs = static_cast<const ForStmt*>(stmt);
            if (fs->start) merge(exprEffects(fs->start.get(), selfName));
            if (fs->end)   merge(exprEffects(fs->end.get(), selfName));
            if (fs->step)  merge(exprEffects(fs->step.get(), selfName));
            merge(stmtEffects(fs->body.get(), selfName));
            break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            auto* fe = static_cast<const ForEachStmt*>(stmt);
            merge(exprEffects(fe->collection.get(), selfName));
            merge(stmtEffects(fe->body.get(), selfName));
            // Iterating an array reads it.
            fx.readsMemory = true;
            break;
        }
        case ASTNodeType::SWITCH_STMT: {
            auto* sw = static_cast<const SwitchStmt*>(stmt);
            merge(exprEffects(sw->condition.get(), selfName));
            for (const auto& c : sw->cases) {
                if (c.value) merge(exprEffects(c.value.get(), selfName));
                for (const auto& s : c.body) merge(stmtEffects(s.get(), selfName));
            }
            break;
        }
        case ASTNodeType::CATCH_STMT: {
            auto* cs = static_cast<const CatchStmt*>(stmt);
            merge(stmtEffects(cs->body.get(), selfName));
            break;
        }
        case ASTNodeType::THROW_STMT: {
            auto* th = static_cast<const ThrowStmt*>(stmt);
            if (th->value) merge(exprEffects(th->value.get(), selfName));
            fx.hasIO = true; // throw is an observable effect
            break;
        }
        case ASTNodeType::PIPELINE_STMT: {
            auto* pl = static_cast<const PipelineStmt*>(stmt);
            if (pl->count) merge(exprEffects(pl->count.get(), selfName));
            for (const auto& stage : pl->stages)
                merge(stmtEffects(stage.body.get(), selfName));
            break;
        }
        case ASTNodeType::DEFER_STMT:
            merge(stmtEffects(static_cast<const DeferStmt*>(stmt)->body.get(), selfName));
            break;
        default:
            break;
        }
        return fx;
    };

    // Fixed-point iteration over the call graph.
    // Initialise all user functions with empty effects; iterate until stable.
    for (const auto& f : program->functions)
        functionEffects_[f->name] = FunctionEffects{};

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& f : program->functions) {
            if (!f->body) continue;
            FunctionEffects computed;
            for (const auto& s : f->body->statements) {
                auto fx = stmtEffects(s.get(), f->name);
                computed.readsMemory  = computed.readsMemory  || fx.readsMemory;
                computed.writesMemory = computed.writesMemory || fx.writesMemory;
                computed.hasIO        = computed.hasIO        || fx.hasIO;
                computed.hasMutation  = computed.hasMutation  || fx.hasMutation;
            }
            FunctionEffects& prev = functionEffects_[f->name];
            if (computed.readsMemory  != prev.readsMemory  ||
                computed.writesMemory != prev.writesMemory ||
                computed.hasIO        != prev.hasIO        ||
                computed.hasMutation  != prev.hasMutation) {
                prev = computed;
                changed = true;
            }
        }
    }

    // Propagate stable effects into OptimizationContext so IR emission can
    // query a single surface without waiting for syncFactsToContext.
    if (optCtx_) {
        for (const auto& kv : functionEffects_) {
            optCtx_->mutableFacts(kv.first).effects = kv.second;
        }
    }

    // Warn if @pure is applied to a function that has detectable side effects.
    for (const auto& f : program->functions) {
        if (!f->hintPure) continue;
        // Prefer optCtx_ (already written above); fall back to private map.
        const FunctionEffects* fxp = nullptr;
        FunctionEffects tmpFx;
        if (optCtx_) {
            tmpFx = optCtx_->effects(f->name);
            fxp = &tmpFx;
        } else {
            auto it = functionEffects_.find(f->name);
            if (it == functionEffects_.end()) continue;
            fxp = &it->second;
        }
        const FunctionEffects& fx = *fxp;
        if (fx.hasIO) {
            std::cerr << "[warning] @pure function '" << f->name
                      << "' performs I/O — @pure annotation may be incorrect\n";
        } else if (fx.writesMemory || fx.hasMutation) {
            std::cerr << "[warning] @pure function '" << f->name
                      << "' mutates memory — @pure annotation may be incorrect\n";
        }
    }
}


} // namespace omscript

namespace omscript {
// maintaining a pool of interned global string constants.  When the
// same string content is used in multiple places, this returns a
// pointer to the single canonical global, enabling:
//   1. Reduced data section size (no duplicate string constants)
//   2. Pointer-equality comparison for identical strings
//   3. Better D-cache utilization (fewer unique cache lines)
llvm::GlobalVariable* CodeGenerator::internString(const std::string& content) {
    auto it = internedStrings_.find(content);
    if (it != internedStrings_.end()) {
        return it->second;
    }

    // Create a new global string constant with internal linkage and
    // unnamed_addr so LLVM can merge it with other identical constants.
    auto* strConst = llvm::ConstantDataArray::getString(*context, content, /*AddNull=*/true);
    auto* gv = new llvm::GlobalVariable(
        *module,
        strConst->getType(),
        /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage,
        strConst,
        ".str.intern");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    gv->setAlignment(llvm::Align(1));

    internedStrings_[content] = gv;
    return gv;
}

llvm::Value* CodeGenerator::emitComptimeArray(const std::vector<ConstValue>& elems) {
    // Build [N+1 x i64] constant with OmScript's array layout:
    //   slot 0  = N (the length)
    //   slot 1…N = element values
    auto* i64Ty = llvm::Type::getInt64Ty(*context);
    size_t N = elems.size();
    std::vector<llvm::Constant*> vals;
    vals.reserve(N + 1);
    vals.push_back(llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(N)));
    for (const auto& elem : elems) {
        // Only integer elements are supported in comptime arrays for now;
        // non-integer elements are clamped to 0 rather than failing hard.
        int64_t v = (elem.kind == ConstValue::Kind::Integer) ? elem.intVal : 0;
        vals.push_back(llvm::ConstantInt::get(i64Ty, v));
    }
    auto* arrTy = llvm::ArrayType::get(i64Ty, N + 1);
    auto* arrConst = llvm::ConstantArray::get(arrTy, vals);
    auto* gv = new llvm::GlobalVariable(
        *module, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, arrConst, ".comptime.arr");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    gv->setAlignment(llvm::Align(8));
    return builder->CreatePtrToInt(gv, i64Ty, "comptime.arr");
}

void CodeGenerator::generateAssume(AssumeStmt* stmt) {
    llvm::Value* cond = generateExpression(stmt->condition.get());
    // Convert to i1 if needed
    if (!cond->getType()->isIntegerTy(1)) {
        cond = builder->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "assume.cond");
    }
    llvm::Function* assumeFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::assume, {});
    builder->CreateCall(assumeFn, {cond});

    if (stmt->deoptBody) {
        // if (!cond) { deoptBody }
        llvm::Function* parent = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* deoptBB = llvm::BasicBlock::Create(*context, "deopt", parent);
        llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(*context, "after.assume", parent);
        builder->CreateCondBr(cond, afterBB, deoptBB);
        builder->SetInsertPoint(deoptBB);
        generateStatement(stmt->deoptBody.get());
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(afterBB);
        }
        builder->SetInsertPoint(afterBB);
    }
}

// ---------------------------------------------------------------------------
// Escape analysis for stack allocation
// ---------------------------------------------------------------------------
// Check whether a local array variable escapes the current function scope.
// Returns true if the array is safe for stack allocation (no escape).
// Conservative: we only return true when we can PROVE it doesn't escape.
// ---------------------------------------------------------------------------

/// Scan an expression tree for any use of varName that would cause escape:
///  - passed as an argument to a function call
///  - used as the object of a return statement (detected at statement level)
static bool exprUsesVar(Expression* expr, const std::string& varName) {
    if (!expr) return false;
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
        return static_cast<IdentifierExpr*>(expr)->name == varName;
    }
    if (expr->type == ASTNodeType::CALL_EXPR) {
        auto* call = static_cast<CallExpr*>(expr);
        for (auto& arg : call->arguments) {
            if (exprUsesVar(arg.get(), varName)) return true;
        }
        return false; // callee name is a string, not an expression
    }
    if (expr->type == ASTNodeType::BINARY_EXPR) {
        auto* bin = static_cast<BinaryExpr*>(expr);
        return exprUsesVar(bin->left.get(), varName) ||
               exprUsesVar(bin->right.get(), varName);
    }
    if (expr->type == ASTNodeType::UNARY_EXPR) {
        return exprUsesVar(static_cast<UnaryExpr*>(expr)->operand.get(), varName);
    }
    if (expr->type == ASTNodeType::INDEX_EXPR) {
        auto* idx = static_cast<IndexExpr*>(expr);
        return exprUsesVar(idx->array.get(), varName) ||
               exprUsesVar(idx->index.get(), varName);
    }
    if (expr->type == ASTNodeType::ASSIGN_EXPR) {
        auto* asgn = static_cast<AssignExpr*>(expr);
        return exprUsesVar(asgn->value.get(), varName);
    }
    return false;
}

/// Returns true if varName escapes in any statement in the block (from stmtIdx onwards).
static bool varEscapesInBlock(const BlockStmt* block, const std::string& varName,
                               size_t startIdx) {
    for (size_t i = startIdx; i < block->statements.size(); ++i) {
        const Statement* s = block->statements[i].get();
        // Return: escapes if returned
        if (auto* ret = dynamic_cast<const ReturnStmt*>(s)) {
            if (ret->value && exprUsesVar(ret->value.get(), varName)) return true;
        }
        // VarDecl initializer: could be assigned to another variable and passed
        if (auto* vd = dynamic_cast<const VarDecl*>(s)) {
            if (vd->initializer && exprUsesVar(vd->initializer.get(), varName)) return true;
        }
        // ExprStmt: call args
        if (auto* es = dynamic_cast<const ExprStmt*>(s)) {
            if (exprUsesVar(es->expression.get(), varName)) {
                // Check if it's a call expression with varName as arg
                if (es->expression->type == ASTNodeType::CALL_EXPR) {
                    auto* call = static_cast<CallExpr*>(es->expression.get());
                    for (auto& arg : call->arguments) {
                        if (exprUsesVar(arg.get(), varName)) return true;
                    }
                }
            }
        }
    }
    return false;
}

bool CodeGenerator::doesVarEscapeCurrentScope(const std::string& varName) const {
    if (!currentFuncDecl_ || !currentFuncDecl_->body) return true; // conservative
    // Find the VarDecl statement index for this variable.
    const BlockStmt* body = currentFuncDecl_->body.get();
    for (size_t i = 0; i < body->statements.size(); ++i) {
        const auto* vd = dynamic_cast<const VarDecl*>(body->statements[i].get());
        if (vd && vd->name == varName) {
            return varEscapesInBlock(body, varName, i + 1);
        }
    }
    // Not at top-level: assume it might escape.
    return true;
}



/// Compare two expressions for equality (only handles literals and identifiers).
static bool exprEqual(const Expression* a, const Expression* b) {
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    if (a->type == ASTNodeType::LITERAL_EXPR) {
        const auto* la = static_cast<const LiteralExpr*>(a);
        const auto* lb = static_cast<const LiteralExpr*>(b);
        if (la->literalType != lb->literalType) return false;
        if (la->literalType == LiteralExpr::LiteralType::INTEGER)
            return la->intValue == lb->intValue;
        if (la->literalType == LiteralExpr::LiteralType::STRING)
            return la->stringValue == lb->stringValue;
        return la->floatValue == lb->floatValue;
    }
    if (a->type == ASTNodeType::IDENTIFIER_EXPR) {
        const auto* ia = static_cast<const IdentifierExpr*>(a);
        const auto* ib = static_cast<const IdentifierExpr*>(b);
        return ia->name == ib->name;
    }
    return false;
}

void CodeGenerator::fuseLoops(BlockStmt* block) {
    // Recursively apply to nested blocks first.
    for (auto& stmt : block->statements) {
        if (auto* blk = dynamic_cast<BlockStmt*>(stmt.get())) {
            fuseLoops(blk);
        } else if (auto* fr = dynamic_cast<ForStmt*>(stmt.get())) {
            if (fr->body) {
                if (auto* innerBlk = dynamic_cast<BlockStmt*>(fr->body.get())) {
                    fuseLoops(innerBlk);
                }
            }
        }
    }

    // Single-pass fusion: walk statement list looking for adjacent ForStmt pairs.
    auto& stmts = block->statements;
    size_t i = 0;
    while (i + 1 < stmts.size()) {
        auto* first  = dynamic_cast<ForStmt*>(stmts[i].get());
        auto* second = dynamic_cast<ForStmt*>(stmts[i + 1].get());

        if (!first || !second) {
            ++i;
            continue;
        }
        // At least one must have @fuse.
        if (!first->loopHints.fuse && !second->loopHints.fuse) {
            ++i;
            continue;
        }
        // Must have identical start and end bounds.
        if (!exprEqual(first->start.get(), second->start.get()) ||
            !exprEqual(first->end.get(),   second->end.get())) {
            ++i;
            continue;
        }

        // Merge: build a combined body.
        // The second iterator is aliased to the first via a VarDecl.
        std::vector<std::unique_ptr<Statement>> combined;

        // Only add the alias if the second loop uses a different iterator name.
        if (second->iteratorVar != first->iteratorVar) {
            auto aliasIdent = std::make_unique<IdentifierExpr>(first->iteratorVar);
            aliasIdent->line = first->line;
            auto aliasDecl = std::make_unique<VarDecl>(
                second->iteratorVar, std::move(aliasIdent), false, "");
            aliasDecl->line = first->line;
            combined.push_back(std::move(aliasDecl));
        }

        // Append first body's statements.
        if (auto* firstBlk = dynamic_cast<BlockStmt*>(first->body.get())) {
            for (auto& s : firstBlk->statements)
                combined.push_back(std::move(s));
        } else if (first->body) {
            combined.push_back(std::move(first->body));
        }

        // Append second body's statements.
        if (auto* secondBlk = dynamic_cast<BlockStmt*>(second->body.get())) {
            for (auto& s : secondBlk->statements)
                combined.push_back(std::move(s));
        } else if (second->body) {
            combined.push_back(std::move(second->body));
        }

        auto fusedBody = std::make_unique<BlockStmt>(std::move(combined));

        // Build the fused ForStmt.
        LoopConfig fusedHints = first->loopHints;
        fusedHints.fuse = false; // already fused
        auto fused = std::make_unique<ForStmt>(
            first->iteratorVar,
            std::move(first->start),
            std::move(first->end),
            std::move(first->step),
            std::move(fusedBody),
            first->iteratorType);
        fused->loopHints = fusedHints;
        fused->line = first->line;
        fused->column = first->column;

        // Replace first with fused, erase second.
        stmts[i] = std::move(fused);
        stmts.erase(stmts.begin() + static_cast<ptrdiff_t>(i + 1));

        optStats_.loopsFused++;
        // Don't advance i — try to fuse again with the next statement.
    }
}

} // namespace omscript
