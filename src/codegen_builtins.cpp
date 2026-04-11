#include "codegen.h"
#include "diagnostic.h"
#include "hardware_graph.h"
#include <cstdlib>
#include <iostream>

// Apply maximum compiler optimizations to this hot path.
// Builtin codegen covers array operations, string handling, and math builtins
// which are called very frequently and contain tight inner loops.
#ifdef __GNUC__
#  pragma GCC optimize("O3,unroll-loops,tree-vectorize")
#endif
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Verifier.h>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

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

namespace omscript {

// ---------------------------------------------------------------------------
// Builtin function dispatch table
// ---------------------------------------------------------------------------
// Maps builtin function names to integer IDs for O(1) lookup via hash map
// instead of scanning ~80 if/else string comparisons linearly.

enum class BuiltinId : uint8_t {
    NONE = 0,   // Not a builtin — fall through to user-defined function lookup
    PRINT, ABS, LEN, MIN, MAX, SIGN, CLAMP, POW, PRINT_CHAR, INPUT,
    INPUT_LINE, SQRT, FAST_ADD, FAST_SUB, FAST_MUL, FAST_DIV,
    PRECISE_ADD, PRECISE_SUB, PRECISE_MUL, PRECISE_DIV,
    IS_EVEN, IS_ODD, SUM, SWAP, REVERSE, TO_CHAR, IS_ALPHA,
    IS_DIGIT, TYPEOF, ASSERT, STR_LEN, CHAR_AT, STR_EQ, STR_CONCAT, LOG2,
    GCD, TO_STRING, STR_FIND, FLOOR, CEIL, ROUND, TO_INT, TO_FLOAT,
    STR_SUBSTR, STR_UPPER, STR_LOWER, STR_CONTAINS, STR_INDEX_OF,
    STR_REPLACE, STR_TRIM, STR_STARTS_WITH, STR_ENDS_WITH, STR_REPEAT,
    STR_REVERSE, PUSH, POP, INDEX_OF, ARRAY_CONTAINS, SORT, ARRAY_FILL,
    ARRAY_CONCAT, ARRAY_SLICE, ARRAY_COPY, ARRAY_REMOVE, ARRAY_MAP,
    ARRAY_FILTER, ARRAY_REDUCE, PRINTLN, WRITE, EXIT_PROGRAM,
    RANDOM, TIME, SLEEP, STR_TO_INT, STR_TO_FLOAT, STR_SPLIT, STR_CHARS,
    FILE_READ, FILE_WRITE, FILE_APPEND, FILE_EXISTS, MAP_NEW, MAP_SET,
    MAP_GET, MAP_HAS, MAP_REMOVE, MAP_KEYS, MAP_VALUES, MAP_SIZE,
    RANGE, RANGE_STEP, CHAR_CODE, NUMBER_TO_STRING, STRING_TO_NUMBER,
    THREAD_CREATE, THREAD_JOIN, MUTEX_NEW, MUTEX_LOCK, MUTEX_UNLOCK,
    MUTEX_DESTROY,
    SIN, COS, TAN, ASIN, ACOS, ATAN, ATAN2, EXP, LOG, LOG10, CBRT, HYPOT,
    ARRAY_MIN, ARRAY_MAX, ARRAY_ANY, ARRAY_EVERY, ARRAY_FIND, ARRAY_COUNT,
    STR_JOIN, STR_COUNT,
    POPCOUNT, CLZ, CTZ, BITREVERSE, EXP2, IS_POWER_OF_2,
    LCM,
    // New intrinsic builtins
    ROTATE_LEFT, ROTATE_RIGHT, BSWAP, SATURATING_ADD, SATURATING_SUB,
    FMA_BUILTIN, COPYSIGN, MIN_FLOAT, MAX_FLOAT
};

static const std::unordered_map<std::string_view, BuiltinId> builtinLookup = {
    {"print", BuiltinId::PRINT},
    {"abs", BuiltinId::ABS},
    {"len", BuiltinId::LEN},
    {"min", BuiltinId::MIN},
    {"max", BuiltinId::MAX},
    {"sign", BuiltinId::SIGN},
    {"clamp", BuiltinId::CLAMP},
    {"pow", BuiltinId::POW},
    {"print_char", BuiltinId::PRINT_CHAR},
    {"input", BuiltinId::INPUT},
    {"input_line", BuiltinId::INPUT_LINE},
    {"sqrt", BuiltinId::SQRT},
    {"fast_add", BuiltinId::FAST_ADD},
    {"fast_sub", BuiltinId::FAST_SUB},
    {"fast_mul", BuiltinId::FAST_MUL},
    {"fast_div", BuiltinId::FAST_DIV},
    {"precise_add", BuiltinId::PRECISE_ADD},
    {"precise_sub", BuiltinId::PRECISE_SUB},
    {"precise_mul", BuiltinId::PRECISE_MUL},
    {"precise_div", BuiltinId::PRECISE_DIV},
    {"is_even", BuiltinId::IS_EVEN},
    {"is_odd", BuiltinId::IS_ODD},
    {"sum", BuiltinId::SUM},
    {"swap", BuiltinId::SWAP},
    {"reverse", BuiltinId::REVERSE},
    {"to_char", BuiltinId::TO_CHAR},
    {"is_alpha", BuiltinId::IS_ALPHA},
    {"is_digit", BuiltinId::IS_DIGIT},
    {"typeof", BuiltinId::TYPEOF},
    {"assert", BuiltinId::ASSERT},
    {"str_len", BuiltinId::STR_LEN},
    {"char_at", BuiltinId::CHAR_AT},
    {"str_eq", BuiltinId::STR_EQ},
    {"str_concat", BuiltinId::STR_CONCAT},
    {"log2", BuiltinId::LOG2},
    {"gcd", BuiltinId::GCD},
    {"to_string", BuiltinId::TO_STRING},
    {"str_find", BuiltinId::STR_FIND},
    {"floor", BuiltinId::FLOOR},
    {"ceil", BuiltinId::CEIL},
    {"round", BuiltinId::ROUND},
    {"to_int", BuiltinId::TO_INT},
    {"to_float", BuiltinId::TO_FLOAT},
    {"str_substr", BuiltinId::STR_SUBSTR},
    {"str_upper", BuiltinId::STR_UPPER},
    {"str_lower", BuiltinId::STR_LOWER},
    {"str_contains", BuiltinId::STR_CONTAINS},
    {"str_index_of", BuiltinId::STR_INDEX_OF},
    {"str_replace", BuiltinId::STR_REPLACE},
    {"str_trim", BuiltinId::STR_TRIM},
    {"str_starts_with", BuiltinId::STR_STARTS_WITH},
    {"str_ends_with", BuiltinId::STR_ENDS_WITH},
    {"str_repeat", BuiltinId::STR_REPEAT},
    {"str_reverse", BuiltinId::STR_REVERSE},
    {"push", BuiltinId::PUSH},
    {"pop", BuiltinId::POP},
    {"index_of", BuiltinId::INDEX_OF},
    {"array_contains", BuiltinId::ARRAY_CONTAINS},
    {"sort", BuiltinId::SORT},
    {"array_fill", BuiltinId::ARRAY_FILL},
    {"array_concat", BuiltinId::ARRAY_CONCAT},
    {"array_slice", BuiltinId::ARRAY_SLICE},
    {"array_copy", BuiltinId::ARRAY_COPY},
    {"array_remove", BuiltinId::ARRAY_REMOVE},
    {"array_map", BuiltinId::ARRAY_MAP},
    {"array_filter", BuiltinId::ARRAY_FILTER},
    {"array_reduce", BuiltinId::ARRAY_REDUCE},
    {"println", BuiltinId::PRINTLN},
    {"write", BuiltinId::WRITE},
    {"exit_program", BuiltinId::EXIT_PROGRAM},
    {"exit", BuiltinId::EXIT_PROGRAM},
    {"random", BuiltinId::RANDOM},
    {"time", BuiltinId::TIME},
    {"sleep", BuiltinId::SLEEP},
    {"str_to_int", BuiltinId::STR_TO_INT},
    {"str_to_float", BuiltinId::STR_TO_FLOAT},
    {"str_split", BuiltinId::STR_SPLIT},
    {"str_chars", BuiltinId::STR_CHARS},
    {"file_read", BuiltinId::FILE_READ},
    {"file_write", BuiltinId::FILE_WRITE},
    {"file_append", BuiltinId::FILE_APPEND},
    {"file_exists", BuiltinId::FILE_EXISTS},
    {"map_new", BuiltinId::MAP_NEW},
    {"map_set", BuiltinId::MAP_SET},
    {"map_get", BuiltinId::MAP_GET},
    {"map_has", BuiltinId::MAP_HAS},
    {"map_remove", BuiltinId::MAP_REMOVE},
    {"map_keys", BuiltinId::MAP_KEYS},
    {"map_values", BuiltinId::MAP_VALUES},
    {"map_size", BuiltinId::MAP_SIZE},
    {"range", BuiltinId::RANGE},
    {"range_step", BuiltinId::RANGE_STEP},
    {"char_code", BuiltinId::CHAR_CODE},
    {"number_to_string", BuiltinId::NUMBER_TO_STRING},
    {"string_to_number", BuiltinId::STRING_TO_NUMBER},
    {"thread_create", BuiltinId::THREAD_CREATE},
    {"thread_join", BuiltinId::THREAD_JOIN},
    {"mutex_new", BuiltinId::MUTEX_NEW},
    {"mutex_lock", BuiltinId::MUTEX_LOCK},
    {"mutex_unlock", BuiltinId::MUTEX_UNLOCK},
    {"mutex_destroy", BuiltinId::MUTEX_DESTROY},
    {"sin", BuiltinId::SIN},
    {"cos", BuiltinId::COS},
    {"tan", BuiltinId::TAN},
    {"asin", BuiltinId::ASIN},
    {"acos", BuiltinId::ACOS},
    {"atan", BuiltinId::ATAN},
    {"atan2", BuiltinId::ATAN2},
    {"exp", BuiltinId::EXP},
    {"log", BuiltinId::LOG},
    {"log10", BuiltinId::LOG10},
    {"cbrt", BuiltinId::CBRT},
    {"hypot", BuiltinId::HYPOT},
    {"array_min", BuiltinId::ARRAY_MIN},
    {"array_max", BuiltinId::ARRAY_MAX},
    {"array_any", BuiltinId::ARRAY_ANY},
    {"array_every", BuiltinId::ARRAY_EVERY},
    {"array_find", BuiltinId::ARRAY_FIND},
    {"array_count", BuiltinId::ARRAY_COUNT},
    {"str_join", BuiltinId::STR_JOIN},
    {"str_count", BuiltinId::STR_COUNT},
    {"popcount", BuiltinId::POPCOUNT},
    {"clz", BuiltinId::CLZ},
    {"ctz", BuiltinId::CTZ},
    {"bitreverse", BuiltinId::BITREVERSE},
    {"exp2", BuiltinId::EXP2},
    {"is_power_of_2", BuiltinId::IS_POWER_OF_2},
    {"lcm", BuiltinId::LCM},
    {"rotate_left", BuiltinId::ROTATE_LEFT},
    {"rotate_right", BuiltinId::ROTATE_RIGHT},
    {"bswap", BuiltinId::BSWAP},
    {"saturating_add", BuiltinId::SATURATING_ADD},
    {"saturating_sub", BuiltinId::SATURATING_SUB},
    {"fma", BuiltinId::FMA_BUILTIN},
    {"copysign", BuiltinId::COPYSIGN},
    {"min_float", BuiltinId::MIN_FLOAT},
    {"max_float", BuiltinId::MAX_FLOAT},
};

static BuiltinId lookupBuiltin(const std::string& name) {
    auto it = builtinLookup.find(std::string_view(name));
    return it != builtinLookup.end() ? it->second : BuiltinId::NONE;
}

// ---------------------------------------------------------------------------
// Compile-time constant folding helper
// ---------------------------------------------------------------------------
// Extract a compile-time integer constant from an AST expression node.
// Returns std::nullopt if the expression isn't a plain integer literal.
static std::optional<int64_t> getConstantInt(Expression* expr) {
    if (!expr || expr->type != ASTNodeType::LITERAL_EXPR) return std::nullopt;
    auto* lit = static_cast<LiteralExpr*>(expr);
    if (lit->literalType != LiteralExpr::LiteralType::INTEGER) return std::nullopt;
    return static_cast<int64_t>(lit->intValue);
}

void CodeGenerator::validateArgCount(const CallExpr* expr, const std::string& funcName, size_t expected) {
    if (expr->arguments.size() != expected) {
        codegenError("Built-in function '" + funcName + "' expects " + std::to_string(expected) +
                         " argument(s), but " + std::to_string(expr->arguments.size()) + " provided",
                     expr);
    }
}

llvm::Value* CodeGenerator::generateCall(CallExpr* expr) {
    // O(1) hash map lookup replaces the previous linear chain of ~80
    // string comparisons.  The switch below dispatches to the same
    // implementation code; only the dispatch mechanism has changed.
    BuiltinId bid = lookupBuiltin(expr->callee);

    // All stdlib built-in functions are compiled to native machine code below.
    if (bid == BuiltinId::PRINT) {
        validateArgCount(expr, "print", 1);
        Expression* argExpr = expr->arguments[0].get();
        // Fast path: string literal → use puts() instead of printf("%s\n", ...)
        // puts() appends a newline automatically and avoids format-string parsing.
        if (argExpr->type == ASTNodeType::LITERAL_EXPR) {
            auto* lit = static_cast<LiteralExpr*>(argExpr);
            if (lit->literalType == LiteralExpr::LiteralType::STRING) {
                llvm::Value* strVal = builder->CreateGlobalString(lit->stringValue, "print.lit");
                builder->CreateCall(getOrDeclarePuts(), {strVal});
                return llvm::ConstantInt::get(getDefaultType(), 0);
            }
        }
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
            // Use puts() instead of printf("%s\n", ...) — puts appends a
            // newline automatically and avoids format-string parsing overhead.
            builder->CreateCall(getOrDeclarePuts(), {arg});
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

    if (bid == BuiltinId::ABS) {
        validateArgCount(expr, "abs", 1);
        // Constant-fold abs(literal): eliminates the intrinsic call entirely.
        if (auto cv = getConstantInt(expr->arguments[0].get())) {
            int64_t v = *cv;
            int64_t result = (v < 0) ? -v : v;
            auto* c = llvm::ConstantInt::get(getDefaultType(), result);
            nonNegValues_.insert(c);
            return c;
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        if (arg->getType()->isDoubleTy()) {
            // Use llvm.fabs intrinsic for native hardware abs on floats
            llvm::Function* fabsIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::fabs, {getFloatType()});
            return builder->CreateCall(fabsIntrinsic, {arg}, "fabsval");
        }
        // Use llvm.abs.i64 intrinsic for native hardware abs on integers
        llvm::Function* absIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::abs, {getDefaultType()});
        // The second argument (is_int_min_poison) is false for safe behavior
        auto* result = builder->CreateCall(absIntrinsic, {arg, builder->getFalse()}, "absval");
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::LEN) {
        validateArgCount(expr, "len", 1);

        // Constant-fold len() on string literals: the length is known at
        // compile time, so emit a constant instead of a runtime strlen call.
        // This eliminates O(n) work per literal and enables further constant
        // propagation / dead code elimination in the optimizer.
        if (auto* lit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (lit->literalType == LiteralExpr::LiteralType::STRING) {
                int64_t len = static_cast<int64_t>(lit->stringValue.size());
                auto* result = llvm::ConstantInt::get(getDefaultType(), len);
                nonNegValues_.insert(result);
                return result;
            }
        }

        // Constant-fold len() on const string variables: when a variable
        // was declared as `const s = "hello"`, len(s) folds to 5.
        if (auto* ident = dynamic_cast<IdentifierExpr*>(expr->arguments[0].get())) {
            auto it = constStringFolds_.find(ident->name);
            if (it != constStringFolds_.end()) {
                int64_t len = static_cast<int64_t>(it->second.size());
                auto* result = llvm::ConstantInt::get(getDefaultType(), len);
                nonNegValues_.insert(result);
                return result;
            }
        }

        // Constant-fold len(array_fill(N, val)): when N is a constant the
        // array length is known at compile time.  Avoids a runtime load from
        // the array header and enables further constant propagation.
        if (auto* call = dynamic_cast<CallExpr*>(expr->arguments[0].get())) {
            if (call->callee == "array_fill" && call->arguments.size() == 2) {
                if (auto* sizelit = dynamic_cast<LiteralExpr*>(call->arguments[0].get())) {
                    if (sizelit->literalType == LiteralExpr::LiteralType::INTEGER && sizelit->intValue >= 0) {
                        auto* result = llvm::ConstantInt::get(getDefaultType(), sizelit->intValue);
                        nonNegValues_.insert(result);
                        return result;
                    }
                }
            }
        }

        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        Expression* argExpr = expr->arguments[0].get();

        // String detection uses two complementary checks:
        //   1. arg->getType()->isPointerTy()  — string literals (ptr @"...") and
        //      string variables whose LLVM alloca holds a ptr type.
        //   2. isStringExpr(argExpr) — string variables tracked in stringVars_
        //      that are stored as i64 (pointer cast to integer, OmScript's
        //      canonical runtime representation for strings passed across call
        //      boundaries).  In that case the IntToPtr below reconstructs the
        //      char* needed by strlen.
        // If neither is true the argument is assumed to be an array.
        if (arg->getType()->isPointerTy() || isStringExpr(argExpr)) {
            // Reconstruct the char* for strlen when the value is stored as i64.
            llvm::Value* strPtr = arg->getType()->isPointerTy()
                                      ? arg
                                      : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "len.sptr");
            llvm::Value* rawLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "len.strlen");
            // strlen returns size_t (i64 on 64-bit); ensure we return the default type.
            auto* result = rawLen->getType() == getDefaultType()
                       ? rawLen
                       : builder->CreateZExtOrTrunc(rawLen, getDefaultType(), "len.strsz");
            nonNegValues_.insert(result);
            return result;
        }
        // Array is stored as an i64 holding a pointer to [length, elem0, elem1, ...]
        // Convert to integer first if needed (e.g. if stored in a float variable)
        arg = toDefaultType(arg);
        llvm::Value* arrPtr =
            arg->getType()->isPointerTy() ? arg : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "arrptr");
        auto* arrlenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "arrlen");
        arrlenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        arrlenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        nonNegValues_.insert(arrlenLoad);
        return arrlenLoad;
    }

    if (bid == BuiltinId::MIN) {
        validateArgCount(expr, "min", 2);
        // Constant-fold min(a, b) when both are integer literals.
        if (auto ca = getConstantInt(expr->arguments[0].get())) {
            if (auto cb = getConstantInt(expr->arguments[1].get())) {
                int64_t result = std::min(*ca, *cb);
                auto* c = llvm::ConstantInt::get(getDefaultType(), result);
                if (result >= 0) nonNegValues_.insert(c);
                return c;
            }
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        if (a->getType()->isDoubleTy() || b->getType()->isDoubleTy()) {
            if (!a->getType()->isDoubleTy())
                a = ensureFloat(a);
            if (!b->getType()->isDoubleTy())
                b = ensureFloat(b);
            // Use llvm.minnum intrinsic for native hardware fmin
            llvm::Function* fminIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::minnum, {getFloatType()});
            return builder->CreateCall(fminIntrinsic, {a, b}, "fminval");
        }
        // Use llvm.smin intrinsic for native hardware signed integer min
        llvm::Function* sminIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smin, {getDefaultType()});
        auto* result = builder->CreateCall(sminIntrinsic, {a, b}, "minval");
        // min(a, b) is non-negative when both inputs are non-negative.
        // This enables nsw on downstream add/mul operations.
        if (nonNegValues_.count(a) && nonNegValues_.count(b))
            nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::MAX) {
        validateArgCount(expr, "max", 2);
        // Constant-fold max(a, b) when both are integer literals.
        if (auto ca = getConstantInt(expr->arguments[0].get())) {
            if (auto cb = getConstantInt(expr->arguments[1].get())) {
                int64_t result = std::max(*ca, *cb);
                auto* c = llvm::ConstantInt::get(getDefaultType(), result);
                if (result >= 0) nonNegValues_.insert(c);
                return c;
            }
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        if (a->getType()->isDoubleTy() || b->getType()->isDoubleTy()) {
            if (!a->getType()->isDoubleTy())
                a = ensureFloat(a);
            if (!b->getType()->isDoubleTy())
                b = ensureFloat(b);
            // Use llvm.maxnum intrinsic for native hardware fmax
            llvm::Function* fmaxIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::maxnum, {getFloatType()});
            return builder->CreateCall(fmaxIntrinsic, {a, b}, "fmaxval");
        }
        // Use llvm.smax intrinsic for native hardware signed integer max
        llvm::Function* smaxIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smax, {getDefaultType()});
        auto* result = builder->CreateCall(smaxIntrinsic, {a, b}, "maxval");
        // max(a, b) is non-negative when either input is non-negative:
        // if a >= 0 then max(a, b) >= a >= 0.
        if (nonNegValues_.count(a) || nonNegValues_.count(b))
            nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::SIGN) {
        validateArgCount(expr, "sign", 1);
        // Constant-fold sign(literal).
        if (auto cv = getConstantInt(expr->arguments[0].get())) {
            int64_t result = (*cv > 0) ? 1 : ((*cv < 0) ? -1 : 0);
            return llvm::ConstantInt::get(getDefaultType(), result, true);
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

    if (bid == BuiltinId::CLAMP) {
        validateArgCount(expr, "clamp", 3);
        // Constant-fold clamp(val, lo, hi) when all three are integer literals.
        if (auto cv = getConstantInt(expr->arguments[0].get())) {
            if (auto cl = getConstantInt(expr->arguments[1].get())) {
                if (auto ch = getConstantInt(expr->arguments[2].get())) {
                    int64_t result = std::max(*cl, std::min(*cv, *ch));
                    auto* c = llvm::ConstantInt::get(getDefaultType(), result, true);
                    if (result >= 0) nonNegValues_.insert(c);
                    return c;
                }
            }
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
            llvm::Function* fminIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::minnum, {getFloatType()});
            llvm::Function* fmaxIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::maxnum, {getFloatType()});
            llvm::Value* minVH = builder->CreateCall(fminIntrinsic, {val, hi}, "fclampmin");
            return builder->CreateCall(fmaxIntrinsic, {minVH, lo}, "fclampval");
        }
        llvm::Function* sminIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smin, {getDefaultType()});
        llvm::Function* smaxIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smax, {getDefaultType()});
        llvm::Value* minVH = builder->CreateCall(sminIntrinsic, {val, hi}, "clampmin");
        auto* result = builder->CreateCall(smaxIntrinsic, {minVH, lo}, "clampval");
        // clamp(val, lo, hi) = max(lo, min(val, hi)) is non-negative when
        // the lower bound is non-negative (result >= lo >= 0).
        if (nonNegValues_.count(lo))
            nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::POW) {
        validateArgCount(expr, "pow", 2);
        // Constant-fold pow(base, exp) when both are integer literals and exp >= 0.
        // Eliminates the entire exponentiation loop at compile time.
        if (auto cb = getConstantInt(expr->arguments[0].get())) {
            if (auto ce = getConstantInt(expr->arguments[1].get())) {
                int64_t b = *cb, e = *ce;
                if (e >= 0) {
                    int64_t result = 1;
                    int64_t cur = b;
                    int64_t rem = e;
                    while (rem > 0) {
                        if (rem & 1) result *= cur;
                        cur *= cur;
                        rem >>= 1;
                    }
                    auto* c = llvm::ConstantInt::get(getDefaultType(), result, true);
                    if (result >= 0) nonNegValues_.insert(c);
                    return c;
                } else {
                    // Negative exponent in integer pow → 0 (matches runtime)
                    return llvm::ConstantInt::get(getDefaultType(), 0);
                }
            }
        }
        llvm::Value* base = generateExpression(expr->arguments[0].get());
        llvm::Value* exp  = generateExpression(expr->arguments[1].get());

        // If either argument is a float, delegate to the llvm.pow.f64 intrinsic.
        // This handles pow(2.0, 0.5) = sqrt(2), pow(x, n) for fractional n, etc.
        if (base->getType()->isDoubleTy() || exp->getType()->isDoubleTy()) {
            if (!base->getType()->isDoubleTy()) base = ensureFloat(base);
            if (!exp->getType()->isDoubleTy())  exp  = ensureFloat(exp);
            llvm::Function* powFn =
                OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::pow, {getFloatType()});
            return builder->CreateCall(powFn, {base, exp}, "pow.fresult");
        }

        // Integer path: convert to i64 and use binary exponentiation (O(log n)).
        base = toDefaultType(base);
        exp  = toDefaultType(exp);

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Binary exponentiation (exponentiation by squaring): O(log n) in the exponent
        llvm::BasicBlock* negExpBB = llvm::BasicBlock::Create(*context, "pow.negexp", function);
        llvm::BasicBlock* posExpBB = llvm::BasicBlock::Create(*context, "pow.posexp", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "pow.loop", function);
        llvm::BasicBlock* oddBB = llvm::BasicBlock::Create(*context, "pow.odd", function);
        llvm::BasicBlock* squareBB = llvm::BasicBlock::Create(*context, "pow.square", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "pow.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1, true);
        llvm::Value* isNegExp = builder->CreateICmpSLT(exp, zero, "pow.isneg");
        // Negative exponent is uncommon for integer pow(); favour the positive path.
        auto* negExpW = llvm::MDBuilder(*context).createBranchWeights(1, 100);
        builder->CreateCondBr(isNegExp, negExpBB, posExpBB, negExpW);

        // Negative exponent: return 0 (integer approximation of base^(-n))
        builder->SetInsertPoint(negExpBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(posExpBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // Loop: result *= base when exponent is odd; base *= base; exp >>= 1
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 3, "pow.result");
        llvm::PHINode* curBase = builder->CreatePHI(getDefaultType(), 3, "pow.base");
        llvm::PHINode* counter = builder->CreatePHI(getDefaultType(), 3, "pow.counter");
        result->addIncoming(one, posExpBB);
        curBase->addIncoming(base, posExpBB);
        counter->addIncoming(exp, posExpBB);

        llvm::Value* done = builder->CreateICmpSLE(counter, zero, "pow.done.cmp");
        builder->CreateCondBr(done, doneBB, oddBB);

        // Check if exponent is odd
        builder->SetInsertPoint(oddBB);
        llvm::Value* expBit = builder->CreateAnd(counter, one, "pow.bit");
        llvm::Value* isOdd = builder->CreateICmpNE(expBit, zero, "pow.isodd");
        llvm::Value* newResult = builder->CreateMul(result, curBase, "pow.mul");
        llvm::Value* resultSel = builder->CreateSelect(isOdd, newResult, result, "pow.rsel");
        builder->CreateBr(squareBB);

        // Square the base and halve the exponent
        builder->SetInsertPoint(squareBB);
        llvm::Value* newBase = builder->CreateMul(curBase, curBase, "pow.sq");
        llvm::Value* newCounter = builder->CreateAShr(counter, one, "pow.halve");
        result->addIncoming(resultSel, squareBB);
        curBase->addIncoming(newBase, squareBB);
        counter->addIncoming(newCounter, squareBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* finalResult = builder->CreatePHI(getDefaultType(), 2, "pow.final");
        finalResult->addIncoming(zero, negExpBB);
        finalResult->addIncoming(result, loopBB);
        return finalResult;
    }

    if (bid == BuiltinId::PRINT_CHAR) {
        validateArgCount(expr, "print_char", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        if (arg->getType()->isDoubleTy()) {
            arg = builder->CreateFPToSI(arg, getDefaultType(), "ftoi");
        }
        llvm::Value* charCode;
        if (arg->getType()->isPointerTy() || isStringExpr(expr->arguments[0].get())) {
            // Argument is a string (e.g. result of to_char): load the first byte.
            llvm::Value* ptr = arg->getType()->isPointerTy()
                                   ? arg
                                   : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "pc.strptr");
            llvm::Value* byte = builder->CreateLoad(llvm::Type::getInt8Ty(*context), ptr, "pc.byte");
            charCode = builder->CreateZExt(byte, llvm::Type::getInt32Ty(*context), "charval");
        } else {
            charCode = builder->CreateTrunc(arg, llvm::Type::getInt32Ty(*context), "charval");
        }
        builder->CreateCall(getOrDeclarePutchar(), {charCode});
        return arg; // return the original argument as documented
    }

    if (bid == BuiltinId::INPUT) {
        validateArgCount(expr, "input", 0);
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::AllocaInst* inputAlloca = createEntryBlockAlloca(function, "input_val");
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), inputAlloca);
        llvm::GlobalVariable* scanfFmt = module->getGlobalVariable("scanf_fmt", true);
        if (!scanfFmt) {
            scanfFmt = builder->CreateGlobalString("%lld", "scanf_fmt");
        }
        builder->CreateCall(getOrDeclareScanf(), {scanfFmt, inputAlloca});
        return builder->CreateAlignedLoad(getDefaultType(), inputAlloca, llvm::MaybeAlign(8), "input_read");
    }

    if (bid == BuiltinId::INPUT_LINE) {
        validateArgCount(expr, "input_line", 0);
        // Allocate a 1024-byte buffer
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 1024);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "inputln.buf");
        // Declare stdin as external global
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::GlobalVariable* stdinVar = module->getGlobalVariable("stdin");
        if (!stdinVar) {
            stdinVar =
                new llvm::GlobalVariable(*module, ptrTy, false, llvm::GlobalValue::ExternalLinkage, nullptr, "stdin");
        }
        llvm::Value* stdinVal = builder->CreateLoad(ptrTy, stdinVar, "inputln.stdin");
        // Call fgets(buf, 1024, stdin)
        llvm::Value* intSize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1024);
        llvm::Value* fgetsRet = builder->CreateCall(getOrDeclareFgets(), {buf, intSize, stdinVal}, "inputln.fgets");
        // If fgets returns NULL (EOF/error), store empty string in buffer
        llvm::Value* fgetsNull = builder->CreateICmpEQ(fgetsRet, llvm::ConstantPointerNull::get(ptrTy), "inputln.eof");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* eofBB = llvm::BasicBlock::Create(*context, "inputln.eof", function);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "inputln.ok", function);
        llvm::BasicBlock* stripBB = llvm::BasicBlock::Create(*context, "inputln.strip", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "inputln.done", function);
        // EOF/error on fgets is rare — favour the success path.
        auto* fgetsW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(fgetsNull, eofBB, okBB, fgetsW);
        // EOF path: store '\0' at start of buffer
        builder->SetInsertPoint(eofBB);
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), buf);
        builder->CreateBr(doneBB);
        // OK path: strip trailing newline
        builder->SetInsertPoint(okBB);
        llvm::Value* nlChar = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10);
        llvm::Value* nlPtr = builder->CreateCall(getOrDeclareStrchr(), {buf, nlChar}, "inputln.nl");
        llvm::Value* isNull = builder->CreateICmpEQ(nlPtr, llvm::ConstantPointerNull::get(ptrTy), "inputln.isnull");
        // Well-formed input lines almost always have a trailing newline.
        auto* nlW = llvm::MDBuilder(*context).createBranchWeights(1, 100);
        builder->CreateCondBr(isNull, doneBB, stripBB, nlW);
        builder->SetInsertPoint(stripBB);
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nlPtr);
        builder->CreateBr(doneBB);
        builder->SetInsertPoint(doneBB);
        stringReturningFunctions_.insert("input_line");
        return buf;
    }

    if (bid == BuiltinId::SQRT) {
        validateArgCount(expr, "sqrt", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Use the llvm.sqrt intrinsic which maps directly to hardware sqrtsd/sqrtss
        // instructions on x86, producing results in a single cycle on modern CPUs.
        bool inputIsDouble = x->getType()->isDoubleTy();
        llvm::Value* fval = ensureFloat(x);
        llvm::Function* sqrtIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sqrt, {getFloatType()});
        llvm::Value* result = builder->CreateCall(sqrtIntrinsic, {fval}, "sqrt.result");
        // Return double when input is double, truncate to integer only for integer input.
        if (inputIsDouble)
            return result;
        return builder->CreateFPToSI(result, getDefaultType(), "sqrt.int");
    }

    // -----------------------------------------------------------------------
    // Per-variable precision control builtins
    // -----------------------------------------------------------------------
    // fast_add / fast_sub / fast_mul / fast_div:
    //   Perform the FP operation with full -ffast-math flags (reassociation,
    //   reciprocal transforms, NaN/Inf assumptions, fused operations).
    //   Use in hot loops where numerical accuracy is less important.
    //
    // precise_add / precise_sub / precise_mul / precise_div:
    //   Perform the FP operation with strict IEEE-754 semantics — no
    //   reassociation, no NaN/Inf assumptions, no implicit fusing.
    //   Use when numerical stability is critical.
    //
    // Both families accept two arguments, convert them to double, perform
    // the operation, and return the result as an integer (consistent with
    // OmScript's convention of i64-encoded floats for interop).

    if (bid == BuiltinId::FAST_ADD || bid == BuiltinId::FAST_SUB ||
        bid == BuiltinId::FAST_MUL || bid == BuiltinId::FAST_DIV) {
        validateArgCount(expr, expr->callee, 2);
        llvm::Value* lhs = ensureFloat(generateExpression(expr->arguments[0].get()));
        llvm::Value* rhs = ensureFloat(generateExpression(expr->arguments[1].get()));
        llvm::Value* result = nullptr;
        if (bid == BuiltinId::FAST_ADD)
            result = builder->CreateFAdd(lhs, rhs, "fast.add");
        else if (bid == BuiltinId::FAST_SUB)
            result = builder->CreateFSub(lhs, rhs, "fast.sub");
        else if (bid == BuiltinId::FAST_MUL)
            result = builder->CreateFMul(lhs, rhs, "fast.mul");
        else
            result = builder->CreateFDiv(lhs, rhs, "fast.div");
        // Set all fast-math flags on the instruction.
        if (auto* fpInst = llvm::dyn_cast<llvm::Instruction>(result)) {
            fpInst->setFastMathFlags(llvm::FastMathFlags::getFast());
            hgoe::setInstructionPrecision(fpInst, hgoe::FPPrecision::Fast);
        }
        return builder->CreateFPToSI(result, getDefaultType(), "fast.result");
    }

    if (bid == BuiltinId::PRECISE_ADD || bid == BuiltinId::PRECISE_SUB ||
        bid == BuiltinId::PRECISE_MUL || bid == BuiltinId::PRECISE_DIV) {
        validateArgCount(expr, expr->callee, 2);
        llvm::Value* lhs = ensureFloat(generateExpression(expr->arguments[0].get()));
        llvm::Value* rhs = ensureFloat(generateExpression(expr->arguments[1].get()));
        llvm::Value* result = nullptr;
        if (bid == BuiltinId::PRECISE_ADD)
            result = builder->CreateFAdd(lhs, rhs, "precise.add");
        else if (bid == BuiltinId::PRECISE_SUB)
            result = builder->CreateFSub(lhs, rhs, "precise.sub");
        else if (bid == BuiltinId::PRECISE_MUL)
            result = builder->CreateFMul(lhs, rhs, "precise.mul");
        else
            result = builder->CreateFDiv(lhs, rhs, "precise.div");
        // Strict: explicitly clear all fast-math flags for IEEE compliance.
        if (auto* fpInst = llvm::dyn_cast<llvm::Instruction>(result)) {
            llvm::FastMathFlags strictFlags;
            // All flags default to off — full IEEE compliance.
            fpInst->setFastMathFlags(strictFlags);
            hgoe::setInstructionPrecision(fpInst, hgoe::FPPrecision::Strict);
        }
        return builder->CreateFPToSI(result, getDefaultType(), "precise.result");
    }

    if (bid == BuiltinId::IS_EVEN) {
        validateArgCount(expr, "is_even", 1);
        // Constant-fold is_even(literal).
        if (auto cv = getConstantInt(expr->arguments[0].get())) {
            int64_t result = ((*cv & 1) == 0) ? 1 : 0;
            auto* c = llvm::ConstantInt::get(getDefaultType(), result);
            nonNegValues_.insert(c);
            return c;
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_even() is an integer operation
        x = toDefaultType(x);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* bit = builder->CreateAnd(x, one, "evenbit");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isEven = builder->CreateICmpEQ(bit, zero, "iseven");
        auto* result = builder->CreateZExt(isEven, getDefaultType(), "evenval");
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::IS_ODD) {
        validateArgCount(expr, "is_odd", 1);
        // Constant-fold is_odd(literal).
        if (auto cv = getConstantInt(expr->arguments[0].get())) {
            int64_t result = (*cv & 1) ? 1 : 0;
            auto* c = llvm::ConstantInt::get(getDefaultType(), result);
            nonNegValues_.insert(c);
            return c;
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_odd() is an integer operation
        x = toDefaultType(x);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        auto* result = builder->CreateAnd(x, one, "oddval");
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::SUM) {
        validateArgCount(expr, "sum", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // Array layout: [length, elem0, elem1, ...]
        llvm::Value* arrPtr =
            arg->getType()->isPointerTy() ? arg : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "sum.arrptr");
        auto* sumLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "sum.len");
        sumLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        sumLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* length = sumLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "sum.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "sum.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "sum.done", function);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::PHINode* acc = builder->CreatePHI(getDefaultType(), 2, "sum.acc");
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "sum.idx");
        acc->addIncoming(zero, entryBB);
        idx->addIncoming(zero, entryBB);

        // Unsigned comparison: idx starts at 0 and length has !range [0, i64max],
        // so both are provably non-negative.  UGE gives SCEV a canonical unsigned
        // trip count which improves vectorizer cost modeling and IndVarSimplify.
        llvm::Value* done = builder->CreateICmpUGE(idx, length, "sum.done");
        auto* sumCondBr = builder->CreateCondBr(done, doneBB, bodyBB);
        if (optimizationLevel >= OptimizationLevel::O2) {
            sumCondBr->setMetadata(llvm::LLVMContext::MD_prof,
                llvm::MDBuilder(*context).createBranchWeights(1, 2000));
        }

        builder->SetInsertPoint(bodyBB);
        // Element is at offset (idx + 1) from array base
        llvm::Value* offset = builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 1), "sum.offset", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "sum.elemptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "sum.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* newAcc = builder->CreateAdd(acc, elem, "sum.newacc");
        llvm::Value* newIdx = builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 1), "sum.newidx", /*HasNUW=*/true, /*HasNSW=*/true);
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(newIdx, bodyBB);
        auto* backBr_674 = builder->CreateBr(loopBB);
        if (optimizationLevel >= OptimizationLevel::O1) {
            llvm::SmallVector<llvm::Metadata*, 4> mds;
            mds.push_back(nullptr);
            mds.push_back(llvm::MDNode::get(*context,
                {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
            // Vectorization hints: sum() is a classic reduction loop — enable
            // vectorization with interleave count 4 to exploit SIMD lanes and
            // hide memory latency through overlapping independent accumulation.
            if (optimizationLevel >= OptimizationLevel::O2) {
                mds.push_back(llvm::MDNode::get(*context,
                    {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
                mds.push_back(llvm::MDNode::get(*context,
                    {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 4))}));
            }
            llvm::MDNode* md = llvm::MDNode::get(*context, mds);
            md->replaceOperandWith(0, md);
            backBr_674->setMetadata(llvm::LLVMContext::MD_loop, md);
        }

        builder->SetInsertPoint(doneBB);
        return acc;
    }

    if (bid == BuiltinId::SWAP) {
        validateArgCount(expr, "swap", 3);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* i = generateExpression(expr->arguments[1].get());
        llvm::Value* j = generateExpression(expr->arguments[2].get());

        llvm::Value* arrPtr =
            arg->getType()->isPointerTy() ? arg : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "swap.arrptr");
        auto* swapLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "swap.len");
        swapLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        swapLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* length = swapLenLoad;

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
        // Swap OOB is extremely unlikely.
        llvm::MDNode* swapW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(bothValid, okBB, failBB, swapW);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: swap index out of bounds\n", "swap_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Elements are at offset (index + 1)
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* offI = builder->CreateAdd(i, one, "swap.offi", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* offJ = builder->CreateAdd(j, one, "swap.offj", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* ptrI = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offI, "swap.ptri");
        llvm::Value* ptrJ = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offJ, "swap.ptrj");
        llvm::Value* valI = builder->CreateAlignedLoad(getDefaultType(), ptrI, llvm::MaybeAlign(8), "swap.vali");
        llvm::Value* valJ = builder->CreateAlignedLoad(getDefaultType(), ptrJ, llvm::MaybeAlign(8), "swap.valj");
        llvm::cast<llvm::Instruction>(valI)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::cast<llvm::Instruction>(valJ)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        auto* stI = builder->CreateAlignedStore(valJ, ptrI, llvm::MaybeAlign(8));
        auto* stJ = builder->CreateAlignedStore(valI, ptrJ, llvm::MaybeAlign(8));
        stI->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        stJ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::REVERSE) {
        validateArgCount(expr, "reverse", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr =
            arg->getType()->isPointerTy() ? arg : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "rev.arrptr");
        auto* revLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "rev.len");
        revLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        revLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* length = revLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "rev.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "rev.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "rev.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* lastIdx = builder->CreateSub(length, one, "rev.last", /*HasNUW=*/true, /*HasNSW=*/true);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* lo = builder->CreatePHI(getDefaultType(), 2, "rev.lo");
        llvm::PHINode* hi = builder->CreatePHI(getDefaultType(), 2, "rev.hi");
        lo->addIncoming(zero, entryBB);
        hi->addIncoming(lastIdx, entryBB);

        llvm::Value* done = builder->CreateICmpSGE(lo, hi, "rev.done");
        builder->CreateCondBr(done, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* offLo = builder->CreateAdd(lo, one, "rev.offlo", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* offHi = builder->CreateAdd(hi, one, "rev.offhi", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* ptrLo = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offLo, "rev.ptrlo");
        llvm::Value* ptrHi = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offHi, "rev.ptrhi");
        llvm::Value* valLo = builder->CreateAlignedLoad(getDefaultType(), ptrLo, llvm::MaybeAlign(8), "rev.vallo");
        llvm::Value* valHi = builder->CreateAlignedLoad(getDefaultType(), ptrHi, llvm::MaybeAlign(8), "rev.valhi");
        llvm::cast<llvm::Instruction>(valLo)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::cast<llvm::Instruction>(valHi)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        auto* stLo = builder->CreateAlignedStore(valHi, ptrLo, llvm::MaybeAlign(8));
        auto* stHi = builder->CreateAlignedStore(valLo, ptrHi, llvm::MaybeAlign(8));
        stLo->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        stHi->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* newLo = builder->CreateAdd(lo, one, "rev.newlo", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* newHi = builder->CreateSub(hi, one, "rev.newhi", /*HasNUW=*/true, /*HasNSW=*/true);
        lo->addIncoming(newLo, bodyBB);
        hi->addIncoming(newHi, bodyBB);
        auto* backBr_769 = builder->CreateBr(loopBB);
        if (optimizationLevel >= OptimizationLevel::O1) {
            llvm::SmallVector<llvm::Metadata*, 4> mds;
            mds.push_back(nullptr);
            mds.push_back(llvm::MDNode::get(*context,
                {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
            // Vectorization hints: reverse() swaps elements from opposite
            // ends of the array — enable vectorization to process multiple
            // swaps per iteration using SIMD gather/scatter operations.
            if (optimizationLevel >= OptimizationLevel::O2) {
                mds.push_back(llvm::MDNode::get(*context,
                    {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
            }
            llvm::MDNode* md = llvm::MDNode::get(*context, mds);
            md->replaceOperandWith(0, md);
            backBr_769->setMetadata(llvm::LLVMContext::MD_loop, md);
        }

        builder->SetInsertPoint(doneBB);
        return arg;
    }

    if (bid == BuiltinId::TO_CHAR) {
        validateArgCount(expr, "to_char", 1);
        // Allocate a 2-byte buffer [char, '\0'] and return a pointer to it,
        // so the result behaves like a one-character string.
        llvm::Value* code = generateExpression(expr->arguments[0].get());
        code = toDefaultType(code);  // ensure i64
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {two}, "tochar.buf");
        llvm::Value* byteVal = builder->CreateTrunc(code, llvm::Type::getInt8Ty(*context), "tochar.byte");
        builder->CreateStore(byteVal, buf);
        llvm::Value* nulPtr = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), buf,
            llvm::ConstantInt::get(getDefaultType(), 1), "tochar.nul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nulPtr);
        stringReturningFunctions_.insert("to_char");
        return buf;
    }

    if (bid == BuiltinId::IS_ALPHA) {
        validateArgCount(expr, "is_alpha", 1);
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

    if (bid == BuiltinId::IS_DIGIT) {
        validateArgCount(expr, "is_digit", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_digit() is an integer operation
        x = toDefaultType(x);
        // is_digit: x >= '0' && x <= '9'
        llvm::Value* ge0 = builder->CreateICmpSGE(x, llvm::ConstantInt::get(getDefaultType(), 48), "ge.0");
        llvm::Value* le9 = builder->CreateICmpSLE(x, llvm::ConstantInt::get(getDefaultType(), 57), "le.9");
        llvm::Value* isDigit = builder->CreateAnd(ge0, le9, "isdigit");
        return builder->CreateZExt(isDigit, getDefaultType(), "digitval");
    }

    // typeof(x) returns a type tag: 1=integer, 2=float, 3=string.
    // The tag is determined from the LLVM IR type of the expression, which is
    // known statically for literals and for variables whose alloca type was
    // set at declaration time.  Integer variables stored as i64 and arrays
    // (also i64 pointers) both return 1; floats stored as double return 2;
    // string literals and tracked string variables return 3.
    if (bid == BuiltinId::TYPEOF) {
        validateArgCount(expr, "typeof", 1);
        // Evaluate the argument for its side effects, then derive type tag.
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        long long tag;
        if (arg->getType()->isDoubleTy()) {
            tag = 2; // float
        } else if (arg->getType()->isPointerTy() || isStringExpr(expr->arguments[0].get())) {
            tag = 3; // string
        } else {
            tag = 1; // integer (default for all i64 values including arrays)
        }
        (void)arg;
        return llvm::ConstantInt::get(getDefaultType(), tag);
    }

    // assert(condition) — aborts with an error if the condition is falsy.
    if (bid == BuiltinId::ASSERT) {
        validateArgCount(expr, "assert", 1);
        llvm::Value* condVal = generateExpression(expr->arguments[0].get());
        condVal = toBool(condVal);

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "assert.fail", function);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "assert.ok", function);

        // Assertions are expected to pass.
        llvm::MDNode* assertW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(condVal, okBB, failBB, assertW);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: assertion failed\n", "assert_fail_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        return llvm::ConstantInt::get(getDefaultType(), 1);
    }

    if (bid == BuiltinId::STR_LEN) {
        validateArgCount(expr, "str_len", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // String may be a raw pointer (from a literal/local) or an i64 holding a pointer.
        llvm::Value* strPtr = arg->getType()->isPointerTy()
                                  ? arg
                                  : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "strlen.ptr");
        auto* result = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strlen.result");
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::CHAR_AT) {
        validateArgCount(expr, "char_at", 2);

        // ── Compile-time char_at folding ────────────────────────────
        // When both the string and index are compile-time constants,
        // fold char_at("hello", 1) → 'e' (ASCII 101) at compile time.
        // This eliminates the runtime strlen + bounds check + load entirely.
        if (auto* strLit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (strLit->literalType == LiteralExpr::LiteralType::STRING) {
                if (auto* idxLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get())) {
                    if (idxLit->literalType == LiteralExpr::LiteralType::INTEGER) {
                        int64_t idx = idxLit->intValue;
                        int64_t len = static_cast<int64_t>(strLit->stringValue.size());
                        if (idx >= 0 && idx < len) {
                            // Valid index — fold to the character value.
                            char ch = strLit->stringValue[static_cast<size_t>(idx)];
                            return llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(static_cast<unsigned char>(ch)));
                        }
                        // Out-of-bounds index with literals → still emit runtime
                        // error path (don't silently UB).
                    }
                }
            }
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
        // char_at OOB is extremely unlikely.
        llvm::MDNode* charAtW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(valid, okBB, failBB, charAtW);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg =
            builder->CreateGlobalString("Runtime error: char_at index out of bounds\n", "charat_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Load char via GEP
        llvm::Value* charPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, idxArg, "charat.gep");
        auto* charVal = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "charat.char");
        charVal->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        // Zero-extend to i64
        return builder->CreateZExt(charVal, getDefaultType(), "charat.ext");
    }

    if (bid == BuiltinId::STR_EQ) {
        validateArgCount(expr, "str_eq", 2);
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

    if (bid == BuiltinId::STR_CONCAT) {
        validateArgCount(expr, "str_concat", 2);
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

        // Optimization: use a length-cache alloca for the LHS string variable.
        // On the first call, we compute strlen and store the result; on
        // subsequent loop iterations (at runtime), the cached value is loaded
        // instead of calling strlen again.  This turns O(n²) append loops
        // into amortized O(n).
        std::string lhsVarName;
        llvm::AllocaInst* lenCacheAlloca = nullptr;
        if (expr->arguments[0]->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto* id = static_cast<IdentifierExpr*>(expr->arguments[0].get());
            lhsVarName = id->name;
            auto cacheIt = stringLenCache_.find(lhsVarName);
            if (cacheIt != stringLenCache_.end()) {
                lenCacheAlloca = cacheIt->second;
            } else {
                // Lazily create the cache alloca in the entry block.
                llvm::Function* fn = builder->GetInsertBlock()->getParent();
                lenCacheAlloca = createEntryBlockAlloca(fn, lhsVarName + ".strlen");
                // Initialize with -1 sentinel (meaning "not yet computed").
                // Insert the store right after the alloca in the entry block.
                llvm::IRBuilder<> entryBuilder(lenCacheAlloca->getNextNode());
                entryBuilder.CreateStore(
                    llvm::ConstantInt::get(getDefaultType(), -1, true), lenCacheAlloca);
                stringLenCache_[lhsVarName] = lenCacheAlloca;
            }
        }

        llvm::Value* len1;
        if (lenCacheAlloca) {
            // Load cached length; if -1 (sentinel), fall back to strlen.
            llvm::Value* cachedLen = builder->CreateAlignedLoad(getDefaultType(), lenCacheAlloca, llvm::MaybeAlign(8), "concat.cachedlen1");
            llvm::Value* isSentinel = builder->CreateICmpEQ(
                cachedLen, llvm::ConstantInt::get(getDefaultType(), -1, true), "concat.issent");
            llvm::Function* fn = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock* cachedBB = builder->GetInsertBlock();
            llvm::BasicBlock* strlenBB = llvm::BasicBlock::Create(*context, "concat.strlen", fn);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "concat.merge", fn);
            // Cache hit is the common case after first concat — favour merge.
            auto* sentW = llvm::MDBuilder(*context).createBranchWeights(1, 9);
            builder->CreateCondBr(isSentinel, strlenBB, mergeBB, sentW);

            builder->SetInsertPoint(strlenBB);
            llvm::Value* realLen = builder->CreateCall(getOrDeclareStrlen(), {lhsPtr}, "concat.len1.real");
            builder->CreateBr(mergeBB);

            builder->SetInsertPoint(mergeBB);
            llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "concat.len1");
            phi->addIncoming(cachedLen, cachedBB);
            phi->addIncoming(realLen, strlenBB);
            len1 = phi;
        } else {
            len1 = builder->CreateCall(getOrDeclareStrlen(), {lhsPtr}, "concat.len1");
        }
        llvm::Value* len2 = builder->CreateCall(getOrDeclareStrlen(), {rhsPtr}, "concat.len2");
        llvm::Value* totalLen = builder->CreateAdd(len1, len2, "concat.totallen", /*HasNUW=*/true, /*HasNSW=*/true);

        // Update the string length cache for the LHS variable.
        if (lenCacheAlloca) {
            builder->CreateStore(totalLen, lenCacheAlloca);
        }

        // Determine whether the LHS came from a heap allocation (variable load
        // or call return) vs a string literal in read-only memory.  If heap,
        // we can realloc in-place with capacity tracking to avoid calling
        // realloc on every append.
        bool lhsIsHeap = false;
        llvm::AllocaInst* capCacheAlloca = nullptr;
        if (expr->arguments[0]->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto* id = static_cast<IdentifierExpr*>(expr->arguments[0].get());
            if (stringVars_.count(id->name)) {
                lhsIsHeap = true;
                // Look up or create a capacity cache alloca for this variable.
                auto capIt = stringCapCache_.find(id->name);
                if (capIt != stringCapCache_.end()) {
                    capCacheAlloca = capIt->second;
                } else {
                    llvm::Function* fn = builder->GetInsertBlock()->getParent();
                    capCacheAlloca = createEntryBlockAlloca(fn, id->name + ".strcap");
                    // Initialize capacity to 0 (forces first realloc).
                    llvm::IRBuilder<> entryBuilder(capCacheAlloca->getNextNode());
                    entryBuilder.CreateStore(
                        llvm::ConstantInt::get(getDefaultType(), 0), capCacheAlloca);
                    stringCapCache_[id->name] = capCacheAlloca;
                }
            }
        } else if (expr->arguments[0]->type == ASTNodeType::CALL_EXPR) {
            lhsIsHeap = true;
        }
        llvm::Value* buf;
        if (lhsIsHeap && capCacheAlloca) {
            // Capacity-tracked path: only realloc when buffer is too small.
            // This matches the C pattern: if (len+1 > cap) { cap*=2; realloc; }
            llvm::Value* needed =
                builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "concat.needed", /*HasNUW=*/true, /*HasNSW=*/true);
            llvm::Value* curCap = builder->CreateAlignedLoad(getDefaultType(), capCacheAlloca, llvm::MaybeAlign(8), "concat.curcap");
            llvm::Value* needGrow = builder->CreateICmpUGT(needed, curCap, "concat.needgrow");

            llvm::Function* fn = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock* growBB = llvm::BasicBlock::Create(*context, "concat.grow", fn);
            llvm::BasicBlock* appendBB = llvm::BasicBlock::Create(*context, "concat.append", fn);
            llvm::BasicBlock* curBB = builder->GetInsertBlock();
            // Growth is rare in append loops — weight accordingly for branch
            // predictor and code layout (grow path moved out of line).
            llvm::MDNode* concatGrowWeights = llvm::MDBuilder(*context).createBranchWeights(1, 100);
            builder->CreateCondBr(needGrow, growBB, appendBB, concatGrowWeights);

            // Grow path: double capacity until sufficient, then realloc.
            builder->SetInsertPoint(growBB);
            // newCap = max(curCap * 2, needed); but at least 16
            llvm::Value* doubled = builder->CreateShl(curCap, llvm::ConstantInt::get(getDefaultType(), 1), "concat.doubled");
            llvm::Value* sixteen = llvm::ConstantInt::get(getDefaultType(), 16);
            llvm::Value* minCap = builder->CreateSelect(
                builder->CreateICmpUGT(doubled, sixteen), doubled, sixteen, "concat.mincap");
            llvm::Value* newCap = builder->CreateSelect(
                builder->CreateICmpUGT(needed, minCap), needed, minCap, "concat.newcap");
            builder->CreateStore(newCap, capCacheAlloca);
            llvm::Value* grownBuf = builder->CreateCall(getOrDeclareRealloc(), {lhsPtr, newCap}, "concat.grown");
            llvm::BasicBlock* growExitBB = builder->GetInsertBlock();
            builder->CreateBr(appendBB);

            // Append path: merge buffer pointer from grow / no-grow paths.
            builder->SetInsertPoint(appendBB);
            llvm::PHINode* bufPhi = builder->CreatePHI(lhsPtr->getType(), 2, "concat.buf");
            bufPhi->addIncoming(lhsPtr, curBB);
            bufPhi->addIncoming(grownBuf, growExitBB);
            buf = bufPhi;

            // memcpy(buf + len1, rhs, len2) — only append the RHS portion
            llvm::Value* dst2 = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, len1, "concat.dst2");
            builder->CreateCall(getOrDeclareMemcpy(), {dst2, rhsPtr, len2});
        } else if (lhsIsHeap) {
            // Heap LHS without capacity tracking: use power-of-2 realloc.
            llvm::Value* minSize =
                builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "concat.minsize", /*HasNUW=*/true, /*HasNSW=*/true);
            llvm::Value* one64 = llvm::ConstantInt::get(getDefaultType(), 1);
            llvm::Value* v = builder->CreateSub(minSize, one64, "concat.pm1", /*HasNUW=*/true, /*HasNSW=*/true);
            v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 1)));
            v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 2)));
            v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 4)));
            v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 8)));
            v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 16)));
            v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 32)));
            llvm::Value* allocSize = builder->CreateAdd(v, one64, "concat.allocsize", /*HasNUW=*/true, /*HasNSW=*/true);
            buf = builder->CreateCall(getOrDeclareRealloc(), {lhsPtr, allocSize}, "concat.buf");
            // memcpy(buf + len1, rhs, len2) — only append the RHS portion
            llvm::Value* dst2 = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, len1, "concat.dst2");
            builder->CreateCall(getOrDeclareMemcpy(), {dst2, rhsPtr, len2});
        } else {
            llvm::Value* allocSize =
                builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "concat.allocsize", /*HasNUW=*/true, /*HasNSW=*/true);
            buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "concat.buf");
            // memcpy(buf, lhs, len1) — copy LHS into the new buffer
            builder->CreateCall(getOrDeclareMemcpy(), {buf, lhsPtr, len1});
            // memcpy(buf + len1, rhs, len2) — append RHS
            llvm::Value* dst2 = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, len1, "concat.dst2");
            builder->CreateCall(getOrDeclareMemcpy(), {dst2, rhsPtr, len2});
        }
        // null-terminate: buf[totalLen] = '\0'
        llvm::Value* endPtr = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, totalLen, "concat.end");
        builder->CreateStore(builder->getInt8(0), endPtr);
        // Mark return as string-returning so callers can track it
        stringReturningFunctions_.insert("str_concat");
        return buf;
    }

    if (bid == BuiltinId::LOG2) {
        validateArgCount(expr, "log2", 1);
        llvm::Value* n = generateExpression(expr->arguments[0].get());
        n = toDefaultType(n);
        // Integer log2 via CTZ intrinsic: 63 - clz(n).
        // Uses the llvm.ctlz intrinsic which maps directly to the BSR/LZCNT
        // hardware instruction on x86, producing the result in a single cycle.
        // Returns -1 for n <= 0.
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        llvm::Value* bits = llvm::ConstantInt::get(getDefaultType(), 63);

        // n <= 0 → return -1
        llvm::Value* isPositive = builder->CreateICmpSGT(n, zero, "log2.pos");

        // clz(n) returns number of leading zeros; log2(n) = 63 - clz(n)
        llvm::Function* ctlzIntrinsic = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::ctlz, {getDefaultType()});
        // is_zero_poison=true since we guard with isPositive
        llvm::Value* clz = builder->CreateCall(
            ctlzIntrinsic, {n, builder->getTrue()}, "log2.clz");
        llvm::Value* log2val = builder->CreateSub(bits, clz, "log2.val");

        return builder->CreateSelect(isPositive, log2val, negOne, "log2.result");
    }

    if (bid == BuiltinId::GCD) {
        validateArgCount(expr, "gcd", 2);
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

        // Binary GCD (Stein's algorithm): avoids expensive division by using
        // only ctz, shifts, comparisons, and subtraction.  On modern x86 CPUs
        // division takes 20-40 cycles while ctz/shift/sub are all 1-cycle ops,
        // making binary GCD ~5x faster for typical integer inputs.
        //
        // Algorithm:
        //   if a == 0: return b;  if b == 0: return a
        //   k = ctz(a | b)        // common factor of 2
        //   a >>= ctz(a)          // make a odd
        //   loop:
        //     b >>= ctz(b)        // make b odd
        //     if a > b: swap(a,b)
        //     b -= a
        //   while b != 0
        //   return a << k
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::Function* cttzFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::cttz, {getDefaultType()});

        // Compute k = ctz(a | b) in the entry block (always dominates doneBB).
        // For the edge case where a==0 or b==0, k is unused so its value
        // doesn't matter, but it must dominate all uses.
        llvm::Value* aOrB = builder->CreateOr(a, b, "gcd.aorb");
        llvm::Value* k = builder->CreateCall(cttzFn, {aOrB, builder->getFalse()}, "gcd.k");

        llvm::BasicBlock* mainBB = llvm::BasicBlock::Create(*context, "gcd.main", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "gcd.loop", function);
        llvm::BasicBlock* contBB = llvm::BasicBlock::Create(*context, "gcd.cont", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "gcd.done", function);

        // Edge case: if a == 0 return b, if b == 0 return a.
        // Combined: if (a == 0 || b == 0) return a | b.
        llvm::Value* edgeCase = builder->CreateOr(
            builder->CreateICmpEQ(a, zero, "gcd.a0"),
            builder->CreateICmpEQ(b, zero, "gcd.b0"), "gcd.edge");
        // a==0 or b==0 is a degenerate edge case; favour the main loop.
        auto* gcdEdgeW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(edgeCase, doneBB, mainBB, gcdEdgeW);

        // Main: make a odd
        builder->SetInsertPoint(mainBB);
        llvm::Value* ctzA = builder->CreateCall(cttzFn, {a, builder->getTrue()}, "gcd.ctza");
        llvm::Value* aOdd = builder->CreateLShr(a, ctzA, "gcd.aodd");
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // Loop header
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* phiA = builder->CreatePHI(getDefaultType(), 2, "gcd.pa");
        phiA->addIncoming(aOdd, mainBB);
        llvm::PHINode* phiB = builder->CreatePHI(getDefaultType(), 2, "gcd.pb");
        phiB->addIncoming(b, mainBB);

        // Make b odd: b >>= ctz(b)
        llvm::Value* ctzB = builder->CreateCall(cttzFn, {phiB, builder->getTrue()}, "gcd.ctzb");
        llvm::Value* bOdd = builder->CreateLShr(phiB, ctzB, "gcd.bodd");

        // Branchless min/max + subtract: equivalent to if(a>b) swap(a,b); b-=a
        llvm::Value* aGtB = builder->CreateICmpUGT(phiA, bOdd, "gcd.gt");
        llvm::Value* lo = builder->CreateSelect(aGtB, bOdd, phiA, "gcd.lo");
        llvm::Value* hi = builder->CreateSelect(aGtB, phiA, bOdd, "gcd.hi");
        llvm::Value* diff = builder->CreateSub(hi, lo, "gcd.diff");

        // Continue if diff != 0
        llvm::Value* done = builder->CreateICmpEQ(diff, zero, "gcd.dz");
        phiA->addIncoming(lo, loopBB);
        phiB->addIncoming(diff, loopBB);
        builder->CreateCondBr(done, contBB, loopBB);

        // Multiply result by 2^k (common factor)
        builder->SetInsertPoint(contBB);
        llvm::Value* shifted = builder->CreateShl(lo, k, "gcd.shifted");
        builder->CreateBr(doneBB);

        // Final merge
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "gcd.result");
        result->addIncoming(aOrB, entryBB);     // edge case: a|b = max(a,b) when one is 0
        result->addIncoming(shifted, contBB);   // normal case
        // GCD is always non-negative: the algorithm takes abs() of both inputs.
        // This enables nsw on downstream add/mul operations with the result.
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::TO_STRING) {
        validateArgCount(expr, "to_string", 1);
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        bool isFloat = val->getType()->isDoubleTy();
        if (!isFloat)
            val = toDefaultType(val);
        if (isFloat) {
            // Float: use a 32-byte buffer and %g format to preserve decimal places.
            llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 32);
            llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "tostr.buf");
            llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_float_fmt", true);
            if (!fmtStr)
                fmtStr = builder->CreateGlobalString("%g", "tostr_float_fmt");
            builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
            stringReturningFunctions_.insert("to_string");
            return buf;
        }
        // Integer: 21 bytes is enough for any 64-bit signed decimal plus null terminator.
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

    if (bid == BuiltinId::STR_FIND) {
        validateArgCount(expr, "str_find", 2);
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

    // -----------------------------------------------------------------------
    // Math built-ins: floor, ceil, round
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::FLOOR) {
        validateArgCount(expr, "floor", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        bool inputIsDouble = arg->getType()->isDoubleTy();
        llvm::Value* fval = ensureFloat(arg);
        // Use llvm.floor intrinsic for native hardware rounding
        llvm::Function* floorIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::floor, {getFloatType()});
        llvm::Value* result = builder->CreateCall(floorIntrinsic, {fval}, "floor.result");
        if (inputIsDouble)
            return result;
        return builder->CreateFPToSI(result, getDefaultType(), "floor.int");
    }

    if (bid == BuiltinId::CEIL) {
        validateArgCount(expr, "ceil", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        bool inputIsDouble = arg->getType()->isDoubleTy();
        llvm::Value* fval = ensureFloat(arg);
        // Use llvm.ceil intrinsic for native hardware rounding
        llvm::Function* ceilIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ceil, {getFloatType()});
        llvm::Value* result = builder->CreateCall(ceilIntrinsic, {fval}, "ceil.result");
        if (inputIsDouble)
            return result;
        return builder->CreateFPToSI(result, getDefaultType(), "ceil.int");
    }

    if (bid == BuiltinId::ROUND) {
        validateArgCount(expr, "round", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        bool inputIsDouble = arg->getType()->isDoubleTy();
        llvm::Value* fval = ensureFloat(arg);
        // Use llvm.round intrinsic for native hardware rounding
        llvm::Function* roundIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::round, {getFloatType()});
        llvm::Value* result = builder->CreateCall(roundIntrinsic, {fval}, "round.result");
        if (inputIsDouble)
            return result;
        return builder->CreateFPToSI(result, getDefaultType(), "round.int");
    }

    // -----------------------------------------------------------------------
    // Trigonometric and transcendental math built-ins
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::SIN) {
        validateArgCount(expr, "sin", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::Function* sinIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sin, {getFloatType()});
        return builder->CreateCall(sinIntrinsic, {fval}, "sin.result");
    }

    if (bid == BuiltinId::COS) {
        validateArgCount(expr, "cos", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::Function* cosIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::cos, {getFloatType()});
        return builder->CreateCall(cosIntrinsic, {fval}, "cos.result");
    }

    if (bid == BuiltinId::TAN) {
        validateArgCount(expr, "tan", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
        llvm::FunctionCallee tanFn = module->getOrInsertFunction("tan", ft);
        return builder->CreateCall(tanFn, {fval}, "tan.result");
    }

    if (bid == BuiltinId::ASIN) {
        validateArgCount(expr, "asin", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
        llvm::FunctionCallee asinFn = module->getOrInsertFunction("asin", ft);
        return builder->CreateCall(asinFn, {fval}, "asin.result");
    }

    if (bid == BuiltinId::ACOS) {
        validateArgCount(expr, "acos", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
        llvm::FunctionCallee acosFn = module->getOrInsertFunction("acos", ft);
        return builder->CreateCall(acosFn, {fval}, "acos.result");
    }

    if (bid == BuiltinId::ATAN) {
        validateArgCount(expr, "atan", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
        llvm::FunctionCallee atanFn = module->getOrInsertFunction("atan", ft);
        return builder->CreateCall(atanFn, {fval}, "atan.result");
    }

    if (bid == BuiltinId::ATAN2) {
        validateArgCount(expr, "atan2", 2);
        llvm::Value* y = generateExpression(expr->arguments[0].get());
        llvm::Value* x = generateExpression(expr->arguments[1].get());
        llvm::Value* fy = ensureFloat(y);
        llvm::Value* fx = ensureFloat(x);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType(), getFloatType()}, false);
        llvm::FunctionCallee atan2Fn = module->getOrInsertFunction("atan2", ft);
        return builder->CreateCall(atan2Fn, {fy, fx}, "atan2.result");
    }

    if (bid == BuiltinId::EXP) {
        validateArgCount(expr, "exp", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::Function* expIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::exp, {getFloatType()});
        return builder->CreateCall(expIntrinsic, {fval}, "exp.result");
    }

    if (bid == BuiltinId::LOG) {
        validateArgCount(expr, "log", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::Function* logIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::log, {getFloatType()});
        return builder->CreateCall(logIntrinsic, {fval}, "log.result");
    }

    if (bid == BuiltinId::LOG10) {
        validateArgCount(expr, "log10", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::Function* log10Intrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::log10, {getFloatType()});
        return builder->CreateCall(log10Intrinsic, {fval}, "log10.result");
    }

    if (bid == BuiltinId::CBRT) {
        validateArgCount(expr, "cbrt", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
        llvm::FunctionCallee cbrtFn = module->getOrInsertFunction("cbrt", ft);
        return builder->CreateCall(cbrtFn, {fval}, "cbrt.result");
    }

    if (bid == BuiltinId::HYPOT) {
        validateArgCount(expr, "hypot", 2);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        llvm::Value* y = generateExpression(expr->arguments[1].get());
        llvm::Value* fx = ensureFloat(x);
        llvm::Value* fy = ensureFloat(y);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType(), getFloatType()}, false);
        llvm::FunctionCallee hypotFn = module->getOrInsertFunction("hypot", ft);
        return builder->CreateCall(hypotFn, {fx, fy}, "hypot.result");
    }

    // -----------------------------------------------------------------------
    // Type conversion built-ins: to_int, to_float
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::TO_INT) {
        validateArgCount(expr, "to_int", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        if (arg->getType()->isDoubleTy()) {
            return builder->CreateFPToSI(arg, getDefaultType(), "toint.ftoi");
        }
        // If the argument is a string, parse it with strtoll.
        if (arg->getType()->isPointerTy() || isStringExpr(expr->arguments[0].get())) {
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            llvm::Value* strPtr = arg->getType()->isPointerTy()
                                      ? arg
                                      : builder->CreateIntToPtr(arg, ptrTy, "toint.strptr");
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            auto* base10 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10);
            return builder->CreateCall(getOrDeclareStrtoll(), {strPtr, nullPtr, base10}, "toint.parsed");
        }
        return toDefaultType(arg);
    }

    if (bid == BuiltinId::TO_FLOAT) {
        validateArgCount(expr, "to_float", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // If the argument is a string, parse it with strtod.
        if (arg->getType()->isPointerTy() || isStringExpr(expr->arguments[0].get())) {
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            llvm::Value* strPtr = arg->getType()->isPointerTy()
                                      ? arg
                                      : builder->CreateIntToPtr(arg, ptrTy, "tofloat.strptr");
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            return builder->CreateCall(getOrDeclareStrtod(), {strPtr, nullPtr}, "tofloat.parsed");
        }
        return ensureFloat(arg);
    }

    // -----------------------------------------------------------------------
    // String built-ins: str_substr, str_upper, str_lower, str_contains,
    //   str_replace, str_trim, str_starts_with, str_ends_with,
    //   str_index_of, str_repeat, str_reverse
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::STR_SUBSTR) {
        validateArgCount(expr, "str_substr", 3);

        // ── Compile-time str_substr folding ─────────────────────────
        // When all three arguments are compile-time constants, fold the
        // substr to a string literal.  This eliminates malloc + memcpy.
        if (auto* strLit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (strLit->literalType == LiteralExpr::LiteralType::STRING) {
                if (auto* startLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get())) {
                    if (startLit->literalType == LiteralExpr::LiteralType::INTEGER) {
                        if (auto* lenLit = dynamic_cast<LiteralExpr*>(expr->arguments[2].get())) {
                            if (lenLit->literalType == LiteralExpr::LiteralType::INTEGER) {
                                const auto& s = strLit->stringValue;
                                int64_t slen = static_cast<int64_t>(s.size());
                                int64_t startVal = static_cast<int64_t>(startLit->intValue);
                                int64_t lenVal = static_cast<int64_t>(lenLit->intValue);
                                if (startVal < 0) startVal = 0;
                                if (startVal > slen) startVal = slen;
                                int64_t maxLen = slen - startVal;
                                if (lenVal < 0) lenVal = 0;
                                if (lenVal > maxLen) lenVal = maxLen;
                                std::string result = s.substr(static_cast<size_t>(startVal), static_cast<size_t>(lenVal));
                                llvm::GlobalVariable* gv = internString(result);
                                return llvm::ConstantExpr::getInBoundsGetElementPtr(
                                    gv->getValueType(),
                                    gv,
                                    llvm::ArrayRef<llvm::Constant*>{
                                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)
                                    });
                            }
                        }
                    }
                }
            }
        }

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* startArg = generateExpression(expr->arguments[1].get());
        llvm::Value* lenArg = generateExpression(expr->arguments[2].get());
        startArg = toDefaultType(startArg);
        lenArg = toDefaultType(lenArg);
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "substr.ptr");

        // Bounds checking: clamp start and length to valid range.
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "substr.strlen");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        // Clamp start: max(0, min(start, strLen))
        llvm::Value* startNeg = builder->CreateICmpSLT(startArg, zero, "substr.startneg");
        startArg = builder->CreateSelect(startNeg, zero, startArg, "substr.startclamp");
        llvm::Value* startOverflow = builder->CreateICmpSGT(startArg, strLen, "substr.startover");
        startArg = builder->CreateSelect(startOverflow, strLen, startArg, "substr.startfinal");
        // Clamp length: max(0, min(len, strLen - start))
        llvm::Value* remaining = builder->CreateSub(strLen, startArg, "substr.remaining");
        llvm::Value* lenNeg = builder->CreateICmpSLT(lenArg, zero, "substr.lenneg");
        lenArg = builder->CreateSelect(lenNeg, zero, lenArg, "substr.lenclamp");
        llvm::Value* lenOverflow = builder->CreateICmpSGT(lenArg, remaining, "substr.lenover");
        lenArg = builder->CreateSelect(lenOverflow, remaining, lenArg, "substr.lenfinal");

        // Allocate buffer: len + 1
        llvm::Value* allocSize =
            builder->CreateAdd(lenArg, llvm::ConstantInt::get(getDefaultType(), 1), "substr.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "substr.buf");
        // memcpy(buf, strPtr + start, len)
        llvm::Value* srcPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, startArg, "substr.src");
        builder->CreateCall(getOrDeclareMemcpy(), {buf, srcPtr, lenArg});
        // Null-terminate: buf[len] = 0
        llvm::Value* endPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, lenArg, "substr.end");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), endPtr);
        stringReturningFunctions_.insert("str_substr");
        return buf;
    }

    if (bid == BuiltinId::STR_UPPER) {
        validateArgCount(expr, "str_upper", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "upper.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "upper.len");
        llvm::Value* allocSize = builder->CreateAdd(strLen, llvm::ConstantInt::get(getDefaultType(), 1), "upper.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "upper.buf");
        // Copy string then transform each character in a loop
        builder->CreateCall(getOrDeclareStrcpy(), {buf, strPtr});
        // Loop: for i = 0; i < strLen; i++ { buf[i] = toupper(buf[i]); }
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "upper.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "upper.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "upper.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "upper.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpULT(idx, strLen, "upper.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* charPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, idx, "upper.charptr");
        auto* chLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "upper.ch");
        chLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* ch32 = builder->CreateZExt(chLoad, llvm::Type::getInt32Ty(*context), "upper.ch32");
        llvm::Value* upper = builder->CreateCall(getOrDeclareToupper(), {ch32}, "upper.toupper");
        llvm::Value* upper8 = builder->CreateTrunc(upper, llvm::Type::getInt8Ty(*context), "upper.trunc");
        auto* upperStore = builder->CreateStore(upper8, charPtr);
        upperStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "upper.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(doneBB);
        stringReturningFunctions_.insert("str_upper");
        return buf;
    }

    if (bid == BuiltinId::STR_LOWER) {
        validateArgCount(expr, "str_lower", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "lower.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "lower.len");
        llvm::Value* allocSize = builder->CreateAdd(strLen, llvm::ConstantInt::get(getDefaultType(), 1), "lower.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "lower.buf");
        builder->CreateCall(getOrDeclareStrcpy(), {buf, strPtr});
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "lower.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "lower.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "lower.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "lower.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpULT(idx, strLen, "lower.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* charPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, idx, "lower.charptr");
        auto* chLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "lower.ch");
        chLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* ch32 = builder->CreateZExt(chLoad, llvm::Type::getInt32Ty(*context), "lower.ch32");
        llvm::Value* lower = builder->CreateCall(getOrDeclareTolower(), {ch32}, "lower.tolower");
        llvm::Value* lower8 = builder->CreateTrunc(lower, llvm::Type::getInt8Ty(*context), "lower.trunc");
        auto* lowerStore = builder->CreateStore(lower8, charPtr);
        lowerStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "lower.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(doneBB);
        stringReturningFunctions_.insert("str_lower");
        return buf;
    }

    if (bid == BuiltinId::STR_CONTAINS) {
        validateArgCount(expr, "str_contains", 2);

        // ── Compile-time str_contains folding ───────────────────────
        // When both arguments are string literals, fold at compile time.
        if (auto* hayLit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (hayLit->literalType == LiteralExpr::LiteralType::STRING) {
                if (auto* needleLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get())) {
                    if (needleLit->literalType == LiteralExpr::LiteralType::STRING) {
                        bool found = hayLit->stringValue.find(needleLit->stringValue) != std::string::npos;
                        return llvm::ConstantInt::get(getDefaultType(), found ? 1 : 0);
                    }
                }
            }
        }

        // Detect single-character literal needle → use memchr (SIMD-optimized)
        // instead of strstr (byte-by-byte scan).
        Expression* needleExpr = expr->arguments[1].get();
        bool isSingleChar = false;
        char singleCharVal = 0;
        if (needleExpr->type == ASTNodeType::LITERAL_EXPR) {
            auto* lit = static_cast<LiteralExpr*>(needleExpr);
            if (lit->literalType == LiteralExpr::LiteralType::STRING && lit->stringValue.size() == 1) {
                isSingleChar = true;
                singleCharVal = lit->stringValue[0];
            }
        }
        llvm::Value* haystackArg = generateExpression(expr->arguments[0].get());
        llvm::Value* haystackPtr =
            haystackArg->getType()->isPointerTy()
                ? haystackArg
                : builder->CreateIntToPtr(haystackArg, llvm::PointerType::getUnqual(*context), "contains.haystack");
        llvm::Value* result;
        if (isSingleChar) {
            // memchr(haystack, char, strlen(haystack)) is SIMD-optimized on
            // modern libc, ~2-3x faster than strstr for single-char searches.
            llvm::Value* len = builder->CreateCall(getOrDeclareStrlen(), {haystackPtr}, "contains.len");
            // Cast char to i32 for memchr.  Use unsigned char intermediate to
            // ensure correct zero-extension (plain char may be signed on some
            // platforms, which would sign-extend values > 127 incorrectly).
            auto unsignedCharValue = static_cast<uint32_t>(static_cast<unsigned char>(singleCharVal));
            llvm::Value* charVal = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), unsignedCharValue);
            result = builder->CreateCall(getOrDeclareMemchr(), {haystackPtr, charVal, len}, "contains.memchr");
        } else {
            llvm::Value* needleArg = generateExpression(needleExpr);
            llvm::Value* needlePtr =
                needleArg->getType()->isPointerTy()
                    ? needleArg
                    : builder->CreateIntToPtr(needleArg, llvm::PointerType::getUnqual(*context), "contains.needle");
            result = builder->CreateCall(getOrDeclareStrstr(), {haystackPtr, needlePtr}, "contains.strstr");
        }
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNotNull = builder->CreateICmpNE(result, nullPtr, "contains.notnull");
        return builder->CreateZExt(isNotNull, getDefaultType(), "contains.result");
    }

    if (bid == BuiltinId::STR_INDEX_OF) {
        validateArgCount(expr, "str_index_of", 2);

        // ── Compile-time str_index_of folding ───────────────────────
        if (auto* hayLit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (hayLit->literalType == LiteralExpr::LiteralType::STRING) {
                if (auto* needleLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get())) {
                    if (needleLit->literalType == LiteralExpr::LiteralType::STRING) {
                        auto pos = hayLit->stringValue.find(needleLit->stringValue);
                        int64_t result = (pos == std::string::npos) ? -1 : static_cast<int64_t>(pos);
                        return llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(result));
                    }
                }
            }
        }

        // Detect single-character literal needle → use memchr (SIMD-optimized)
        // instead of strstr (byte-by-byte scan).
        Expression* needleExpr = expr->arguments[1].get();
        bool isSingleChar = false;
        char singleCharVal = 0;
        if (needleExpr->type == ASTNodeType::LITERAL_EXPR) {
            auto* lit = static_cast<LiteralExpr*>(needleExpr);
            if (lit->literalType == LiteralExpr::LiteralType::STRING && lit->stringValue.size() == 1) {
                isSingleChar = true;
                singleCharVal = lit->stringValue[0];
            }
        }
        llvm::Value* haystackArg = generateExpression(expr->arguments[0].get());
        llvm::Value* haystackPtr =
            haystackArg->getType()->isPointerTy()
                ? haystackArg
                : builder->CreateIntToPtr(haystackArg, llvm::PointerType::getUnqual(*context), "indexof.haystack");
        llvm::Value* result;
        if (isSingleChar) {
            llvm::Value* len = builder->CreateCall(getOrDeclareStrlen(), {haystackPtr}, "indexof.len");
            // Cast char to i32 for memchr (see str_contains above for rationale).
            auto unsignedCharValue = static_cast<uint32_t>(static_cast<unsigned char>(singleCharVal));
            llvm::Value* charVal = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), unsignedCharValue);
            result = builder->CreateCall(getOrDeclareMemchr(), {haystackPtr, charVal, len}, "indexof.memchr");
        } else {
            llvm::Value* needleArg = generateExpression(needleExpr);
            llvm::Value* needlePtr =
                needleArg->getType()->isPointerTy()
                    ? needleArg
                    : builder->CreateIntToPtr(needleArg, llvm::PointerType::getUnqual(*context), "indexof.needle");
            result = builder->CreateCall(getOrDeclareStrstr(), {haystackPtr, needlePtr}, "indexof.strstr");
        }
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNull = builder->CreateICmpEQ(result, nullPtr, "indexof.isnull");
        llvm::Value* foundInt = builder->CreatePtrToInt(result, getDefaultType(), "indexof.foundint");
        llvm::Value* baseInt = builder->CreatePtrToInt(haystackPtr, getDefaultType(), "indexof.baseint");
        llvm::Value* offset = builder->CreateSub(foundInt, baseInt, "indexof.offset");
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        return builder->CreateSelect(isNull, negOne, offset, "indexof.result");
    }

    if (bid == BuiltinId::STR_REPLACE) {
        validateArgCount(expr, "str_replace", 3);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* oldArg = generateExpression(expr->arguments[1].get());
        llvm::Value* newArg = generateExpression(expr->arguments[2].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, ptrTy, "replace.str");
        llvm::Value* oldPtr =
            oldArg->getType()->isPointerTy()
                ? oldArg
                : builder->CreateIntToPtr(oldArg, ptrTy, "replace.old");
        llvm::Value* newPtr =
            newArg->getType()->isPointerTy()
                ? newArg
                : builder->CreateIntToPtr(newArg, ptrTy, "replace.new");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::Value* nullPtr  = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* zero     = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one      = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* i8zero   = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0);

        llvm::Value* oldLen  = builder->CreateCall(getOrDeclareStrlen(), {oldPtr}, "replace.oldlen");
        llvm::Value* newLen  = builder->CreateCall(getOrDeclareStrlen(), {newPtr}, "replace.newlen");
        llvm::Value* strLen  = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "replace.strlen");

        // If old is the empty string, just return a copy of str to avoid
        // an infinite loop (strstr("x","") always succeeds).
        llvm::BasicBlock* emptyOldBB = llvm::BasicBlock::Create(*context, "replace.emptyold", function);
        llvm::BasicBlock* replaceMainBB = llvm::BasicBlock::Create(*context, "replace.main", function);
        llvm::Value* oldIsEmpty = builder->CreateICmpEQ(oldLen, zero, "replace.oldempty");
        // Empty-old is a degenerate edge case — heavily favour normal path.
        auto* eoW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(oldIsEmpty, emptyOldBB, replaceMainBB, eoW);

        // Empty old: return strdup(str)
        builder->SetInsertPoint(emptyOldBB);
        llvm::Value* copySize0 = builder->CreateAdd(strLen, one, "replace.copysize0", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* copyBuf0  = builder->CreateCall(getOrDeclareMalloc(), {copySize0}, "replace.copybuf0");
        builder->CreateCall(getOrDeclareStrcpy(), {copyBuf0, strPtr});

        llvm::BasicBlock* emptyOldExitBB = builder->GetInsertBlock();
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "replace.merge", function);
        builder->CreateBr(mergeBB);

        // ---------------------------------------------------------------
        // Pass 1: count occurrences of old in str
        // ---------------------------------------------------------------
        builder->SetInsertPoint(replaceMainBB);
        llvm::BasicBlock* countLoopBB = llvm::BasicBlock::Create(*context, "replace.countloop", function);
        llvm::BasicBlock* countBodyBB = llvm::BasicBlock::Create(*context, "replace.countbody", function);
        llvm::BasicBlock* countDoneBB = llvm::BasicBlock::Create(*context, "replace.countdone", function);
        builder->CreateBr(countLoopBB);

        // Loop header: PHIs for cursor position and running count.
        builder->SetInsertPoint(countLoopBB);
        llvm::PHINode* cCursor = builder->CreatePHI(ptrTy, 2, "replace.ccursor");
        cCursor->addIncoming(strPtr, replaceMainBB);
        llvm::PHINode* cCount  = builder->CreatePHI(getDefaultType(), 2, "replace.ccount");
        cCount->addIncoming(zero, replaceMainBB);

        // Single strstr call per iteration.
        llvm::Value* cFound  = builder->CreateCall(getOrDeclareStrstr(), {cCursor, oldPtr}, "replace.cfound");
        llvm::Value* cIsNull = builder->CreateICmpEQ(cFound, nullPtr, "replace.cisnull");
        builder->CreateCondBr(cIsNull, countDoneBB, countBodyBB);

        // Body: increment count, advance cursor past the matched occurrence.
        builder->SetInsertPoint(countBodyBB);
        llvm::Value* newCount   = builder->CreateAdd(cCount, one, "replace.newcount", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextCursor = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), cFound, oldLen, "replace.nextcursor");
        cCursor->addIncoming(nextCursor, countBodyBB);
        cCount->addIncoming(newCount, countBodyBB);
        builder->CreateBr(countLoopBB);

        // Done: cCount holds the total number of occurrences.
        builder->SetInsertPoint(countDoneBB);
        // cCount is the final count; use it directly (single predecessor from countLoopBB).
        llvm::Value* totalCount = cCount;

        // ---------------------------------------------------------------
        // Compute result size and allocate
        // ---------------------------------------------------------------
        // resultLen = strLen + totalCount * (newLen - oldLen)
        llvm::Value* lenDiff   = builder->CreateSub(newLen, oldLen, "replace.lendiff");
        llvm::Value* extraLen  = builder->CreateMul(totalCount, lenDiff, "replace.extralen");
        llvm::Value* resultLen = builder->CreateAdd(strLen, extraLen, "replace.resultlen");
        llvm::Value* resultSize= builder->CreateAdd(resultLen, one, "replace.resultsize", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* resultBuf = builder->CreateCall(getOrDeclareMalloc(), {resultSize}, "replace.resultbuf");

        // ---------------------------------------------------------------
        // Pass 2: build output string, replacing every occurrence
        // ---------------------------------------------------------------
        llvm::BasicBlock* buildLoopBB = llvm::BasicBlock::Create(*context, "replace.buildloop", function);
        llvm::BasicBlock* buildBodyBB = llvm::BasicBlock::Create(*context, "replace.buildbody", function);
        llvm::BasicBlock* buildDoneBB = llvm::BasicBlock::Create(*context, "replace.builddone", function);
        builder->CreateBr(buildLoopBB);

        builder->SetInsertPoint(buildLoopBB);
        llvm::PHINode* bSrc = builder->CreatePHI(ptrTy, 2, "replace.bsrc");
        bSrc->addIncoming(strPtr, countDoneBB);
        llvm::PHINode* bDst = builder->CreatePHI(ptrTy, 2, "replace.bdst");
        bDst->addIncoming(resultBuf, countDoneBB);

        llvm::Value* bFound  = builder->CreateCall(getOrDeclareStrstr(), {bSrc, oldPtr}, "replace.bfound");
        llvm::Value* bIsNull = builder->CreateICmpEQ(bFound, nullPtr, "replace.bnull");
        builder->CreateCondBr(bIsNull, buildDoneBB, buildBodyBB);

        // Body: copy prefix, then replacement, advance
        builder->SetInsertPoint(buildBodyBB);
        llvm::Value* prefLen = builder->CreatePtrDiff(llvm::Type::getInt8Ty(*context), bFound, bSrc, "replace.preflen");
        builder->CreateCall(getOrDeclareMemcpy(), {bDst, bSrc, prefLen});
        llvm::Value* dstAfterPref = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), bDst, prefLen, "replace.dstpref");
        builder->CreateCall(getOrDeclareMemcpy(), {dstAfterPref, newPtr, newLen});
        llvm::Value* dstAfterNew  = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), dstAfterPref, newLen, "replace.dstnew");
        llvm::Value* srcAfterOld  = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), bFound, oldLen, "replace.srcold");
        bSrc->addIncoming(srcAfterOld, buildBodyBB);
        bDst->addIncoming(dstAfterNew, buildBodyBB);
        builder->CreateBr(buildLoopBB);

        // Done: copy remaining tail and null-terminate.
        // bSrc and bDst are live values from the loop header (single predecessor).
        builder->SetInsertPoint(buildDoneBB);
        // Copy remaining chars: tail = strLen - (bSrc - strPtr)
        llvm::Value* srcBase  = builder->CreatePtrToInt(strPtr, getDefaultType(), "replace.srcbase");
        llvm::Value* srcCurr  = builder->CreatePtrToInt(bSrc,   getDefaultType(), "replace.srccurr");
        llvm::Value* consumed = builder->CreateSub(srcCurr, srcBase, "replace.consumed");
        llvm::Value* tail     = builder->CreateSub(strLen, consumed, "replace.tail");
        builder->CreateCall(getOrDeclareMemcpy(), {bDst, bSrc, tail});
        llvm::Value* endPtr   = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), bDst, tail, "replace.end");
        builder->CreateStore(i8zero, endPtr);

        llvm::BasicBlock* buildExitBB = builder->GetInsertBlock();
        builder->CreateBr(mergeBB);

        // ---------------------------------------------------------------
        // Merge: return result (or copy if old was empty)
        // ---------------------------------------------------------------
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* resultPhi = builder->CreatePHI(ptrTy, 2, "replace.result");
        resultPhi->addIncoming(copyBuf0,   emptyOldExitBB);
        resultPhi->addIncoming(resultBuf,  buildExitBB);
        stringReturningFunctions_.insert("str_replace");
        return resultPhi;
    }

    if (bid == BuiltinId::STR_TRIM) {
        validateArgCount(expr, "str_trim", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "trim.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "trim.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Find start (skip leading whitespace)
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* startLoopBB = llvm::BasicBlock::Create(*context, "trim.startloop", function);
        llvm::BasicBlock* startBodyBB = llvm::BasicBlock::Create(*context, "trim.startbody", function);
        llvm::BasicBlock* startDoneBB = llvm::BasicBlock::Create(*context, "trim.startdone", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        builder->CreateBr(startLoopBB);

        builder->SetInsertPoint(startLoopBB);
        llvm::PHINode* startIdx = builder->CreatePHI(getDefaultType(), 2, "trim.startidx");
        startIdx->addIncoming(zero, preheader);
        llvm::Value* startCond = builder->CreateICmpULT(startIdx, strLen, "trim.startcond");
        builder->CreateCondBr(startCond, startBodyBB, startDoneBB);

        builder->SetInsertPoint(startBodyBB);
        llvm::Value* startCharPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, startIdx, "trim.startcharptr");
        auto* startCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), startCharPtr, "trim.startchar");
        startCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* startChar32 = builder->CreateZExt(startCharLoad, llvm::Type::getInt32Ty(*context), "trim.startchar32");
        llvm::Value* isStartSpace = builder->CreateCall(getOrDeclareIsspace(), {startChar32}, "trim.isspace");
        llvm::Value* isStartSpaceBool = builder->CreateICmpNE(isStartSpace, builder->getInt32(0), "trim.isspacebool");
        llvm::Value* nextStartIdx = builder->CreateAdd(startIdx, one, "trim.nextstartidx", /*HasNUW=*/true, /*HasNSW=*/true);
        startIdx->addIncoming(nextStartIdx, startBodyBB);
        // If space, continue; otherwise done
        llvm::BasicBlock* startContBB = llvm::BasicBlock::Create(*context, "trim.startcont", function);
        builder->CreateCondBr(isStartSpaceBool, startContBB, startDoneBB);
        builder->SetInsertPoint(startContBB);
        builder->CreateBr(startLoopBB);
        // Update PHI to accept from cont block instead of body block
        startIdx->removeIncomingValue(startBodyBB);
        startIdx->addIncoming(nextStartIdx, startContBB);

        builder->SetInsertPoint(startDoneBB);
        llvm::PHINode* trimStart = builder->CreatePHI(getDefaultType(), 2, "trim.start");
        trimStart->addIncoming(startIdx, startLoopBB); // reached end of string
        trimStart->addIncoming(startIdx, startBodyBB); // found non-space

        // Find end (skip trailing whitespace)
        llvm::BasicBlock* endPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* endLoopBB = llvm::BasicBlock::Create(*context, "trim.endloop", function);
        llvm::BasicBlock* endBodyBB = llvm::BasicBlock::Create(*context, "trim.endbody", function);
        llvm::BasicBlock* endDoneBB = llvm::BasicBlock::Create(*context, "trim.enddone", function);
        builder->CreateBr(endLoopBB);

        builder->SetInsertPoint(endLoopBB);
        llvm::PHINode* endIdx = builder->CreatePHI(getDefaultType(), 2, "trim.endidx");
        endIdx->addIncoming(strLen, endPreBB);
        llvm::Value* endCond = builder->CreateICmpUGT(endIdx, trimStart, "trim.endcond");
        builder->CreateCondBr(endCond, endBodyBB, endDoneBB);

        builder->SetInsertPoint(endBodyBB);
        llvm::Value* prevEndIdx = builder->CreateSub(endIdx, one, "trim.prevendidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* endCharPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, prevEndIdx, "trim.endcharptr");
        auto* endCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), endCharPtr, "trim.endchar");
        endCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* endChar32 = builder->CreateZExt(endCharLoad, llvm::Type::getInt32Ty(*context), "trim.endchar32");
        llvm::Value* isEndSpace = builder->CreateCall(getOrDeclareIsspace(), {endChar32}, "trim.isendspace");
        llvm::Value* isEndSpaceBool = builder->CreateICmpNE(isEndSpace, builder->getInt32(0), "trim.isendbool");
        llvm::BasicBlock* endContBB = llvm::BasicBlock::Create(*context, "trim.endcont", function);
        builder->CreateCondBr(isEndSpaceBool, endContBB, endDoneBB);
        builder->SetInsertPoint(endContBB);
        endIdx->addIncoming(prevEndIdx, endContBB);
        builder->CreateBr(endLoopBB);

        builder->SetInsertPoint(endDoneBB);
        llvm::PHINode* trimEnd = builder->CreatePHI(getDefaultType(), 2, "trim.end");
        trimEnd->addIncoming(endIdx, endLoopBB); // empty result
        trimEnd->addIncoming(endIdx, endBodyBB); // found non-space

        // Build trimmed string
        llvm::Value* trimLen = builder->CreateSub(trimEnd, trimStart, "trim.len2");
        llvm::Value* trimAlloc = builder->CreateAdd(trimLen, llvm::ConstantInt::get(getDefaultType(), 1), "trim.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* trimBuf = builder->CreateCall(getOrDeclareMalloc(), {trimAlloc}, "trim.buf");
        llvm::Value* trimSrc = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, trimStart, "trim.src");
        builder->CreateCall(getOrDeclareMemcpy(), {trimBuf, trimSrc, trimLen});
        llvm::Value* trimEndPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), trimBuf, trimLen, "trim.endptr");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), trimEndPtr);
        stringReturningFunctions_.insert("str_trim");
        return trimBuf;
    }

    if (bid == BuiltinId::STR_STARTS_WITH) {
        validateArgCount(expr, "str_starts_with", 2);

        // ── Compile-time str_starts_with folding ────────────────────
        // When both arguments are string literals, fold at compile time.
        if (auto* strLit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (strLit->literalType == LiteralExpr::LiteralType::STRING) {
                if (auto* prefixLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get())) {
                    if (prefixLit->literalType == LiteralExpr::LiteralType::STRING) {
                        bool result = strLit->stringValue.substr(0, prefixLit->stringValue.size())
                                      == prefixLit->stringValue;
                        return llvm::ConstantInt::get(getDefaultType(), result ? 1 : 0);
                    }
                }
            }
        }

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* prefixArg = generateExpression(expr->arguments[1].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "startswith.str");
        llvm::Value* prefixPtr =
            prefixArg->getType()->isPointerTy()
                ? prefixArg
                : builder->CreateIntToPtr(prefixArg, llvm::PointerType::getUnqual(*context), "startswith.prefix");
        // Use strncmp(str, prefix, prefix_len) == 0 instead of strstr
        // strncmp only examines the first prefix_len bytes, while strstr
        // would scan the entire string looking for the prefix anywhere.
        llvm::Value* prefixLen = builder->CreateCall(getOrDeclareStrlen(), {prefixPtr}, "startswith.plen");
        llvm::Value* cmpResult = builder->CreateCall(getOrDeclareStrncmp(),
            {strPtr, prefixPtr, prefixLen}, "startswith.cmp");
        llvm::Value* isEqual = builder->CreateICmpEQ(cmpResult, builder->getInt32(0), "startswith.eq");
        return builder->CreateZExt(isEqual, getDefaultType(), "startswith.result");
    }

    if (bid == BuiltinId::STR_ENDS_WITH) {
        validateArgCount(expr, "str_ends_with", 2);

        // ── Compile-time str_ends_with folding ──────────────────────
        // When both arguments are string literals, fold at compile time.
        if (auto* strLit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (strLit->literalType == LiteralExpr::LiteralType::STRING) {
                if (auto* suffixLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get())) {
                    if (suffixLit->literalType == LiteralExpr::LiteralType::STRING) {
                        const auto& s = strLit->stringValue;
                        const auto& suffix = suffixLit->stringValue;
                        bool result = s.size() >= suffix.size() &&
                                      s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
                        return llvm::ConstantInt::get(getDefaultType(), result ? 1 : 0);
                    }
                }
            }
        }

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* suffixArg = generateExpression(expr->arguments[1].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "endswith.str");
        llvm::Value* suffixPtr =
            suffixArg->getType()->isPointerTy()
                ? suffixArg
                : builder->CreateIntToPtr(suffixArg, llvm::PointerType::getUnqual(*context), "endswith.suffix");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "endswith.strlen");
        llvm::Value* sufLen = builder->CreateCall(getOrDeclareStrlen(), {suffixPtr}, "endswith.suflen");
        // If suffix longer than string, return 0
        llvm::Value* tooLong = builder->CreateICmpSGT(sufLen, strLen, "endswith.toolong");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(*context, "endswith.check", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "endswith.fail", function);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "endswith.merge", function);
        // Suffix longer than string is unlikely in typical use.
        llvm::MDNode* ewW = llvm::MDBuilder(*context).createBranchWeights(1, 99);
        builder->CreateCondBr(tooLong, failBB, checkBB, ewW);
        builder->SetInsertPoint(failBB);
        builder->CreateBr(mergeBB);
        builder->SetInsertPoint(checkBB);
        // Compare str + (strLen - sufLen) with suffix using memcmp.
        // memcmp is faster than strcmp because we already know the exact
        // length to compare and it can use SIMD-optimized comparison.
        llvm::Value* offset = builder->CreateSub(strLen, sufLen, "endswith.offset");
        llvm::Value* tailPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, offset, "endswith.tail");
        llvm::Value* cmpResult = builder->CreateCall(getOrDeclareMemcmp(), {tailPtr, suffixPtr, sufLen}, "endswith.cmp");
        llvm::Value* isEqual = builder->CreateICmpEQ(cmpResult, builder->getInt32(0), "endswith.eq");
        llvm::Value* resultCheck = builder->CreateZExt(isEqual, getDefaultType(), "endswith.result");
        builder->CreateBr(mergeBB);
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "endswith.phi");
        result->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), failBB);
        result->addIncoming(resultCheck, checkBB);
        return result;
    }

    if (bid == BuiltinId::STR_REPEAT) {
        validateArgCount(expr, "str_repeat", 2);

        // ── Compile-time str_repeat folding ─────────────────────────
        // When both arguments are compile-time constants and the result
        // is reasonably small, fold at compile time.
        if (auto* strLit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (strLit->literalType == LiteralExpr::LiteralType::STRING) {
                if (auto* countLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get())) {
                    if (countLit->literalType == LiteralExpr::LiteralType::INTEGER) {
                        int64_t count = countLit->intValue;
                        // Only fold for small results (≤ 256 bytes) to avoid
                        // bloating the data section.
                        if (count >= 0 && count * static_cast<int64_t>(strLit->stringValue.size()) <= 256) {
                            std::string result;
                            result.reserve(static_cast<size_t>(count) * strLit->stringValue.size());
                            for (int64_t i = 0; i < count; ++i) {
                                result += strLit->stringValue;
                            }
                            llvm::GlobalVariable* gv = internString(result);
                            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                                gv->getValueType(),
                                gv,
                                llvm::ArrayRef<llvm::Constant*>{
                                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)
                                });
                        }
                    }
                }
            }
        }

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* countArg = generateExpression(expr->arguments[1].get());
        countArg = toDefaultType(countArg);
        // Clamp negative counts to 0 to prevent integer overflow in the
        // totalLen = strLen * count multiplication (negative * positive wraps
        // to a large unsigned value, causing malloc to over-allocate or fail).
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isNeg = builder->CreateICmpSLT(countArg, zero, "repeat.isneg");
        countArg = builder->CreateSelect(isNeg, zero, countArg, "repeat.clamp");
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "repeat.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "repeat.len");
        llvm::Value* totalLen = builder->CreateMul(strLen, countArg, "repeat.total", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* allocSize =
            builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "repeat.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "repeat.buf");
        // Use memcpy with tracked offset instead of strcat to avoid O(n²) rescanning
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "repeat.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "repeat.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "repeat.done", function);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "repeat.idx");
        idx->addIncoming(zero, preheader);
        llvm::PHINode* offset = builder->CreatePHI(getDefaultType(), 2, "repeat.off");
        offset->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpULT(idx, countArg, "repeat.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        // memcpy(buf + offset, str, strLen)
        llvm::Value* dst = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, offset, "repeat.dst");
        builder->CreateCall(getOrDeclareMemcpy(), {dst, strPtr, strLen});
        llvm::Value* nextOffset = builder->CreateAdd(offset, strLen, "repeat.nextoff", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "repeat.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        offset->addIncoming(nextOffset, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(doneBB);
        // Null-terminate: buf[totalLen] = '\0'
        llvm::Value* endPtr = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, totalLen, "repeat.end");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), endPtr);
        stringReturningFunctions_.insert("str_repeat");
        return buf;
    }

    if (bid == BuiltinId::STR_REVERSE) {
        validateArgCount(expr, "str_reverse", 1);

        // ── Compile-time str_reverse folding ────────────────────────
        if (auto* strLit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (strLit->literalType == LiteralExpr::LiteralType::STRING) {
                std::string reversed = strLit->stringValue;
                std::reverse(reversed.begin(), reversed.end());
                llvm::GlobalVariable* gv = internString(reversed);
                return llvm::ConstantExpr::getInBoundsGetElementPtr(
                    gv->getValueType(),
                    gv,
                    llvm::ArrayRef<llvm::Constant*>{
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)
                    });
            }
        }

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strrev.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strrev.len");
        llvm::Value* allocSize =
            builder->CreateAdd(strLen, llvm::ConstantInt::get(getDefaultType(), 1), "strrev.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "strrev.buf");
        // Loop: buf[i] = str[len-1-i]
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "strrev.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "strrev.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "strrev.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "strrev.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpULT(idx, strLen, "strrev.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* revIdx = builder->CreateSub(builder->CreateSub(strLen, one, "strrev.lenm1", /*HasNUW=*/true, /*HasNSW=*/true), idx, "strrev.revidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* srcPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, revIdx, "strrev.srcptr");
        auto* revLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), srcPtr, "strrev.ch");
        revLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* dstPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, idx, "strrev.dstptr");
        auto* revStore = builder->CreateStore(revLoad, dstPtr);
        revStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "strrev.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(doneBB);
        // Null-terminate
        llvm::Value* endPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, strLen, "strrev.endptr");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), endPtr);
        stringReturningFunctions_.insert("str_reverse");
        return buf;
    }

    // -----------------------------------------------------------------------
    // Array built-ins: push, pop, index_of, array_contains, sort,
    //   array_fill, array_concat, array_slice
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::PUSH) {
        validateArgCount(expr, "push", 2);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        // Array layout: [length, elem0, elem1, ...]
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "push.arrptr");
        auto* pushLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "push.oldlen");
        pushLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        pushLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* oldLen = pushLenLoad;
        llvm::Value* newLen = builder->CreateAdd(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "push.newlen", /*HasNUW=*/true, /*HasNSW=*/true);
        // Only call realloc when the new slot count crosses a power-of-2
        // boundary.  The allocation capacity is always max(16, nextPow2(slots)).
        // We need to grow when nextPow2(newSlots) > nextPow2(oldSlots), which
        // happens when oldSlots (= oldLen + 1) is a power of 2 AND >= 16.
        // For oldSlots < 16 we still realloc for the very first push (oldLen==0)
        // to ensure the buffer is large enough for the minimum capacity of 16.
        llvm::Value* one64 = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* oldSlots = builder->CreateAdd(oldLen, one64, "push.oldslots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* minSlots = llvm::ConstantInt::get(getDefaultType(), 16);
        // Check: oldSlots is a power of 2 → (oldSlots & (oldSlots - 1)) == 0
        llvm::Value* oldSlotsM1 = builder->CreateSub(oldSlots, one64, "push.osm1", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* andCheck = builder->CreateAnd(oldSlots, oldSlotsM1, "push.ispow2");
        llvm::Value* zero64 = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isPow2 = builder->CreateICmpEQ(andCheck, zero64, "push.ispow2cmp");
        // Need to grow when oldSlots >= 16 AND is power of 2, OR when oldSlots < 16
        llvm::Value* belowMin = builder->CreateICmpSLT(oldSlots, minSlots, "push.belowmin");
        llvm::Value* atBoundary = builder->CreateAnd(isPow2,
            builder->CreateNot(belowMin, "push.abovemin"), "push.atbound");
        llvm::Value* needsGrow = builder->CreateOr(belowMin, atBoundary, "push.needsgrow");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* growBB = llvm::BasicBlock::Create(*context, "push.grow", function);
        llvm::BasicBlock* nogrowBB = llvm::BasicBlock::Create(*context, "push.nogrow", function);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "push.merge", function);

        // Growth is rare (only at power-of-2 boundaries + first push to min 16).
        // Weight the no-grow path heavily for branch prediction and code layout.
        llvm::MDNode* pushGrowW = llvm::MDBuilder(*context).createBranchWeights(1, 99);
        builder->CreateCondBr(needsGrow, growBB, nogrowBB, pushGrowW);

        // Grow path: compute new capacity and realloc
        builder->SetInsertPoint(growBB);
        llvm::Value* slots = builder->CreateAdd(newLen, one64, "push.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        // nextPow2 via ctlz intrinsic: 1 << (64 - ctlz(slots - 1))
        // This replaces a 6-shift OR-cascade (~14 instructions) with 4
        // instructions (sub, ctlz, sub, shl), which maps to a single
        // BSR/LZCNT + shift on x86-64.
        llvm::Value* slotsM1 = builder->CreateSub(slots, one64, "push.pm1", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Function* ctlzFn = OMSC_GET_INTRINSIC(module.get(),
            llvm::Intrinsic::ctlz, {getDefaultType()});
        // is_zero_poison=true: slots is always >= 2 here (newLen >= 1), so
        // slotsM1 is always >= 1, never zero.
        llvm::Value* lz = builder->CreateCall(ctlzFn,
            {slotsM1, llvm::ConstantInt::getTrue(*context)}, "push.lz");
        llvm::Value* shift = builder->CreateSub(
            llvm::ConstantInt::get(getDefaultType(), 64), lz, "push.shift");
        llvm::Value* cap = builder->CreateShl(one64, shift, "push.cap", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* useMin = builder->CreateICmpSLT(cap, minSlots, "push.usemin");
        cap = builder->CreateSelect(useMin, minSlots, cap, "push.finalcap");
        llvm::Value* newSize = builder->CreateMul(cap,
            llvm::ConstantInt::get(getDefaultType(), 8), "push.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* grownBuf = builder->CreateCall(getOrDeclareRealloc(), {arrPtr, newSize}, "push.newbuf");
        builder->CreateBr(mergeBB);

        // No-grow path: reuse existing buffer
        builder->SetInsertPoint(nogrowBB);
        builder->CreateBr(mergeBB);

        // Merge: select the buffer pointer
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* newBuf = builder->CreatePHI(llvm::PointerType::getUnqual(*context), 2, "push.buf");
        newBuf->addIncoming(grownBuf, growBB);
        newBuf->addIncoming(arrPtr, nogrowBB);

        // Update length
        auto* pushLenSt = builder->CreateAlignedStore(newLen, newBuf, llvm::MaybeAlign(8));
        pushLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        // Store new value at index oldLen + 1 (after header)
        llvm::Value* newElemIdx =
            builder->CreateAdd(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "push.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* newElemPtr = builder->CreateInBoundsGEP(getDefaultType(), newBuf, newElemIdx, "push.elemptr");
        auto* pushElemSt = builder->CreateAlignedStore(valArg, newElemPtr, llvm::MaybeAlign(8));
        pushElemSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        // Return new array pointer as i64
        return builder->CreatePtrToInt(newBuf, getDefaultType(), "push.result");
    }

    if (bid == BuiltinId::POP) {
        validateArgCount(expr, "pop", 1);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "pop.arrptr");
        auto* popLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "pop.oldlen");
        popLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        popLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* oldLen = popLenLoad;

        // Guard against popping from an empty array.
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isEmpty = builder->CreateICmpSLE(oldLen, zero, "pop.empty");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "pop.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "pop.fail", function);
        // Popping from an empty array is extremely unlikely.
        llvm::MDNode* popW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(isEmpty, failBB, okBB, popW);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: pop from empty array\n", "pop_empty_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Return the last element
        llvm::Value* lastIdx =
            builder->CreateAdd(builder->CreateSub(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "pop.lastoff", /*HasNUW=*/true, /*HasNSW=*/true),
                               llvm::ConstantInt::get(getDefaultType(), 1), "pop.lastidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* lastPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, lastIdx, "pop.lastptr");
        llvm::Value* lastVal = builder->CreateAlignedLoad(getDefaultType(), lastPtr, llvm::MaybeAlign(8), "pop.lastval");
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(lastVal))
            load->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        // Decrease length in-place
        llvm::Value* newLen = builder->CreateSub(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "pop.newlen", /*HasNUW=*/true, /*HasNSW=*/true);
        auto* popLenSt = builder->CreateAlignedStore(newLen, arrPtr, llvm::MaybeAlign(8));
        popLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        return lastVal;
    }

    if (bid == BuiltinId::INDEX_OF) {
        validateArgCount(expr, "index_of", 2);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "indexof.arrptr");
        auto* idxofLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "indexof.len");
        idxofLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        idxofLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = idxofLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "indexof.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "indexof.body", function);
        llvm::BasicBlock* nextBB = llvm::BasicBlock::Create(*context, "indexof.next", function);
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "indexof.found", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "indexof.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "indexof.idx");
        idx->addIncoming(zero, preheader);
        // Unsigned: idx starts at 0, arrLen ≥ 0 (range metadata).
        llvm::Value* cond = builder->CreateICmpULT(idx, arrLen, "indexof.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "indexof.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, elemIdx, "indexof.elemptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "indexof.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* match = builder->CreateICmpEQ(elem, valArg, "indexof.match");
        builder->CreateCondBr(match, foundBB, nextBB);
        builder->SetInsertPoint(nextBB);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "indexof.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, nextBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(foundBB);
        builder->CreateBr(doneBB);
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "indexof.result");
        result->addIncoming(negOne, loopBB);
        result->addIncoming(idx, foundBB);
        return result;
    }

    if (bid == BuiltinId::ARRAY_CONTAINS) {
        validateArgCount(expr, "array_contains", 2);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "contains.arrptr");
        auto* containsLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "contains.len");
        containsLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        containsLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = containsLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "contains.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "contains.body", function);
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "contains.found", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "contains.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "contains.idx");
        idx->addIncoming(zero, preheader);
        // Unsigned: idx starts at 0, arrLen ≥ 0 (range metadata).
        llvm::Value* cond = builder->CreateICmpULT(idx, arrLen, "contains.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "contains.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, elemIdx, "contains.elemptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "contains.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* match = builder->CreateICmpEQ(elem, valArg, "contains.match");
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "contains.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        builder->CreateCondBr(match, foundBB, loopBB);
        builder->SetInsertPoint(foundBB);
        builder->CreateBr(doneBB);
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "contains.result");
        result->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), loopBB);
        result->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), foundBB);
        return result;
    }

    if (bid == BuiltinId::SORT) {
        validateArgCount(expr, "sort", 1);
        bool sortStrings = isStringArrayExpr(expr->arguments[0].get());
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "sort.arrptr");
        auto* sortLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "sort.len");
        sortLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        sortLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = sortLenLoad;

        // Early exit: skip qsort for arrays with 0 or 1 elements (already sorted).
        // This avoids the overhead of a function call to qsort for trivial cases.
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* sortBB = llvm::BasicBlock::Create(*context, "sort.call", function);
        llvm::BasicBlock* skipBB = llvm::BasicBlock::Create(*context, "sort.skip", function);
        llvm::Value* needsSort = builder->CreateICmpUGT(arrLen,
            llvm::ConstantInt::get(getDefaultType(), 1), "sort.needed");
        builder->CreateCondBr(needsSort, sortBB, skipBB);
        builder->SetInsertPoint(sortBB);

        // Use libc qsort() — O(n log n) instead of the previous O(n²)
        // bubble sort.  Each element is an i64 (8 bytes); string arrays
        // store char* pointers cast to i64.
        //
        // Emit a tiny static comparator function into the module on first
        // use (one for integers, one for strings).

        auto getOrEmitIntCmp = [&]() -> llvm::Function* {
            const char* name = "__omsc_cmp_i64_asc";
            if (auto* fn = module->getFunction(name))
                return fn;
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            auto* cmpTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context),
                                                   {ptrTy, ptrTy}, false);
            auto* fn = llvm::Function::Create(cmpTy, llvm::Function::InternalLinkage,
                                              name, module.get());
            fn->addFnAttr(llvm::Attribute::NoUnwind);
            fn->addFnAttr(llvm::Attribute::WillReturn);
            fn->addFnAttr(llvm::Attribute::NoFree);
            fn->addFnAttr(llvm::Attribute::NoSync);
            fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
                *context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
            // Comparator body: load two i64s, return (a > b) - (a < b)
            auto savedIP = builder->saveIP();
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            builder->SetInsertPoint(entry);
            auto* aPtr = fn->getArg(0);
            auto* bPtr = fn->getArg(1);
            auto* a = builder->CreateAlignedLoad(getDefaultType(), aPtr, llvm::MaybeAlign(8), "a");
            auto* b = builder->CreateAlignedLoad(getDefaultType(), bPtr, llvm::MaybeAlign(8), "b");
            auto* gt = builder->CreateZExt(builder->CreateICmpSGT(a, b, "gt"),
                                           llvm::Type::getInt32Ty(*context));
            auto* lt = builder->CreateZExt(builder->CreateICmpSLT(a, b, "lt"),
                                           llvm::Type::getInt32Ty(*context));
            builder->CreateRet(builder->CreateSub(gt, lt, "cmp"));
            builder->restoreIP(savedIP);
            return fn;
        };

        auto getOrEmitStrCmp = [&]() -> llvm::Function* {
            const char* name = "__omsc_cmp_str_asc";
            if (auto* fn = module->getFunction(name))
                return fn;
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            auto* cmpTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context),
                                                   {ptrTy, ptrTy}, false);
            auto* fn = llvm::Function::Create(cmpTy, llvm::Function::InternalLinkage,
                                              name, module.get());
            fn->addFnAttr(llvm::Attribute::NoUnwind);
            fn->addFnAttr(llvm::Attribute::WillReturn);
            fn->addFnAttr(llvm::Attribute::NoFree);
            fn->addFnAttr(llvm::Attribute::NoSync);
            // String comparator: load i64 pointers, cast to char*, strcmp
            auto savedIP = builder->saveIP();
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            builder->SetInsertPoint(entry);
            auto* aSlotPtr = fn->getArg(0);  // ptr to i64 slot
            auto* bSlotPtr = fn->getArg(1);  // ptr to i64 slot
            auto* aI64 = builder->CreateAlignedLoad(getDefaultType(), aSlotPtr, llvm::MaybeAlign(8), "a.i64");
            auto* bI64 = builder->CreateAlignedLoad(getDefaultType(), bSlotPtr, llvm::MaybeAlign(8), "b.i64");
            auto* aStr = builder->CreateIntToPtr(aI64, ptrTy, "a.str");
            auto* bStr = builder->CreateIntToPtr(bI64, ptrTy, "b.str");
            auto* result = builder->CreateCall(getOrDeclareStrcmp(), {aStr, bStr}, "cmp");
            builder->CreateRet(result);
            builder->restoreIP(savedIP);
            return fn;
        };

        llvm::Function* comparator = sortStrings ? getOrEmitStrCmp() : getOrEmitIntCmp();

        // Data pointer: skip the length header (element 0 is the count).
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* dataPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, one, "sort.data");
        llvm::Value* elemSize = llvm::ConstantInt::get(getDefaultType(), 8); // sizeof(i64)
        builder->CreateCall(getOrDeclareQsort(), {dataPtr, arrLen, elemSize, comparator});
        builder->CreateBr(skipBB);
        builder->SetInsertPoint(skipBB);
        return arrArg; // Return the array itself
    }

    if (bid == BuiltinId::ARRAY_FILL) {
        validateArgCount(expr, "array_fill", 2);
        llvm::Value* sizeArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        sizeArg = toDefaultType(sizeArg);
        valArg = toDefaultType(valArg);
        // Clamp negative sizes to 0 to prevent integer overflow in the
        // allocation size calculation (negative * 8 wraps to a huge unsigned
        // value, causing malloc to over-allocate or fail).
        {
            llvm::Value* zeroClamp = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* isNeg = builder->CreateICmpSLT(sizeArg, zeroClamp, "fill.isneg");
            sizeArg = builder->CreateSelect(isNeg, zeroClamp, sizeArg, "fill.clamp");
        }
        // Allocate: (size + 1) * 8 bytes.  Header slot stores the length.
        llvm::Value* slots = builder->CreateAdd(sizeArg, llvm::ConstantInt::get(getDefaultType(), 1), "fill.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Optimization: when filling with zero, use calloc() instead of
        // malloc() + loop.  calloc() lets the OS provide pre-zeroed pages
        // via virtual memory (mmap MAP_ANONYMOUS), avoiding the loop entirely
        // for large allocations — identical to what C's calloc() gives.
        bool isZeroFill = false;
        if (auto* constVal = llvm::dyn_cast<llvm::ConstantInt>(valArg)) {
            isZeroFill = constVal->isZero();
        }

        llvm::Value* buf;
        if (isZeroFill) {
            // calloc(slots, 8) returns pre-zeroed memory — both the header
            // (length = 0) and all element slots are zero.  We only need to
            // fix up the header with the correct length.
            buf = builder->CreateCall(getOrDeclareCalloc(), {slots, eight}, "fill.buf");
            llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
            // Store length in header (calloc zeroed it; overwrite with actual size)
            auto* fillLenSt = builder->CreateStore(sizeArg, buf);
            fillLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        } else {
            llvm::Value* bytes = builder->CreateMul(slots, eight, "fill.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
            buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "fill.buf");
            // Store length
            auto* fillLenSt2 = builder->CreateStore(sizeArg, buf);
            fillLenSt2->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
            // Fill loop
            llvm::Function* function = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock* preheader = builder->GetInsertBlock();
            llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "fill.loop", function);
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "fill.body", function);
            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "fill.done", function);
            llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
            attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
            builder->SetInsertPoint(loopBB);
            llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "fill.idx");
            idx->addIncoming(zero, preheader);
            llvm::Value* cond = builder->CreateICmpULT(idx, sizeArg, "fill.cond");
            builder->CreateCondBr(cond, bodyBB, doneBB);
            builder->SetInsertPoint(bodyBB);
            llvm::Value* elemIdx = builder->CreateAdd(idx, one, "fill.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
            llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, elemIdx, "fill.elemptr");
            auto* fillElemSt = builder->CreateStore(valArg, elemPtr);
            fillElemSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            llvm::Value* nextIdx = builder->CreateAdd(idx, one, "fill.next", /*HasNUW=*/true, /*HasNSW=*/true);
            idx->addIncoming(nextIdx, bodyBB);
            auto* backBr_2379 = builder->CreateBr(loopBB);
            if (optimizationLevel >= OptimizationLevel::O1) {
                llvm::SmallVector<llvm::Metadata*, 4> mds;
                mds.push_back(nullptr);
                mds.push_back(llvm::MDNode::get(*context,
                    {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
                // Enable vectorization: the fill loop is trivially parallel
                // (every iteration writes to a unique index) and benefits from
                // SIMD store widening at O2+.
                if (optimizationLevel >= OptimizationLevel::O2) {
                    mds.push_back(llvm::MDNode::get(*context,
                        {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                         llvm::ConstantAsMetadata::get(builder->getTrue())}));
                }
                llvm::MDNode* md = llvm::MDNode::get(*context, mds);
                md->replaceOperandWith(0, md);
                backBr_2379->setMetadata(llvm::LLVMContext::MD_loop, md);
            }
            builder->SetInsertPoint(doneBB);
        }
        return builder->CreatePtrToInt(buf, getDefaultType(), "fill.result");
    }

    if (bid == BuiltinId::ARRAY_CONCAT) {
        validateArgCount(expr, "array_concat", 2);
        llvm::Value* arr1Arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arr2Arg = generateExpression(expr->arguments[1].get());
        arr1Arg = toDefaultType(arr1Arg);
        arr2Arg = toDefaultType(arr2Arg);
        llvm::Value* arr1Ptr = builder->CreateIntToPtr(arr1Arg, llvm::PointerType::getUnqual(*context), "aconcat.ptr1");
        llvm::Value* arr2Ptr = builder->CreateIntToPtr(arr2Arg, llvm::PointerType::getUnqual(*context), "aconcat.ptr2");
        auto* acatLen1Load = builder->CreateAlignedLoad(getDefaultType(), arr1Ptr, llvm::MaybeAlign(8), "aconcat.len1");
        acatLen1Load->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        acatLen1Load->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* len1 = acatLen1Load;
        auto* acatLen2Load = builder->CreateAlignedLoad(getDefaultType(), arr2Ptr, llvm::MaybeAlign(8), "aconcat.len2");
        acatLen2Load->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        acatLen2Load->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* len2 = acatLen2Load;
        llvm::Value* totalLen = builder->CreateAdd(len1, len2, "aconcat.total", /*HasNUW=*/true, /*HasNSW=*/true);
        // Allocate: (totalLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "aconcat.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, llvm::ConstantInt::get(getDefaultType(), 8), "aconcat.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "aconcat.buf");
        // Store length
        auto* aconcatLenSt = builder->CreateStore(totalLen, buf);
        aconcatLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        // Copy arr1 elements (len1 * 8 bytes starting at arr1[1])
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* src1 = builder->CreateInBoundsGEP(getDefaultType(), arr1Ptr, one, "aconcat.src1");
        llvm::Value* dst1 = builder->CreateInBoundsGEP(getDefaultType(), buf, one, "aconcat.dst1");
        llvm::Value* copy1Size = builder->CreateMul(len1, eight, "aconcat.copy1size", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemcpy(), {dst1, src1, copy1Size});
        // Copy arr2 elements
        llvm::Value* dst2Idx = builder->CreateAdd(len1, one, "aconcat.dst2idx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* dst2 = builder->CreateInBoundsGEP(getDefaultType(), buf, dst2Idx, "aconcat.dst2");
        llvm::Value* src2 = builder->CreateInBoundsGEP(getDefaultType(), arr2Ptr, one, "aconcat.src2");
        llvm::Value* copy2Size = builder->CreateMul(len2, eight, "aconcat.copy2size", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemcpy(), {dst2, src2, copy2Size});
        return builder->CreatePtrToInt(buf, getDefaultType(), "aconcat.result");
    }

    if (bid == BuiltinId::ARRAY_SLICE) {
        validateArgCount(expr, "array_slice", 3);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* startArg = generateExpression(expr->arguments[1].get());
        llvm::Value* endArg = generateExpression(expr->arguments[2].get());
        arrArg = toDefaultType(arrArg);
        startArg = toDefaultType(startArg);
        endArg = toDefaultType(endArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "slice.arrptr");
        auto* sliceLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "slice.arrlen");
        sliceLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        sliceLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = sliceLenLoad;
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        // Clamp start: max(0, min(start, arrLen))
        llvm::Value* startNeg = builder->CreateICmpSLT(startArg, zero, "slice.startneg");
        startArg = builder->CreateSelect(startNeg, zero, startArg, "slice.startclamp");
        llvm::Value* startOver = builder->CreateICmpSGT(startArg, arrLen, "slice.startover");
        startArg = builder->CreateSelect(startOver, arrLen, startArg, "slice.startfinal");
        // Clamp end: max(start, min(end, arrLen))
        llvm::Value* endNeg = builder->CreateICmpSLT(endArg, startArg, "slice.endneg");
        endArg = builder->CreateSelect(endNeg, startArg, endArg, "slice.endclamp");
        llvm::Value* endOver = builder->CreateICmpSGT(endArg, arrLen, "slice.endover");
        endArg = builder->CreateSelect(endOver, arrLen, endArg, "slice.endfinal");

        llvm::Value* sliceLen = builder->CreateSub(endArg, startArg, "slice.len");
        // Allocate: (sliceLen + 1) * 8
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(sliceLen, one, "slice.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "slice.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "slice.buf");
        auto* sliceLenSt = builder->CreateStore(sliceLen, buf);
        sliceLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        // Copy elements: arr[start+1..end+1) to buf[1..)
        llvm::Value* srcIdx = builder->CreateAdd(startArg, one, "slice.srcidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* src = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, srcIdx, "slice.src");
        llvm::Value* dst = builder->CreateInBoundsGEP(getDefaultType(), buf, one, "slice.dst");
        llvm::Value* copySize = builder->CreateMul(sliceLen, eight, "slice.copysize", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemcpy(), {dst, src, copySize});
        return builder->CreatePtrToInt(buf, getDefaultType(), "slice.result");
    }

    if (bid == BuiltinId::ARRAY_COPY) {
        validateArgCount(expr, "array_copy", 1);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "acopy.arrptr");
        auto* acopyLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "acopy.len");
        acopyLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        acopyLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = acopyLenLoad;
        // Allocate: (length + 1) * 8 bytes
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "acopy.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "acopy.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "acopy.buf");
        // Copy all data: (length + 1) * 8 bytes
        builder->CreateCall(getOrDeclareMemcpy(), {buf, arrPtr, bytes});
        return builder->CreatePtrToInt(buf, getDefaultType(), "acopy.result");
    }

    if (bid == BuiltinId::ARRAY_REMOVE) {
        validateArgCount(expr, "array_remove", 2);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* idxArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        idxArg = toDefaultType(idxArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "aremove.arrptr");
        auto* aremLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "aremove.len");
        aremLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        aremLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = aremLenLoad;
        // Bounds check: 0 <= idx < length
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* inBounds = builder->CreateICmpSLT(idxArg, arrLen, "aremove.inbounds");
        llvm::Value* notNeg = builder->CreateICmpSGE(idxArg, zero, "aremove.notneg");
        llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "aremove.valid");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "aremove.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "aremove.fail", function);
        // OOB is extremely unlikely.
        llvm::MDNode* removeW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(valid, okBB, failBB, removeW);
        // Out-of-bounds: print error and abort
        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg =
            builder->CreateGlobalString("Runtime error: array_remove index out of bounds\n", "aremove_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();
        // In-bounds: save removed value, shift elements left, decrement length
        builder->SetInsertPoint(okBB);
        llvm::Value* elemOffset = builder->CreateAdd(idxArg, one, "aremove.elemoff", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, elemOffset, "aremove.elemptr");
        llvm::Value* removedVal = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "aremove.removed");
        // TBAA: removed value is an array element, never aliases the length header.
        llvm::cast<llvm::LoadInst>(removedVal)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        // memmove(&arr[idx+1], &arr[idx+2], (length - idx - 1) * 8)
        llvm::Value* srcOffset =
            builder->CreateAdd(idxArg, llvm::ConstantInt::get(getDefaultType(), 2), "aremove.srcoff", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* srcPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, srcOffset, "aremove.srcptr");
        llvm::Value* shiftCount =
            builder->CreateSub(arrLen, builder->CreateAdd(idxArg, one, "aremove.idxp1", /*HasNUW=*/true, /*HasNSW=*/true), "aremove.shiftcnt", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* shiftBytes = builder->CreateMul(shiftCount, eight, "aremove.shiftbytes", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemmove(), {elemPtr, srcPtr, shiftBytes});
        // Decrement length
        llvm::Value* newLen = builder->CreateSub(arrLen, one, "aremove.newlen", /*HasNUW=*/true, /*HasNSW=*/true);
        auto* newLenSt = builder->CreateStore(newLen, arrPtr);
        newLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        return removedVal;
    }

    // -----------------------------------------------------------------------
    // array_map(arr, "fn_name") — apply named function to each element, return new array
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_MAP) {
        validateArgCount(expr, "array_map", 2);
        // The second argument must be a string literal (function name)
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("array_map: second argument must be a string literal (function name)", expr);
        }
        std::string fnName = fnNameLit->stringValue;
        // Look up the target function in the LLVM module
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second) {
            codegenError("array_map: unknown function '" + fnName + "'", expr);
        }
        llvm::Function* mapFn = calleeIt->second;
        if (mapFn->arg_size() < 1) {
            codegenError("array_map: function '" + fnName + "' must accept at least 1 argument", expr);
        }

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "amap.arrptr");
        auto* amapLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "amap.len");
        amapLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        amapLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = amapLenLoad;

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Allocate result array: (arrLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "amap.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "amap.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "amap.buf");
        auto* amapLenSt = builder->CreateStore(arrLen, buf);
        amapLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);

        // Loop: for each element, call mapFn and store result
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "amap.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "amap.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "amap.done", function);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "amap.idx");
        idx->addIncoming(zero, preheader);
        // Unsigned: idx starts at 0, arrLen ≥ 0 (range metadata).
        llvm::Value* cond = builder->CreateICmpULT(idx, arrLen, "amap.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        // Load element from source: arrPtr[idx + 1]
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "amap.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* srcPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, elemIdx, "amap.srcptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), srcPtr, llvm::MaybeAlign(8), "amap.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        // Call the map function with the element
        llvm::Value* mapped = builder->CreateCall(mapFn, {elem}, "amap.mapped");
        mapped = toDefaultType(mapped);
        // Store into result: buf[idx + 1]
        llvm::Value* dstPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, elemIdx, "amap.dstptr");
        auto* mappedStore = builder->CreateStore(mapped, dstPtr);
        mappedStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "amap.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "amap.result");
    }

    // -----------------------------------------------------------------------
    // array_filter(arr, "fn_name") — return new array of elements where fn returns non-zero
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_FILTER) {
        validateArgCount(expr, "array_filter", 2);
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("array_filter: second argument must be a string literal (function name)", expr);
        }
        std::string fnName = fnNameLit->stringValue;
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second) {
            codegenError("array_filter: unknown function '" + fnName + "'", expr);
        }
        llvm::Function* filterFn = calleeIt->second;
        if (filterFn->arg_size() < 1) {
            codegenError("array_filter: function '" + fnName + "' must accept at least 1 argument", expr);
        }

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "afilt.arrptr");
        auto* afiltLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "afilt.len");
        afiltLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        afiltLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = afiltLenLoad;

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Allocate result array with max possible size: (arrLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "afilt.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "afilt.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "afilt.buf");
        // Initialize length to 0 (will be updated as we add elements)
        auto* afiltInitSt = builder->CreateStore(zero, buf);
        afiltInitSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);

        // Loop: for each element, call filterFn; if non-zero, add to result
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "afilt.loop", function);
        llvm::BasicBlock* testBB = llvm::BasicBlock::Create(*context, "afilt.test", function);
        llvm::BasicBlock* addBB = llvm::BasicBlock::Create(*context, "afilt.add", function);
        llvm::BasicBlock* incBB = llvm::BasicBlock::Create(*context, "afilt.inc", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "afilt.done", function);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "afilt.idx");
        idx->addIncoming(zero, preheader);
        llvm::PHINode* outIdx = builder->CreatePHI(getDefaultType(), 2, "afilt.outidx");
        outIdx->addIncoming(zero, preheader);
        // Unsigned: idx starts at 0, arrLen ≥ 0 (range metadata).
        llvm::Value* cond = builder->CreateICmpULT(idx, arrLen, "afilt.cond");
        builder->CreateCondBr(cond, testBB, doneBB);

        builder->SetInsertPoint(testBB);
        // Load element from source
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "afilt.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* srcPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, elemIdx, "afilt.srcptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), srcPtr, llvm::MaybeAlign(8), "afilt.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        // Call the filter function
        llvm::Value* result = builder->CreateCall(filterFn, {elem}, "afilt.result");
        result = toDefaultType(result);
        llvm::Value* keep = builder->CreateICmpNE(result, zero, "afilt.keep");
        builder->CreateCondBr(keep, addBB, incBB);

        // Add element to result
        builder->SetInsertPoint(addBB);
        llvm::Value* dstIdx = builder->CreateAdd(outIdx, one, "afilt.dstidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* dstPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, dstIdx, "afilt.dstptr");
        auto* elemStore = builder->CreateStore(elem, dstPtr);
        elemStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* newOutIdx = builder->CreateAdd(outIdx, one, "afilt.newoutidx", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateBr(incBB);

        // Increment loop counter
        builder->SetInsertPoint(incBB);
        llvm::PHINode* outIdxMerge = builder->CreatePHI(getDefaultType(), 2, "afilt.outmerge");
        outIdxMerge->addIncoming(outIdx, testBB);
        outIdxMerge->addIncoming(newOutIdx, addBB);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "afilt.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, incBB);
        outIdx->addIncoming(outIdxMerge, incBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // Done: store final length
        builder->SetInsertPoint(doneBB);
        auto* afiltLenSt = builder->CreateStore(outIdx, buf);
        afiltLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        return builder->CreatePtrToInt(buf, getDefaultType(), "afilt.result");
    }

    // -----------------------------------------------------------------------
    // array_reduce(arr, "fn_name", initial) — reduce array to single value
    //   fn_name must be a function that takes 2 arguments: (accumulator, element)
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_REDUCE) {
        validateArgCount(expr, "array_reduce", 3);
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("array_reduce: second argument must be a string literal (function name)", expr);
        }
        std::string fnName = fnNameLit->stringValue;
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second) {
            codegenError("array_reduce: unknown function '" + fnName + "'", expr);
        }
        llvm::Function* reduceFn = calleeIt->second;
        if (reduceFn->arg_size() < 2) {
            codegenError("array_reduce: function '" + fnName +
                             "' must accept at least 2 arguments (accumulator, element)",
                         expr);
        }

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* initVal = generateExpression(expr->arguments[2].get());
        arrArg = toDefaultType(arrArg);
        initVal = toDefaultType(initVal);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "areduce.arrptr");
        auto* aredLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "areduce.len");
        aredLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        aredLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = aredLenLoad;

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);

        // Loop: accumulate with fn(acc, element)
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "areduce.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "areduce.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "areduce.done", function);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "areduce.idx");
        idx->addIncoming(zero, preheader);
        llvm::PHINode* acc = builder->CreatePHI(getDefaultType(), 2, "areduce.acc");
        acc->addIncoming(initVal, preheader);
        // Unsigned: idx starts at 0, arrLen ≥ 0 (range metadata).
        llvm::Value* cond = builder->CreateICmpULT(idx, arrLen, "areduce.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        // Load element from source: arrPtr[idx + 1]
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "areduce.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* srcPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, elemIdx, "areduce.srcptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), srcPtr, llvm::MaybeAlign(8), "areduce.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        // Call reduce function: fn(accumulator, element)
        llvm::Value* newAcc = builder->CreateCall(reduceFn, {acc, elem}, "areduce.newacc");
        newAcc = toDefaultType(newAcc);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "areduce.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        acc->addIncoming(newAcc, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // Done: return final accumulator
        builder->SetInsertPoint(doneBB);
        return acc;
    }

    // -----------------------------------------------------------------------
    // array_min(arr) — return the minimum element of an array (0 for empty)
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_MIN) {
        validateArgCount(expr, "array_min", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "amin.arrptr");
        auto* aminLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "amin.len");
        aminLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        aminLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* length = aminLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* emptyBB = llvm::BasicBlock::Create(*context, "amin.empty", function);
        llvm::BasicBlock* initBB = llvm::BasicBlock::Create(*context, "amin.init", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "amin.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "amin.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "amin.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);

        // length has !range metadata [0, i64max], so len <= 0 ≡ len == 0.
        // ICmpEQ is more direct and avoids LLVM having to prove non-negativity.
        llvm::Value* isEmpty = builder->CreateICmpEQ(length, zero, "amin.isempty");
        builder->CreateCondBr(isEmpty, emptyBB, initBB);

        // Empty: return 0
        builder->SetInsertPoint(emptyBB);
        builder->CreateBr(doneBB);

        // Init: load first element as initial min
        builder->SetInsertPoint(initBB);
        llvm::Value* firstPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, one, "amin.firstptr");
        llvm::Value* firstElem = builder->CreateAlignedLoad(getDefaultType(), firstPtr, llvm::MaybeAlign(8), "amin.first");
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // Loop header
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* curMin = builder->CreatePHI(getDefaultType(), 2, "amin.curmin");
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "amin.idx");
        curMin->addIncoming(firstElem, initBB);
        idx->addIncoming(one, initBB);

        // Unsigned: idx starts at 1, length ≥ 0 (range metadata) → UGE is equivalent
        // to SGE but gives SCEV a clean unsigned trip count for the vectorizer.
        llvm::Value* doneCheck = builder->CreateICmpUGE(idx, length, "amin.donecheck");
        builder->CreateCondBr(doneCheck, doneBB, bodyBB);

        // Body: compare and update min
        builder->SetInsertPoint(bodyBB);
        llvm::Value* offset = builder->CreateAdd(idx, one, "amin.offset", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "amin.elemptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "amin.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        // Use llvm.smin intrinsic instead of icmp+select.  The intrinsic:
        //  1. Generates a single cmov/SIMD-min instruction rather than a branch
        //  2. Is recognized by the vectorizer as a min-reduction, enabling
        //     SIMD vpcmpq+vpminsd (AVX2) or vpminq (AVX-512) instructions
        llvm::Function* sminFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smin, {getDefaultType()});
        llvm::Value* newMin = builder->CreateCall(sminFn, {elem, curMin}, "amin.newmin");
        llvm::Value* newIdx = builder->CreateAdd(idx, one, "amin.newidx", /*HasNUW=*/true, /*HasNSW=*/true);
        curMin->addIncoming(newMin, bodyBB);
        idx->addIncoming(newIdx, bodyBB);
        auto* backBr_2825 = builder->CreateBr(loopBB);
        if (optimizationLevel >= OptimizationLevel::O1) {
            llvm::SmallVector<llvm::Metadata*, 4> mds;
            mds.push_back(nullptr);
            mds.push_back(llvm::MDNode::get(*context,
                {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
            // Vectorization: amin is a reduction loop — each iteration is
            // independent (depends only on curMin which tracks the minimum).
            // With llvm.smin intrinsic + vectorize.enable, LLVM emits a
            // vectorized min-reduction (vpminsd/vpminq on x86) and then
            // reduces the vector at the end with a single horizontal min.
            if (optimizationLevel >= OptimizationLevel::O2) {
                mds.push_back(llvm::MDNode::get(*context,
                    {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
                mds.push_back(llvm::MDNode::get(*context,
                    {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 4))}));
            }
            llvm::MDNode* md = llvm::MDNode::get(*context, mds);
            md->replaceOperandWith(0, md);
            backBr_2825->setMetadata(llvm::LLVMContext::MD_loop, md);
        }

        // Done: return result
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "amin.result");
        result->addIncoming(zero, emptyBB);
        result->addIncoming(curMin, loopBB);
        return result;
    }

    // -----------------------------------------------------------------------
    // array_max(arr) — return the maximum element of an array (0 for empty)
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_MAX) {
        validateArgCount(expr, "array_max", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "amax.arrptr");
        auto* amaxLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "amax.len");
        amaxLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        amaxLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* length = amaxLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* emptyBB = llvm::BasicBlock::Create(*context, "amax.empty", function);
        llvm::BasicBlock* initBB = llvm::BasicBlock::Create(*context, "amax.init", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "amax.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "amax.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "amax.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);

        // length has !range metadata [0, i64max], so len <= 0 ≡ len == 0.
        llvm::Value* isEmpty = builder->CreateICmpEQ(length, zero, "amax.isempty");
        builder->CreateCondBr(isEmpty, emptyBB, initBB);

        // Empty: return 0
        builder->SetInsertPoint(emptyBB);
        builder->CreateBr(doneBB);

        // Init: load first element as initial max
        builder->SetInsertPoint(initBB);
        llvm::Value* firstPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, one, "amax.firstptr");
        llvm::Value* firstElem = builder->CreateAlignedLoad(getDefaultType(), firstPtr, llvm::MaybeAlign(8), "amax.first");
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // Loop header
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* curMax = builder->CreatePHI(getDefaultType(), 2, "amax.curmax");
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "amax.idx");
        curMax->addIncoming(firstElem, initBB);
        idx->addIncoming(one, initBB);

        // Unsigned: idx starts at 1, length ≥ 0 (range metadata).
        llvm::Value* doneCheck = builder->CreateICmpUGE(idx, length, "amax.donecheck");
        builder->CreateCondBr(doneCheck, doneBB, bodyBB);

        // Body: compare and update max
        builder->SetInsertPoint(bodyBB);
        llvm::Value* offset = builder->CreateAdd(idx, one, "amax.offset", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "amax.elemptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "amax.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        // Use llvm.smax intrinsic instead of icmp+select for the same reasons
        // as amin: direct SIMD max instruction + vectorizer-friendly reduction.
        llvm::Function* smaxFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smax, {getDefaultType()});
        llvm::Value* newMax = builder->CreateCall(smaxFn, {elem, curMax}, "amax.newmax");
        // Reuse offset (= idx+1, nsw+nuw — no signed wrap, no unsigned wrap) as
        // the loop induction increment. This eliminates a redundant add instruction
        // and gives SCEV the same flags as the GEP offset for accurate trip-count
        // modeling.
        curMax->addIncoming(newMax, bodyBB);
        idx->addIncoming(offset, bodyBB);
        auto* backBr_2892 = builder->CreateBr(loopBB);
        if (optimizationLevel >= OptimizationLevel::O1) {
            llvm::SmallVector<llvm::Metadata*, 4> mds;
            mds.push_back(nullptr);
            mds.push_back(llvm::MDNode::get(*context,
                {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
            if (optimizationLevel >= OptimizationLevel::O2) {
                mds.push_back(llvm::MDNode::get(*context,
                    {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
                mds.push_back(llvm::MDNode::get(*context,
                    {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 4))}));
            }
            llvm::MDNode* md = llvm::MDNode::get(*context, mds);
            md->replaceOperandWith(0, md);
            backBr_2892->setMetadata(llvm::LLVMContext::MD_loop, md);
        }

        // Done: return result
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "amax.result");
        result->addIncoming(zero, emptyBB);
        result->addIncoming(curMax, loopBB);
        return result;
    }

    // -----------------------------------------------------------------------
    // array_any(arr, "fn_name") — return 1 if fn returns non-zero for any element
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_ANY) {
        validateArgCount(expr, "array_any", 2);
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("array_any: second argument must be a string literal (function name)", expr);
        }
        std::string fnName = fnNameLit->stringValue;
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second) {
            codegenError("array_any: unknown function '" + fnName + "'", expr);
        }
        llvm::Function* predFn = calleeIt->second;
        if (predFn->arg_size() < 1) {
            codegenError("array_any: function '" + fnName + "' must accept at least 1 argument", expr);
        }

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "aany.arrptr");
        auto* aanyLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "aany.len");
        aanyLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        aanyLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* length = aanyLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "aany.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "aany.body", function);
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "aany.found", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "aany.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "aany.idx");
        idx->addIncoming(zero, entryBB);
        // Unsigned: idx starts at 0, length ≥ 0 (range metadata).
        llvm::Value* doneCheck = builder->CreateICmpUGE(idx, length, "aany.donecheck");
        builder->CreateCondBr(doneCheck, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* offset = builder->CreateAdd(idx, one, "aany.offset", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "aany.elemptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "aany.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* predResult = builder->CreateCall(predFn, {elem}, "aany.pred");
        predResult = toDefaultType(predResult);
        llvm::Value* isNonZero = builder->CreateICmpNE(predResult, zero, "aany.nz");
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        // Eliminates a redundant add and provides SCEV with tight nsw+nuw flags.
        idx->addIncoming(offset, bodyBB);
        builder->CreateCondBr(isNonZero, foundBB, loopBB);

        builder->SetInsertPoint(foundBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "aany.result");
        result->addIncoming(zero, loopBB);
        result->addIncoming(one, foundBB);
        return result;
    }

    // -----------------------------------------------------------------------
    // array_every(arr, "fn_name") — return 1 if fn returns non-zero for ALL elements
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_EVERY) {
        validateArgCount(expr, "array_every", 2);
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("array_every: second argument must be a string literal (function name)", expr);
        }
        std::string fnName = fnNameLit->stringValue;
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second) {
            codegenError("array_every: unknown function '" + fnName + "'", expr);
        }
        llvm::Function* predFn = calleeIt->second;
        if (predFn->arg_size() < 1) {
            codegenError("array_every: function '" + fnName + "' must accept at least 1 argument", expr);
        }

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "aevery.arrptr");
        auto* aeveryLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "aevery.len");
        aeveryLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        aeveryLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* length = aeveryLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "aevery.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "aevery.body", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "aevery.fail", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "aevery.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "aevery.idx");
        idx->addIncoming(zero, entryBB);
        // Unsigned: idx starts at 0, length ≥ 0 (range metadata).
        llvm::Value* doneCheck = builder->CreateICmpUGE(idx, length, "aevery.donecheck");
        builder->CreateCondBr(doneCheck, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* offset = builder->CreateAdd(idx, one, "aevery.offset", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "aevery.elemptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "aevery.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* predResult = builder->CreateCall(predFn, {elem}, "aevery.pred");
        predResult = toDefaultType(predResult);
        llvm::Value* isZero = builder->CreateICmpEQ(predResult, zero, "aevery.iszero");
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        idx->addIncoming(offset, bodyBB);
        builder->CreateCondBr(isZero, failBB, loopBB);

        builder->SetInsertPoint(failBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "aevery.result");
        result->addIncoming(one, loopBB);
        result->addIncoming(zero, failBB);
        return result;
    }

    // -----------------------------------------------------------------------
    // array_find(arr, value) — return index of first matching element, or -1
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_FIND) {
        validateArgCount(expr, "array_find", 2);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* target = generateExpression(expr->arguments[1].get());
        target = toDefaultType(target);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "afind.arrptr");
        auto* afindLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "afind.len");
        afindLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        afindLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* length = afindLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "afind.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "afind.body", function);
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "afind.found", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "afind.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "afind.idx");
        idx->addIncoming(zero, entryBB);

        // Unsigned: idx starts at 0, length ≥ 0 (range metadata).
        llvm::Value* doneCheck = builder->CreateICmpUGE(idx, length, "afind.donecheck");
        builder->CreateCondBr(doneCheck, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* offset = builder->CreateAdd(idx, one, "afind.offset", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "afind.elemptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "afind.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* isEqual = builder->CreateICmpEQ(elem, target, "afind.iseq");
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        idx->addIncoming(offset, bodyBB);
        builder->CreateCondBr(isEqual, foundBB, loopBB);

        builder->SetInsertPoint(foundBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "afind.result");
        result->addIncoming(negOne, loopBB);
        result->addIncoming(idx, foundBB);
        return result;
    }

    // -----------------------------------------------------------------------
    // array_count(arr, "fn_name") — count elements for which fn returns non-zero
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_COUNT) {
        validateArgCount(expr, "array_count", 2);
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("array_count: second argument must be a string literal (function name)", expr);
        }
        std::string fnName = fnNameLit->stringValue;
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second) {
            codegenError("array_count: unknown function '" + fnName + "'", expr);
        }
        llvm::Function* predFn = calleeIt->second;
        if (predFn->arg_size() < 1) {
            codegenError("array_count: function '" + fnName + "' must accept at least 1 argument", expr);
        }

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "acnt.arrptr");
        auto* acntLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "acnt.len");
        acntLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        acntLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* length = acntLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "acnt.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "acnt.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "acnt.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* acc = builder->CreatePHI(getDefaultType(), 2, "acnt.acc");
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "acnt.idx");
        acc->addIncoming(zero, entryBB);
        idx->addIncoming(zero, entryBB);
        // Unsigned: idx starts at 0, length ≥ 0 (range metadata).
        llvm::Value* doneCheck = builder->CreateICmpUGE(idx, length, "acnt.donecheck");
        builder->CreateCondBr(doneCheck, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* offset = builder->CreateAdd(idx, one, "acnt.offset", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "acnt.elemptr");
        llvm::Value* elem = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "acnt.elem");
        llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* predResult = builder->CreateCall(predFn, {elem}, "acnt.pred");
        predResult = toDefaultType(predResult);
        llvm::Value* isNonZero = builder->CreateICmpNE(predResult, zero, "acnt.nz");
        llvm::Value* incr = builder->CreateZExt(isNonZero, getDefaultType(), "acnt.incr");
        // acnt.newacc: acc is in [0, length], incr in {0,1}; sum ≤ INT64_MAX so
        // both nsw and nuw are safe and let SCEV prove the accumulator is non-negative.
        llvm::Value* newAcc = builder->CreateAdd(acc, incr, "acnt.newacc", /*HasNUW=*/true, /*HasNSW=*/true);
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(offset, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        return acc;
    }

    // -----------------------------------------------------------------------
    // println(x) — print value followed by newline (same as print but explicit)
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::PRINTLN) {
        validateArgCount(expr, "println", 1);
        Expression* argExpr = expr->arguments[0].get();
        // Fast path: string literal → use puts() instead of printf("%s\n", ...)
        // puts() appends a newline automatically and avoids format-string parsing.
        if (argExpr->type == ASTNodeType::LITERAL_EXPR) {
            auto* lit = static_cast<LiteralExpr*>(argExpr);
            if (lit->literalType == LiteralExpr::LiteralType::STRING) {
                llvm::Value* strVal = builder->CreateGlobalString(lit->stringValue, "println.lit");
                builder->CreateCall(getOrDeclarePuts(), {strVal});
                return llvm::ConstantInt::get(getDefaultType(), 0);
            }
        }
        llvm::Value* arg = generateExpression(argExpr);
        if (arg->getType()->isDoubleTy()) {
            llvm::GlobalVariable* floatFmt = module->getGlobalVariable("println_float_fmt", true);
            if (!floatFmt) {
                floatFmt = builder->CreateGlobalString("%g\n", "println_float_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {floatFmt, arg});
        } else if (arg->getType()->isPointerTy() || isStringExpr(argExpr)) {
            if (!arg->getType()->isPointerTy()) {
                arg = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "println.str.ptr");
            }
            // Use puts() instead of printf("%s\n", ...) — puts appends a
            // newline automatically and avoids format-string parsing overhead.
            builder->CreateCall(getOrDeclarePuts(), {arg});
        } else {
            llvm::GlobalVariable* formatStr = module->getGlobalVariable("println_fmt", true);
            if (!formatStr) {
                formatStr = builder->CreateGlobalString("%lld\n", "println_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {formatStr, arg});
        }
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // -----------------------------------------------------------------------
    // write(x) — print without trailing newline
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::WRITE) {
        validateArgCount(expr, "write", 1);
        Expression* argExpr = expr->arguments[0].get();
        llvm::Value* arg = generateExpression(argExpr);
        if (arg->getType()->isDoubleTy()) {
            llvm::GlobalVariable* floatFmt = module->getGlobalVariable("write_float_fmt", true);
            if (!floatFmt) {
                floatFmt = builder->CreateGlobalString("%g", "write_float_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {floatFmt, arg});
        } else if (arg->getType()->isPointerTy() || isStringExpr(argExpr)) {
            if (!arg->getType()->isPointerTy()) {
                arg = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "write.str.ptr");
            }
            // Use fputs(str, stdout) instead of printf("%s", str) to avoid
            // format-string parsing overhead.  fputs writes the string directly
            // to stdout without scanning for % conversion specifiers.
            builder->CreateCall(getOrDeclareFputs(), {arg, getOrDeclareStdout()});
        } else {
            llvm::GlobalVariable* formatStr = module->getGlobalVariable("write_fmt", true);
            if (!formatStr) {
                formatStr = builder->CreateGlobalString("%lld", "write_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {formatStr, arg});
        }
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // -----------------------------------------------------------------------
    // exit_program(code) — terminate the process with the given exit code
    // exit() — terminate with exit code 0 (shorthand alias)
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::EXIT_PROGRAM) {
        llvm::Value* code;
        if (expr->arguments.empty()) {
            // exit() with no args defaults to exit code 0
            code = llvm::ConstantInt::get(getDefaultType(), 0);
        } else if (expr->arguments.size() == 1) {
            code = generateExpression(expr->arguments[0].get());
            code = toDefaultType(code);
        } else {
            codegenError("Built-in function '" + expr->callee + "' expects 0 or 1 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* code32 = builder->CreateTrunc(code, llvm::Type::getInt32Ty(*context), "exit.code");
        builder->CreateCall(getOrDeclareExit(), {code32});
        builder->CreateUnreachable();
        // Create a dead block so subsequent IR generation still has an insert point.
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* deadBB = llvm::BasicBlock::Create(*context, "exit.dead", function);
        builder->SetInsertPoint(deadBB);
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // -----------------------------------------------------------------------
    // random() — returns a pseudo-random integer (seeds once automatically)
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::RANDOM) {
        validateArgCount(expr, "random", 0);
        // Seed on first call via a global flag
        llvm::GlobalVariable* seeded = module->getGlobalVariable("__om_rand_seeded", true);
        if (!seeded) {
            seeded = new llvm::GlobalVariable(
                *module, llvm::Type::getInt32Ty(*context), false, llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "__om_rand_seeded");
        }
        llvm::Value* flag = builder->CreateLoad(llvm::Type::getInt32Ty(*context), seeded, "rand.flag");
        llvm::Value* isZero =
            builder->CreateICmpEQ(flag, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "rand.cmp");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* seedBB = llvm::BasicBlock::Create(*context, "rand.seed", function);
        llvm::BasicBlock* callBB = llvm::BasicBlock::Create(*context, "rand.call", function);

        builder->CreateCondBr(isZero, seedBB, callBB);

        builder->SetInsertPoint(seedBB);
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* t = builder->CreateCall(getOrDeclareTimeFunc(), {nullPtr}, "rand.time");
        llvm::Value* t32 = builder->CreateTrunc(t, llvm::Type::getInt32Ty(*context), "rand.time32");
        builder->CreateCall(getOrDeclareSrand(), {t32});
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1), seeded);
        builder->CreateBr(callBB);

        builder->SetInsertPoint(callBB);
        llvm::Value* r = builder->CreateCall(getOrDeclareRand(), {}, "rand.val");
        return builder->CreateSExt(r, getDefaultType(), "rand.ext");
    }

    // -----------------------------------------------------------------------
    // time() — returns current Unix timestamp in seconds
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::TIME) {
        validateArgCount(expr, "time", 0);
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        return builder->CreateCall(getOrDeclareTimeFunc(), {nullPtr}, "time.val");
    }

    // -----------------------------------------------------------------------
    // sleep(ms) — sleep for given milliseconds
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::SLEEP) {
        validateArgCount(expr, "sleep", 1);
        llvm::Value* ms = generateExpression(expr->arguments[0].get());
        ms = toDefaultType(ms);
        // usleep takes microseconds, so multiply by 1000
        llvm::Value* us = builder->CreateMul(ms, llvm::ConstantInt::get(getDefaultType(), 1000), "sleep.us");
        llvm::Value* us32 = builder->CreateTrunc(us, llvm::Type::getInt32Ty(*context), "sleep.us32");
        builder->CreateCall(getOrDeclareUsleep(), {us32});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // -----------------------------------------------------------------------
    // str_to_int(s) — parse string to integer
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_TO_INT) {
        validateArgCount(expr, "str_to_int", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strtoi.ptr");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* base10 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10);
        return builder->CreateCall(getOrDeclareStrtoll(), {strPtr, nullPtr, base10}, "strtoi.val");
    }

    // -----------------------------------------------------------------------
    // str_to_float(s) — parse string to float
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_TO_FLOAT) {
        validateArgCount(expr, "str_to_float", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strtof.ptr");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        return builder->CreateCall(getOrDeclareStrtod(), {strPtr, nullPtr}, "strtof.val");
    }

    // -----------------------------------------------------------------------
    // str_split(s, delim) — split string by delimiter, returns array of strings
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_SPLIT) {
        validateArgCount(expr, "str_split", 2);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* delimArg = generateExpression(expr->arguments[1].get());

        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "split.str");
        llvm::Value* delimPtr =
            delimArg->getType()->isPointerTy()
                ? delimArg
                : builder->CreateIntToPtr(delimArg, llvm::PointerType::getUnqual(*context), "split.delim");

        // Get the delimiter character (first char of delimiter string)
        auto* delimCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), delimPtr, "split.delimch");
        delimCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* delimChar32 = builder->CreateZExt(delimCharLoad, llvm::Type::getInt32Ty(*context), "split.delimch32");

        // Count delimiters to know array size
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "split.strlen");
        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Count pass
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* countLoopBB = llvm::BasicBlock::Create(*context, "split.countloop", function);
        llvm::BasicBlock* countBodyBB = llvm::BasicBlock::Create(*context, "split.countbody", function);
        llvm::BasicBlock* countIncBB = llvm::BasicBlock::Create(*context, "split.countinc", function);
        llvm::BasicBlock* countDoneBB = llvm::BasicBlock::Create(*context, "split.countdone", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        builder->CreateBr(countLoopBB);

        builder->SetInsertPoint(countLoopBB);
        llvm::PHINode* ci = builder->CreatePHI(getDefaultType(), 2, "split.ci");
        ci->addIncoming(zero, preheader);
        llvm::PHINode* cnt = builder->CreatePHI(getDefaultType(), 2, "split.cnt");
        cnt->addIncoming(one, preheader); // at least 1 part
        llvm::Value* ccond = builder->CreateICmpULT(ci, strLen, "split.ccond");
        builder->CreateCondBr(ccond, countBodyBB, countDoneBB);

        builder->SetInsertPoint(countBodyBB);
        llvm::Value* charPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, ci, "split.cptr");
        auto* splitChLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "split.ch");
        splitChLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* ch32 = builder->CreateZExt(splitChLoad, llvm::Type::getInt32Ty(*context), "split.ch32");
        llvm::Value* isDelim = builder->CreateICmpEQ(ch32, delimChar32, "split.isdelim");
        llvm::Value* inc = builder->CreateSelect(isDelim, one, zero, "split.inc");
        llvm::Value* newCnt = builder->CreateAdd(cnt, inc, "split.newcnt", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateBr(countIncBB);

        builder->SetInsertPoint(countIncBB);
        llvm::Value* nextCi = builder->CreateAdd(ci, one, "split.nextci", /*HasNUW=*/true, /*HasNSW=*/true);
        ci->addIncoming(nextCi, countIncBB);
        cnt->addIncoming(newCnt, countIncBB);
        builder->CreateBr(countLoopBB);

        builder->SetInsertPoint(countDoneBB);
        // cnt now holds the number of parts

        // Allocate result array: (cnt + 1) * 8 bytes (length + elements)
        llvm::Value* slots = builder->CreateAdd(cnt, one, "split.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "split.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* arrBuf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "split.arr");
        builder->CreateStore(cnt, arrBuf);

        // Split pass: iterate and create substrings
        llvm::BasicBlock* splitPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* splitLoopBB = llvm::BasicBlock::Create(*context, "split.loop", function);
        llvm::BasicBlock* splitBodyBB = llvm::BasicBlock::Create(*context, "split.body", function);
        llvm::BasicBlock* splitDelimBB = llvm::BasicBlock::Create(*context, "split.delim", function);
        llvm::BasicBlock* splitContBB = llvm::BasicBlock::Create(*context, "split.cont", function);
        llvm::BasicBlock* splitDoneBB = llvm::BasicBlock::Create(*context, "split.done", function);

        builder->CreateBr(splitLoopBB);

        builder->SetInsertPoint(splitLoopBB);
        llvm::PHINode* si = builder->CreatePHI(getDefaultType(), 2, "split.si");
        si->addIncoming(zero, splitPreBB);
        llvm::PHINode* partIdx = builder->CreatePHI(getDefaultType(), 2, "split.pidx");
        partIdx->addIncoming(zero, splitPreBB);
        llvm::PHINode* partStart = builder->CreatePHI(getDefaultType(), 2, "split.pstart");
        partStart->addIncoming(zero, splitPreBB);
        llvm::Value* scond = builder->CreateICmpULE(si, strLen, "split.scond");
        builder->CreateCondBr(scond, splitBodyBB, splitDoneBB);

        builder->SetInsertPoint(splitBodyBB);
        // Check if at end of string or at delimiter
        llvm::Value* atEnd = builder->CreateICmpEQ(si, strLen, "split.atend");
        llvm::Value* bodyCharPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, si, "split.bptr");
        auto* bodyChLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), bodyCharPtr, "split.bch");
        bodyChLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* bodyCh32 = builder->CreateZExt(bodyChLoad, llvm::Type::getInt32Ty(*context), "split.bch32");
        llvm::Value* bodyIsDelim = builder->CreateICmpEQ(bodyCh32, delimChar32, "split.bisdelim");
        llvm::Value* shouldSplit = builder->CreateOr(atEnd, bodyIsDelim, "split.shouldsplit");
        builder->CreateCondBr(shouldSplit, splitDelimBB, splitContBB);

        builder->SetInsertPoint(splitDelimBB);
        // Create substring from partStart to si
        llvm::Value* partLen = builder->CreateSub(si, partStart, "split.plen", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* srcStart =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, partStart, "split.srcstart");
        llvm::Value* sub = builder->CreateCall(getOrDeclareStrndup(), {srcStart, partLen}, "split.sub");
        llvm::Value* subInt = builder->CreatePtrToInt(sub, getDefaultType(), "split.subint");
        // Store in array at (partIdx + 1) position
        llvm::Value* arrSlot = builder->CreateAdd(partIdx, one, "split.slot", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* arrSlotPtr = builder->CreateInBoundsGEP(getDefaultType(), arrBuf, arrSlot, "split.slotptr");
        builder->CreateStore(subInt, arrSlotPtr);
        llvm::Value* nextPartIdx = builder->CreateAdd(partIdx, one, "split.npidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextPartStart = builder->CreateAdd(si, one, "split.npstart", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateBr(splitContBB);

        builder->SetInsertPoint(splitContBB);
        llvm::PHINode* mergedIdx = builder->CreatePHI(getDefaultType(), 2, "split.midx");
        mergedIdx->addIncoming(partIdx, splitBodyBB);
        mergedIdx->addIncoming(nextPartIdx, splitDelimBB);
        llvm::PHINode* mergedStart = builder->CreatePHI(getDefaultType(), 2, "split.mstart");
        mergedStart->addIncoming(partStart, splitBodyBB);
        mergedStart->addIncoming(nextPartStart, splitDelimBB);
        llvm::Value* nextSi = builder->CreateAdd(si, one, "split.nextsi", /*HasNUW=*/true, /*HasNSW=*/true);
        si->addIncoming(nextSi, splitContBB);
        partIdx->addIncoming(mergedIdx, splitContBB);
        partStart->addIncoming(mergedStart, splitContBB);
        builder->CreateBr(splitLoopBB);

        builder->SetInsertPoint(splitDoneBB);
        return builder->CreatePtrToInt(arrBuf, getDefaultType(), "split.result");
    }

    // -----------------------------------------------------------------------
    // str_chars(s) — convert string into array of character codes
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_CHARS) {
        validateArgCount(expr, "str_chars", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "chars.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "chars.len");

        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);

        // Allocate array: (len + 1) * 8
        llvm::Value* slots = builder->CreateAdd(strLen, one, "chars.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "chars.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "chars.buf");
        builder->CreateStore(strLen, buf);

        // Fill loop
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "chars.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "chars.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "chars.done", function);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "chars.idx");
        idx->addIncoming(zero, preBB);
        llvm::Value* cond = builder->CreateICmpULT(idx, strLen, "chars.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* charP = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, idx, "chars.cptr");
        auto* charsChLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charP, "chars.ch");
        charsChLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* chExt = builder->CreateZExt(charsChLoad, getDefaultType(), "chars.chext");
        llvm::Value* arrSlot = builder->CreateAdd(idx, one, "chars.slot", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* arrSlotPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, arrSlot, "chars.slotptr");
        builder->CreateStore(chExt, arrSlotPtr);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "chars.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "chars.result");
    }

    // -----------------------------------------------------------------------
    // str_join(arr, delim) — join array of strings with delimiter
    // Inverse of str_split: concatenates array elements with delimiter between.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_JOIN) {
        validateArgCount(expr, "str_join", 2);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* delimArg = generateExpression(expr->arguments[1].get());

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy()
                ? arrArg
                : builder->CreateIntToPtr(arrArg, ptrTy, "join.arrptr");
        llvm::Value* delimPtr =
            delimArg->getType()->isPointerTy()
                ? delimArg
                : builder->CreateIntToPtr(delimArg, ptrTy, "join.delim");

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* i8zero = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0);

        auto* joinLenLoad = builder->CreateAlignedLoad(getDefaultType(), arrPtr, llvm::MaybeAlign(8), "join.len");
        joinLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        joinLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* arrLen = joinLenLoad;
        llvm::Value* delimLen = builder->CreateCall(getOrDeclareStrlen(), {delimPtr}, "join.delimlen");

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // --- Pass 1: compute total output length ---
        llvm::BasicBlock* lenPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* lenLoopBB = llvm::BasicBlock::Create(*context, "join.lenloop", function);
        llvm::BasicBlock* lenBodyBB = llvm::BasicBlock::Create(*context, "join.lenbody", function);
        llvm::BasicBlock* lenDoneBB = llvm::BasicBlock::Create(*context, "join.lendone", function);

        builder->CreateBr(lenLoopBB);

        builder->SetInsertPoint(lenLoopBB);
        llvm::PHINode* li = builder->CreatePHI(getDefaultType(), 2, "join.li");
        li->addIncoming(zero, lenPreBB);
        llvm::PHINode* totalLen = builder->CreatePHI(getDefaultType(), 2, "join.totlen");
        totalLen->addIncoming(zero, lenPreBB);
        llvm::Value* lcond = builder->CreateICmpULT(li, arrLen, "join.lcond");
        builder->CreateCondBr(lcond, lenBodyBB, lenDoneBB);

        builder->SetInsertPoint(lenBodyBB);
        // Load string pointer from array slot (index li + 1)
        llvm::Value* lslot = builder->CreateAdd(li, one, "join.lslot", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* lslotPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, lslot, "join.lslotptr");
        llvm::Value* elemInt = builder->CreateAlignedLoad(getDefaultType(), lslotPtr, llvm::MaybeAlign(8), "join.elemint");
        llvm::Value* elemPtr = builder->CreateIntToPtr(elemInt, ptrTy, "join.elemptr");
        llvm::Value* elemLen = builder->CreateCall(getOrDeclareStrlen(), {elemPtr}, "join.elemlen");
        llvm::Value* newTotal = builder->CreateAdd(totalLen, elemLen, "join.newtot", /*HasNUW=*/true, /*HasNSW=*/true);
        // Add delimiter length for all elements except the first
        llvm::Value* isFirst = builder->CreateICmpEQ(li, zero, "join.isfirst");
        llvm::Value* delimAdd = builder->CreateSelect(isFirst, zero, delimLen, "join.delimadd");
        llvm::Value* newTotal2 = builder->CreateAdd(newTotal, delimAdd, "join.newtot2", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextLi = builder->CreateAdd(li, one, "join.nextli", /*HasNUW=*/true, /*HasNSW=*/true);
        li->addIncoming(nextLi, lenBodyBB);
        totalLen->addIncoming(newTotal2, lenBodyBB);
        builder->CreateBr(lenLoopBB);

        // --- Allocate output buffer ---
        builder->SetInsertPoint(lenDoneBB);
        llvm::Value* bufSize = builder->CreateAdd(totalLen, one, "join.bufsize", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "join.buf");
        // Store null terminator at position 0 initially
        builder->CreateStore(i8zero, buf);

        // --- Pass 2: concatenate elements ---
        llvm::BasicBlock* catPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* catLoopBB = llvm::BasicBlock::Create(*context, "join.catloop", function);
        llvm::BasicBlock* catBodyBB = llvm::BasicBlock::Create(*context, "join.catbody", function);
        llvm::BasicBlock* catDoneBB = llvm::BasicBlock::Create(*context, "join.catdone", function);

        builder->CreateBr(catLoopBB);

        builder->SetInsertPoint(catLoopBB);
        llvm::PHINode* ci = builder->CreatePHI(getDefaultType(), 2, "join.ci");
        ci->addIncoming(zero, catPreBB);
        llvm::PHINode* writePos = builder->CreatePHI(getDefaultType(), 2, "join.wpos");
        writePos->addIncoming(zero, catPreBB);
        llvm::Value* ccond = builder->CreateICmpULT(ci, arrLen, "join.ccond");
        builder->CreateCondBr(ccond, catBodyBB, catDoneBB);

        builder->SetInsertPoint(catBodyBB);
        llvm::Value* dstPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, writePos, "join.dst");
        // Copy delimiter before element (for all but first)
        llvm::Value* ciIsFirst = builder->CreateICmpEQ(ci, zero, "join.cifirst");
        llvm::Value* delimCopyLen = builder->CreateSelect(ciIsFirst, zero, delimLen, "join.dcplen");
        // Conditionally copy delimiter
        builder->CreateCall(getOrDeclareMemcpy(), {dstPtr, delimPtr, delimCopyLen});
        llvm::Value* afterDelim = builder->CreateAdd(writePos, delimCopyLen, "join.afterdelim", /*HasNUW=*/true, /*HasNSW=*/true);
        // Load and copy element
        llvm::Value* cslot = builder->CreateAdd(ci, one, "join.cslot", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* cslotPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, cslot, "join.cslotptr");
        llvm::Value* celemInt = builder->CreateAlignedLoad(getDefaultType(), cslotPtr, llvm::MaybeAlign(8), "join.celemint");
        llvm::Value* celemPtr = builder->CreateIntToPtr(celemInt, ptrTy, "join.celemptr");
        llvm::Value* celemLen = builder->CreateCall(getOrDeclareStrlen(), {celemPtr}, "join.celemlen");
        llvm::Value* elemDst = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, afterDelim, "join.elemdst");
        builder->CreateCall(getOrDeclareMemcpy(), {elemDst, celemPtr, celemLen});
        llvm::Value* afterElem = builder->CreateAdd(afterDelim, celemLen, "join.afterelem", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextCi = builder->CreateAdd(ci, one, "join.nextci", /*HasNUW=*/true, /*HasNSW=*/true);
        ci->addIncoming(nextCi, catBodyBB);
        writePos->addIncoming(afterElem, catBodyBB);
        builder->CreateBr(catLoopBB);

        // --- Null-terminate and return ---
        builder->SetInsertPoint(catDoneBB);
        llvm::Value* endPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, writePos, "join.end");
        builder->CreateStore(i8zero, endPtr);
        stringReturningFunctions_.insert("str_join");
        return buf;
    }

    // -----------------------------------------------------------------------
    // str_count(s, sub) — count non-overlapping occurrences of substring
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_COUNT) {
        validateArgCount(expr, "str_count", 2);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* subArg = generateExpression(expr->arguments[1].get());

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, ptrTy, "scount.str");
        llvm::Value* subPtr =
            subArg->getType()->isPointerTy()
                ? subArg
                : builder->CreateIntToPtr(subArg, ptrTy, "scount.sub");

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);

        llvm::Value* subLen = builder->CreateCall(getOrDeclareStrlen(), {subPtr}, "scount.sublen");

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // If sub is empty, return 0 (avoid infinite loop)
        llvm::BasicBlock* emptySubBB = llvm::BasicBlock::Create(*context, "scount.emptysub", function);
        llvm::BasicBlock* mainBB = llvm::BasicBlock::Create(*context, "scount.main", function);
        llvm::Value* subIsEmpty = builder->CreateICmpEQ(subLen, zero, "scount.isempty");
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "scount.merge", function);
        builder->CreateCondBr(subIsEmpty, emptySubBB, mainBB);

        builder->SetInsertPoint(emptySubBB);
        builder->CreateBr(mergeBB);

        // Main counting loop using strstr
        builder->SetInsertPoint(mainBB);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "scount.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "scount.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "scount.done", function);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* cursor = builder->CreatePHI(ptrTy, 2, "scount.cursor");
        cursor->addIncoming(strPtr, mainBB);
        llvm::PHINode* count = builder->CreatePHI(getDefaultType(), 2, "scount.count");
        count->addIncoming(zero, mainBB);

        llvm::Value* found = builder->CreateCall(getOrDeclareStrstr(), {cursor, subPtr}, "scount.found");
        llvm::Value* isNull = builder->CreateICmpEQ(found, nullPtr, "scount.isnull");
        builder->CreateCondBr(isNull, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* newCount = builder->CreateAdd(count, one, "scount.newcount", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextCursor = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), found, subLen, "scount.next");
        cursor->addIncoming(nextCursor, bodyBB);
        count->addIncoming(newCount, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "scount.result");
        result->addIncoming(zero, emptySubBB);
        result->addIncoming(count, doneBB);
        return result;
    }

    // -----------------------------------------------------------------------
    // File I/O built-ins
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::FILE_READ) {
        validateArgCount(expr, "file_read", 1);
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        llvm::Value* pathPtr =
            pathArg->getType()->isPointerTy()
                ? pathArg
                : builder->CreateIntToPtr(pathArg, llvm::PointerType::getUnqual(*context), "fread.path");

        // mode = "rb"
        llvm::GlobalVariable* mode = module->getGlobalVariable("__fread_mode", true);
        if (!mode)
            mode = builder->CreateGlobalString("rb", "__fread_mode");

        // FILE* fp = fopen(path, "rb")
        llvm::Value* fp = builder->CreateCall(getOrDeclareFopen(), {pathPtr, mode}, "fread.fp");

        // Check for null
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* isNull = builder->CreateICmpEQ(fp, nullPtr, "fread.isnull");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(*context, "fread.null", parentFn);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "fread.ok", parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "fread.merge", parentFn);
        // File open normally succeeds — mark null path cold.
        auto* freadW = llvm::MDBuilder(*context).createBranchWeights(1, 100);
        builder->CreateCondBr(isNull, nullBB, okBB, freadW);

        // Null path: return empty string
        builder->SetInsertPoint(nullBB);
        llvm::Value* emptyBuf = builder->CreateCall(getOrDeclareMalloc(),
            {llvm::ConstantInt::get(getDefaultType(), 1)}, "fread.empty");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), emptyBuf);
        llvm::Value* emptyResult = emptyBuf;
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* nullEndBB = builder->GetInsertBlock();

        // OK path: seek to end, get size, read
        builder->SetInsertPoint(okBB);
        // fseek(fp, 0, SEEK_END=2)
        builder->CreateCall(getOrDeclareFseek(),
            {fp, llvm::ConstantInt::get(getDefaultType(), 0),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 2)});
        // size = ftell(fp)
        llvm::Value* fileSize = builder->CreateCall(getOrDeclareFtell(), {fp}, "fread.size");
        // fseek(fp, 0, SEEK_SET=0)
        builder->CreateCall(getOrDeclareFseek(),
            {fp, llvm::ConstantInt::get(getDefaultType(), 0),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0)});
        // buf = malloc(size + 1)
        llvm::Value* bufSize = builder->CreateAdd(fileSize,
            llvm::ConstantInt::get(getDefaultType(), 1), "fread.bufsize");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "fread.buf");
        // fread(buf, 1, size, fp)
        builder->CreateCall(getOrDeclareFread(),
            {buf, llvm::ConstantInt::get(getDefaultType(), 1), fileSize, fp});
        // null terminate
        llvm::Value* nullTermPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, fileSize, "fread.nullterm");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nullTermPtr);
        // fclose(fp)
        builder->CreateCall(getOrDeclareFclose(), {fp});
        llvm::Value* okResult = buf;
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* okEndBB = builder->GetInsertBlock();

        // Merge
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(ptrTy, 2, "fread.result");
        phi->addIncoming(emptyResult, nullEndBB);
        phi->addIncoming(okResult, okEndBB);
        stringReturningFunctions_.insert("file_read");
        return phi;
    }

    if (bid == BuiltinId::FILE_WRITE) {
        validateArgCount(expr, "file_write", 2);
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        llvm::Value* contentArg = generateExpression(expr->arguments[1].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* pathPtr = pathArg->getType()->isPointerTy()
            ? pathArg : builder->CreateIntToPtr(pathArg, ptrTy, "fwrite.path");
        llvm::Value* contentPtr = contentArg->getType()->isPointerTy()
            ? contentArg : builder->CreateIntToPtr(contentArg, ptrTy, "fwrite.content");

        llvm::GlobalVariable* mode = module->getGlobalVariable("__fwrite_mode", true);
        if (!mode)
            mode = builder->CreateGlobalString("wb", "__fwrite_mode");

        llvm::Value* fp = builder->CreateCall(getOrDeclareFopen(), {pathPtr, mode}, "fwrite.fp");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* isNull = builder->CreateICmpEQ(fp, nullPtr, "fwrite.isnull");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(*context, "fwrite.null", parentFn);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "fwrite.ok", parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "fwrite.merge", parentFn);
        // File open normally succeeds — mark null path cold.
        auto* fwriteW = llvm::MDBuilder(*context).createBranchWeights(1, 100);
        builder->CreateCondBr(isNull, nullBB, okBB, fwriteW);

        builder->SetInsertPoint(nullBB);
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(okBB);
        llvm::Value* slen = builder->CreateCall(getOrDeclareStrlen(), {contentPtr}, "fwrite.len");
        builder->CreateCall(getOrDeclareFwrite(),
            {contentPtr, llvm::ConstantInt::get(getDefaultType(), 1), slen, fp});
        builder->CreateCall(getOrDeclareFclose(), {fp});
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "fwrite.result");
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), nullBB);
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), okBB);
        return phi;
    }

    if (bid == BuiltinId::FILE_APPEND) {
        validateArgCount(expr, "file_append", 2);
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        llvm::Value* contentArg = generateExpression(expr->arguments[1].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* pathPtr = pathArg->getType()->isPointerTy()
            ? pathArg : builder->CreateIntToPtr(pathArg, ptrTy, "fappend.path");
        llvm::Value* contentPtr = contentArg->getType()->isPointerTy()
            ? contentArg : builder->CreateIntToPtr(contentArg, ptrTy, "fappend.content");

        llvm::GlobalVariable* mode = module->getGlobalVariable("__fappend_mode", true);
        if (!mode)
            mode = builder->CreateGlobalString("a", "__fappend_mode");

        llvm::Value* fp = builder->CreateCall(getOrDeclareFopen(), {pathPtr, mode}, "fappend.fp");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* isNull = builder->CreateICmpEQ(fp, nullPtr, "fappend.isnull");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(*context, "fappend.null", parentFn);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "fappend.ok", parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "fappend.merge", parentFn);
        // File open normally succeeds — mark null path cold.
        auto* fappW = llvm::MDBuilder(*context).createBranchWeights(1, 100);
        builder->CreateCondBr(isNull, nullBB, okBB, fappW);

        builder->SetInsertPoint(nullBB);
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(okBB);
        llvm::Value* slen = builder->CreateCall(getOrDeclareStrlen(), {contentPtr}, "fappend.len");
        builder->CreateCall(getOrDeclareFwrite(),
            {contentPtr, llvm::ConstantInt::get(getDefaultType(), 1), slen, fp});
        builder->CreateCall(getOrDeclareFclose(), {fp});
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "fappend.result");
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), nullBB);
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), okBB);
        return phi;
    }

    if (bid == BuiltinId::FILE_EXISTS) {
        validateArgCount(expr, "file_exists", 1);
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* pathPtr = pathArg->getType()->isPointerTy()
            ? pathArg : builder->CreateIntToPtr(pathArg, ptrTy, "fexists.path");
        // access(path, F_OK=0) returns 0 on success, -1 on failure
        llvm::Value* result = builder->CreateCall(getOrDeclareAccess(),
            {pathPtr, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0)}, "fexists.access");
        llvm::Value* isZero = builder->CreateICmpEQ(result,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "fexists.cmp");
        return builder->CreateZExt(isZero, getDefaultType(), "fexists.result");
    }

    // -----------------------------------------------------------------------
    // Map/Dictionary built-ins
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::MAP_NEW) {
        validateArgCount(expr, "map_new", 0);
        llvm::Value* buf = builder->CreateCall(getOrEmitHashMapNew(), {}, "mapnew.buf");
        return builder->CreatePtrToInt(buf, getDefaultType(), "mapnew.result");
    }

    if (bid == BuiltinId::MAP_SET) {
        validateArgCount(expr, "map_set", 3);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        llvm::Value* valArg = generateExpression(expr->arguments[2].get());
        keyArg = toDefaultType(keyArg);
        valArg = toDefaultType(valArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
            ? mapArg : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "mapset.ptr");
        llvm::Value* result = builder->CreateCall(getOrEmitHashMapSet(), {mapPtr, keyArg, valArg}, "mapset.result");
        return builder->CreatePtrToInt(result, getDefaultType(), "mapset.i64");
    }

    if (bid == BuiltinId::MAP_GET) {
        validateArgCount(expr, "map_get", 3);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        llvm::Value* defArg = generateExpression(expr->arguments[2].get());
        keyArg = toDefaultType(keyArg);
        defArg = toDefaultType(defArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
            ? mapArg : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "mapget.ptr");
        return builder->CreateCall(getOrEmitHashMapGet(), {mapPtr, keyArg, defArg}, "mapget.result");
    }

    if (bid == BuiltinId::MAP_HAS) {
        validateArgCount(expr, "map_has", 2);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        keyArg = toDefaultType(keyArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
            ? mapArg : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "maphas.ptr");
        return builder->CreateCall(getOrEmitHashMapHas(), {mapPtr, keyArg}, "maphas.result");
    }

    if (bid == BuiltinId::MAP_REMOVE) {
        validateArgCount(expr, "map_remove", 2);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        keyArg = toDefaultType(keyArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
            ? mapArg : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "maprem.ptr");
        llvm::Value* result = builder->CreateCall(getOrEmitHashMapRemove(), {mapPtr, keyArg}, "maprem.result");
        return builder->CreatePtrToInt(result, getDefaultType(), "maprem.i64");
    }

    if (bid == BuiltinId::MAP_KEYS) {
        validateArgCount(expr, "map_keys", 1);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
            ? mapArg : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "mapkeys.ptr");
        llvm::Value* buf = builder->CreateCall(getOrEmitHashMapKeys(), {mapPtr}, "mapkeys.buf");
        return builder->CreatePtrToInt(buf, getDefaultType(), "mapkeys.result");
    }

    if (bid == BuiltinId::MAP_VALUES) {
        validateArgCount(expr, "map_values", 1);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
            ? mapArg : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "mapvals.ptr");
        llvm::Value* buf = builder->CreateCall(getOrEmitHashMapValues(), {mapPtr}, "mapvals.buf");
        return builder->CreatePtrToInt(buf, getDefaultType(), "mapvals.result");
    }

    if (bid == BuiltinId::MAP_SIZE) {
        validateArgCount(expr, "map_size", 1);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
            ? mapArg : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "mapsize.ptr");
        return builder->CreateCall(getOrEmitHashMapSize(), {mapPtr}, "mapsize.result");
    }

    // -----------------------------------------------------------------------
    // Range and utility built-ins
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::RANGE) {
        validateArgCount(expr, "range", 2);
        llvm::Value* startArg = generateExpression(expr->arguments[0].get());
        llvm::Value* endArg = generateExpression(expr->arguments[1].get());
        startArg = toDefaultType(startArg);
        endArg = toDefaultType(endArg);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // count = max(end - start, 0)
        llvm::Value* diff = builder->CreateSub(endArg, startArg, "range.diff");
        llvm::Value* isPos = builder->CreateICmpSGT(diff, zero, "range.ispos");
        llvm::Value* count = builder->CreateSelect(isPos, diff, zero, "range.count");

        // Allocate: (count + 1) * 8
        llvm::Value* arrSlots = builder->CreateAdd(count, one, "range.arrslots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* arrSize = builder->CreateMul(arrSlots, eight, "range.arrsize", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrSize}, "range.buf");
        builder->CreateStore(count, buf);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "range.loop", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "range.body", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "range.done", parentFn);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* i = builder->CreatePHI(getDefaultType(), 2, "range.i");
        i->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpULT(i, count, "range.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* val = builder->CreateAdd(startArg, i, "range.val");
        llvm::Value* slot = builder->CreateAdd(i, one, "range.slot", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, slot, "range.elemptr");
        builder->CreateStore(val, elemPtr);
        llvm::Value* nextI = builder->CreateAdd(i, one, "range.next", /*HasNUW=*/true, /*HasNSW=*/true);
        i->addIncoming(nextI, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "range.result");
    }

    if (bid == BuiltinId::RANGE_STEP) {
        validateArgCount(expr, "range_step", 3);
        llvm::Value* startArg = generateExpression(expr->arguments[0].get());
        llvm::Value* endArg = generateExpression(expr->arguments[1].get());
        llvm::Value* stepArg = generateExpression(expr->arguments[2].get());
        startArg = toDefaultType(startArg);
        endArg = toDefaultType(endArg);
        stepArg = toDefaultType(stepArg);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Guard: step must not be 0 (division by zero).
        llvm::Value* stepIsZero = builder->CreateICmpEQ(stepArg, zero, "rstep.stepzero");
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* stepOkBB = llvm::BasicBlock::Create(*context, "rstep.stepok", parentFn);
        llvm::BasicBlock* stepFailBB = llvm::BasicBlock::Create(*context, "rstep.stepfail", parentFn);
        builder->CreateCondBr(stepIsZero, stepFailBB, stepOkBB);

        builder->SetInsertPoint(stepFailBB);
        llvm::Value* errMsg = builder->CreateGlobalString(
            "Runtime error: range step cannot be zero\n", "rstep_zero_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(stepOkBB);

        // count = max((end - start + step - 1) / step, 0) for positive step
        // Simplified: count = max(0, (end - start + step - sign) / step)
        // Use a safe approach: (end - start) / step, clamped to 0
        llvm::Value* diff = builder->CreateSub(endArg, startArg, "rstep.diff");
        // For positive step: count = (diff + step - 1) / step if diff > 0
        llvm::Value* adjDiff = builder->CreateAdd(diff,
            builder->CreateSub(stepArg, one, "rstep.stepm1"), "rstep.adjdiff");
        llvm::Value* count = builder->CreateSDiv(adjDiff, stepArg, "rstep.count");
        llvm::Value* isPos = builder->CreateICmpSGT(count, zero, "rstep.ispos");
        count = builder->CreateSelect(isPos, count, zero, "rstep.clampcount");

        llvm::Value* arrSlots = builder->CreateAdd(count, one, "rstep.arrslots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* arrSize = builder->CreateMul(arrSlots, eight, "rstep.arrsize", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrSize}, "rstep.buf");
        builder->CreateStore(count, buf);

        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "rstep.loop", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "rstep.body", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "rstep.done", parentFn);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* i = builder->CreatePHI(getDefaultType(), 2, "rstep.i");
        i->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpULT(i, count, "rstep.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* val = builder->CreateAdd(startArg,
            builder->CreateMul(i, stepArg, "rstep.offset"), "rstep.val");
        llvm::Value* slot = builder->CreateAdd(i, one, "rstep.slot");
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, slot, "rstep.elemptr");
        builder->CreateStore(val, elemPtr);
        llvm::Value* nextI = builder->CreateAdd(i, one, "rstep.next", /*HasNUW=*/true, /*HasNSW=*/true);
        i->addIncoming(nextI, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "rstep.result");
    }

    if (bid == BuiltinId::CHAR_CODE) {
        validateArgCount(expr, "char_code", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
            ? strArg : builder->CreateIntToPtr(strArg, ptrTy, "charcode.ptr");
        llvm::Value* ch = builder->CreateLoad(llvm::Type::getInt8Ty(*context), strPtr, "charcode.ch");
        return builder->CreateZExt(ch, getDefaultType(), "charcode.result");
    }

    if (bid == BuiltinId::NUMBER_TO_STRING) {
        validateArgCount(expr, "number_to_string", 1);
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        bool isFloat = val->getType()->isDoubleTy();
        if (!isFloat)
            val = toDefaultType(val);
        if (isFloat) {
            llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 32);
            llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "numtostr.buf");
            llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_float_fmt", true);
            if (!fmtStr)
                fmtStr = builder->CreateGlobalString("%g", "tostr_float_fmt");
            builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
            stringReturningFunctions_.insert("number_to_string");
            return buf;
        }
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 21);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "numtostr.buf");
        llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_fmt", true);
        if (!fmtStr)
            fmtStr = builder->CreateGlobalString("%lld", "tostr_fmt");
        builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
        stringReturningFunctions_.insert("number_to_string");
        return buf;
    }

    if (bid == BuiltinId::STRING_TO_NUMBER) {
        validateArgCount(expr, "string_to_number", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
            ? strArg : builder->CreateIntToPtr(strArg, ptrTy, "strtonum.ptr");
        // Use strtoll to parse as integer first
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* result = builder->CreateCall(getOrDeclareStrtoll(),
            {strPtr, nullPtr, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10)}, "strtonum.result");
        return result;
    }

    // -----------------------------------------------------------------------
    // Concurrency primitives (pthreads)
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::THREAD_CREATE) {
        validateArgCount(expr, "thread_create", 1);
        // thread_create("func_name") — look up the function by name and call pthread_create
        auto* ptrTy = llvm::PointerType::getUnqual(*context);

        // The argument should be a string containing a function name.
        // We look up the function directly by checking if the argument is a literal.
        auto* litArg = dynamic_cast<LiteralExpr*>(expr->arguments[0].get());
        if (!litArg || litArg->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("thread_create requires a string literal function name", expr);
        }
        auto fnIt = functions.find(litArg->stringValue);
        if (fnIt == functions.end() || !fnIt->second) {
            codegenError("thread_create: unknown function '" + litArg->stringValue + "'", expr);
        }
        llvm::Function* targetFunc = fnIt->second;

        // Generate a wrapper function: void* __thread_wrapper_<name>(void* arg) { target(); return NULL; }
        std::string wrapperName = "__thread_wrapper_" + litArg->stringValue;
        llvm::Function* wrapper = module->getFunction(wrapperName);
        if (!wrapper) {
            auto* wrapperType = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
            wrapper = llvm::Function::Create(wrapperType, llvm::Function::InternalLinkage,
                                             wrapperName, module.get());
            auto* savedBB = builder->GetInsertBlock();
            auto* savedPoint = builder->GetInsertPoint() != builder->GetInsertBlock()->end()
                                    ? &*builder->GetInsertPoint() : nullptr;
            auto* entry = llvm::BasicBlock::Create(*context, "entry", wrapper);
            builder->SetInsertPoint(entry);
            builder->CreateCall(targetFunc);
            builder->CreateRet(llvm::ConstantPointerNull::get(ptrTy));
            if (savedPoint) {
                builder->SetInsertPoint(savedBB, llvm::BasicBlock::iterator(savedPoint));
            } else {
                builder->SetInsertPoint(savedBB);
            }
        }

        // Allocate pthread_t on the stack (8 bytes = i64)
        auto* parentFunc = builder->GetInsertBlock()->getParent();
        auto* tidAlloca = createEntryBlockAlloca(parentFunc, "tid");
        llvm::Value* nullAttr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* nullArg = llvm::ConstantPointerNull::get(ptrTy);

        builder->CreateCall(getOrDeclarePthreadCreate(),
                            {tidAlloca, nullAttr, wrapper, nullArg});

        // Return the thread id
        return builder->CreateAlignedLoad(getDefaultType(), tidAlloca, llvm::MaybeAlign(8), "tid.val");
    }

    if (bid == BuiltinId::THREAD_JOIN) {
        validateArgCount(expr, "thread_join", 1);
        llvm::Value* tid = generateExpression(expr->arguments[0].get());
        tid = toDefaultType(tid);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nullRetval = llvm::ConstantPointerNull::get(ptrTy);
        builder->CreateCall(getOrDeclarePthreadJoin(), {tid, nullRetval});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::MUTEX_NEW) {
        validateArgCount(expr, "mutex_new", 0);
        // Allocate space for pthread_mutex_t.  The struct size varies by
        // platform (40 bytes on Linux x86_64, 64 on macOS).  We allocate a
        // generous fixed size that covers all common platforms.
        static constexpr int64_t kMutexAllocSize = 64;
        llvm::Value* size = llvm::ConstantInt::get(getDefaultType(), kMutexAllocSize);
        llvm::Value* mutex = builder->CreateCall(getOrDeclareMalloc(), {size}, "mutex.ptr");
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nullAttr = llvm::ConstantPointerNull::get(ptrTy);
        builder->CreateCall(getOrDeclarePthreadMutexInit(), {mutex, nullAttr});
        // Return as i64
        return builder->CreatePtrToInt(mutex, getDefaultType(), "mutex.val");
    }

    if (bid == BuiltinId::MUTEX_LOCK) {
        validateArgCount(expr, "mutex_lock", 1);
        llvm::Value* mutexVal = generateExpression(expr->arguments[0].get());
        mutexVal = toDefaultType(mutexVal);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mutexPtr = builder->CreateIntToPtr(mutexVal, ptrTy, "mutex.ptr");
        builder->CreateCall(getOrDeclarePthreadMutexLock(), {mutexPtr});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::MUTEX_UNLOCK) {
        validateArgCount(expr, "mutex_unlock", 1);
        llvm::Value* mutexVal = generateExpression(expr->arguments[0].get());
        mutexVal = toDefaultType(mutexVal);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mutexPtr = builder->CreateIntToPtr(mutexVal, ptrTy, "mutex.ptr");
        builder->CreateCall(getOrDeclarePthreadMutexUnlock(), {mutexPtr});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::MUTEX_DESTROY) {
        validateArgCount(expr, "mutex_destroy", 1);
        llvm::Value* mutexVal = generateExpression(expr->arguments[0].get());
        mutexVal = toDefaultType(mutexVal);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mutexPtr = builder->CreateIntToPtr(mutexVal, ptrTy, "mutex.ptr");
        builder->CreateCall(getOrDeclarePthreadMutexDestroy(), {mutexPtr});
        builder->CreateCall(getOrDeclareFree(), {mutexPtr});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // ── Bitwise intrinsic builtins ─────────────────────────────────────
    if (bid == BuiltinId::POPCOUNT) {
        validateArgCount(expr, "popcount", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* ctpopFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctpop, {getDefaultType()});
        auto* result = builder->CreateCall(ctpopFn, {arg}, "popcount.result");
        nonNegValues_.insert(result);  // popcount is always in [0, 64]
        return result;
    }

    if (bid == BuiltinId::CLZ) {
        validateArgCount(expr, "clz", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* ctlzFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctlz, {getDefaultType()});
        auto* result = builder->CreateCall(ctlzFn, {arg, builder->getFalse()}, "clz.result");
        nonNegValues_.insert(result);  // clz is always in [0, 64]
        return result;
    }

    if (bid == BuiltinId::CTZ) {
        validateArgCount(expr, "ctz", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* cttzFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::cttz, {getDefaultType()});
        auto* result = builder->CreateCall(cttzFn, {arg, builder->getFalse()}, "ctz.result");
        nonNegValues_.insert(result);  // ctz is always in [0, 64]
        return result;
    }

    if (bid == BuiltinId::BITREVERSE) {
        validateArgCount(expr, "bitreverse", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* brevFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::bitreverse, {getDefaultType()});
        return builder->CreateCall(brevFn, {arg}, "bitreverse.result");
    }

    if (bid == BuiltinId::EXP2) {
        validateArgCount(expr, "exp2", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::Function* exp2Fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::exp2, {getFloatType()});
        return builder->CreateCall(exp2Fn, {fval}, "exp2.result");
    }

    if (bid == BuiltinId::IS_POWER_OF_2) {
        validateArgCount(expr, "is_power_of_2", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        // x > 0
        llvm::Value* isPos = builder->CreateICmpSGT(arg, zero, "ispow2.pos");
        // x & (x - 1) == 0
        llvm::Value* xm1 = builder->CreateSub(arg, one, "ispow2.xm1");
        llvm::Value* andVal = builder->CreateAnd(arg, xm1, "ispow2.and");
        llvm::Value* isAnd0 = builder->CreateICmpEQ(andVal, zero, "ispow2.and0");
        // x > 0 && (x & (x-1)) == 0
        llvm::Value* result = builder->CreateAnd(isPos, isAnd0, "ispow2.result");
        auto* ext = builder->CreateZExt(result, getDefaultType(), "ispow2.ext");
        nonNegValues_.insert(ext);  // is_power_of_2 returns 0 or 1 — always non-negative
        return ext;
    }

    if (bid == BuiltinId::LCM) {
        validateArgCount(expr, "lcm", 2);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        // Use absolute values
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* aNeg = builder->CreateICmpSLT(a, zero, "lcm.aneg");
        llvm::Value* aNegVal = builder->CreateNeg(a, "lcm.anegval");
        llvm::Value* aAbs = builder->CreateSelect(aNeg, aNegVal, a, "lcm.aabs");
        llvm::Value* bNeg = builder->CreateICmpSLT(b, zero, "lcm.bneg");
        llvm::Value* bNegVal = builder->CreateNeg(b, "lcm.bnegval");
        llvm::Value* bAbs = builder->CreateSelect(bNeg, bNegVal, b, "lcm.babs");

        // GCD via Euclidean algorithm: while (b != 0) { temp = b; b = a % b; a = temp; }
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheaderBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "lcm.gcd.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "lcm.gcd.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "lcm.gcd.done", function);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* phiA = builder->CreatePHI(getDefaultType(), 2, "lcm.gcd.a");
        phiA->addIncoming(aAbs, preheaderBB);
        llvm::PHINode* phiB = builder->CreatePHI(getDefaultType(), 2, "lcm.gcd.b");
        phiB->addIncoming(bAbs, preheaderBB);

        llvm::Value* bIsZero = builder->CreateICmpEQ(phiB, zero, "lcm.gcd.bzero");
        builder->CreateCondBr(bIsZero, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* remainder = builder->CreateURem(phiA, phiB, "lcm.gcd.rem");
        phiA->addIncoming(phiB, bodyBB);
        phiB->addIncoming(remainder, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // lcm(a, b) = |a| / gcd(a, b) * |b|  (divide first to avoid overflow)
        builder->SetInsertPoint(doneBB);
        llvm::Value* gcdVal = phiA;  // phiA holds gcd result
        // Handle gcd == 0 (when both inputs are 0): lcm(0, 0) = 0
        llvm::Value* gcdIsZero = builder->CreateICmpEQ(gcdVal, zero, "lcm.gcd.iszero");
        llvm::Value* divResult = builder->CreateUDiv(aAbs, gcdVal, "lcm.div");
        llvm::Value* lcmResult = builder->CreateMul(divResult, bAbs, "lcm.mul");
        auto* lcmFinal = builder->CreateSelect(gcdIsZero, zero, lcmResult, "lcm.result");
        nonNegValues_.insert(lcmFinal);  // lcm is always non-negative (uses abs of inputs)
        return lcmFinal;
    }

    // ── New intrinsic builtins ─────────────────────────────────────────
    if (bid == BuiltinId::ROTATE_LEFT) {
        validateArgCount(expr, "rotate_left", 2);
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        llvm::Value* amt = generateExpression(expr->arguments[1].get());
        val = toDefaultType(val);
        amt = toDefaultType(amt);
        // fshl(a, a, amt) implements rotate-left: (a << amt) | (a >> (64-amt))
        llvm::Function* fshlFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::fshl, {getDefaultType()});
        return builder->CreateCall(fshlFn, {val, val, amt}, "rotl.result");
    }

    if (bid == BuiltinId::ROTATE_RIGHT) {
        validateArgCount(expr, "rotate_right", 2);
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        llvm::Value* amt = generateExpression(expr->arguments[1].get());
        val = toDefaultType(val);
        amt = toDefaultType(amt);
        // fshr(a, a, amt) implements rotate-right: (a >> amt) | (a << (64-amt))
        llvm::Function* fshrFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::fshr, {getDefaultType()});
        return builder->CreateCall(fshrFn, {val, val, amt}, "rotr.result");
    }

    if (bid == BuiltinId::BSWAP) {
        validateArgCount(expr, "bswap", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* bswapFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::bswap, {getDefaultType()});
        return builder->CreateCall(bswapFn, {arg}, "bswap.result");
    }

    if (bid == BuiltinId::SATURATING_ADD) {
        validateArgCount(expr, "saturating_add", 2);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        llvm::Function* saddSatFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sadd_sat, {getDefaultType()});
        return builder->CreateCall(saddSatFn, {a, b}, "sadd.sat.result");
    }

    if (bid == BuiltinId::SATURATING_SUB) {
        validateArgCount(expr, "saturating_sub", 2);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        llvm::Function* ssubSatFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ssub_sat, {getDefaultType()});
        return builder->CreateCall(ssubSatFn, {a, b}, "ssub.sat.result");
    }

    if (bid == BuiltinId::FMA_BUILTIN) {
        validateArgCount(expr, "fma", 3);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        llvm::Value* c = generateExpression(expr->arguments[2].get());
        a = ensureFloat(a);
        b = ensureFloat(b);
        c = ensureFloat(c);
        llvm::Function* fmaFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::fma, {getFloatType()});
        return builder->CreateCall(fmaFn, {a, b, c}, "fma.result");
    }

    if (bid == BuiltinId::COPYSIGN) {
        validateArgCount(expr, "copysign", 2);
        llvm::Value* mag = generateExpression(expr->arguments[0].get());
        llvm::Value* sgn = generateExpression(expr->arguments[1].get());
        mag = ensureFloat(mag);
        sgn = ensureFloat(sgn);
        llvm::Function* copysignFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::copysign, {getFloatType()});
        return builder->CreateCall(copysignFn, {mag, sgn}, "copysign.result");
    }

    if (bid == BuiltinId::MIN_FLOAT) {
        validateArgCount(expr, "min_float", 2);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = ensureFloat(a);
        b = ensureFloat(b);
        llvm::Function* minnumFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::minnum, {getFloatType()});
        return builder->CreateCall(minnumFn, {a, b}, "minnum.result");
    }

    if (bid == BuiltinId::MAX_FLOAT) {
        validateArgCount(expr, "max_float", 2);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = ensureFloat(a);
        b = ensureFloat(b);
        llvm::Function* maxnumFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::maxnum, {getFloatType()});
        return builder->CreateCall(maxnumFn, {a, b}, "maxnum.result");
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
        // Build "did you mean?" suggestion from known functions.
        std::string msg = "Unknown function: " + expr->callee;
        std::vector<std::string> candidates;
        candidates.reserve(functions.size());
        for (const auto& kv : functions) {
            if (kv.second)
                candidates.push_back(kv.getKey().str());
        }
        std::string suggestion = suggestSimilar(expr->callee, candidates);
        if (!suggestion.empty()) {
            msg += " (did you mean '" + suggestion + "'?)";
        }
        codegenError(msg, expr);
    }
    llvm::Function* callee = calleeIt->second;

    auto declIt = functionDecls_.find(expr->callee);
    size_t requiredArgs = callee->arg_size();
    if (declIt != functionDecls_.end()) {
        requiredArgs = declIt->second->requiredParameters();
    }
    if (expr->arguments.size() < requiredArgs || expr->arguments.size() > callee->arg_size()) {
        codegenError("Function '" + expr->callee + "' expects " +
                         (requiredArgs < callee->arg_size()
                              ? std::to_string(requiredArgs) + " to " + std::to_string(callee->arg_size())
                              : std::to_string(callee->arg_size())) +
                         " argument(s), but " + std::to_string(expr->arguments.size()) + " provided",
                     expr);
    }

    std::vector<llvm::Value*> args;
    args.reserve(callee->arg_size());
    for (size_t i = 0; i < callee->arg_size(); ++i) {
        llvm::Type* expectedTy = callee->getFunctionType()->getParamType(i);
        if (i < expr->arguments.size()) {
            llvm::Value* argVal = generateExpression(expr->arguments[i].get());
            // Convert argument to the callee's declared parameter type.
            argVal = convertTo(argVal, expectedTy);
            args.push_back(argVal);
        } else if (declIt != functionDecls_.end()) {
            auto& param = declIt->second->parameters[i];
            if (param.defaultValue) {
                llvm::Value* argVal = generateExpression(param.defaultValue.get());
                argVal = convertTo(argVal, expectedTy);
                args.push_back(argVal);
            }
        }
    }

    auto* callResult = builder->CreateCall(callee, args, "calltmp");
    // When the callee is marked nounwind (all user-defined functions are at
    // O2+), propagate that to the call site so LLVM can eliminate invoke/
    // landingpad overhead at the call boundary and enable better inlining.
    if (callee->doesNotThrow()) {
        callResult->setDoesNotThrow();
    }
    // When the callee is @pure (speculatable + willreturn + readonly), mark the
    // call site as well.  This enables GVN to deduplicate identical pure calls
    // and LICM to hoist pure calls out of loops.
    if (callee->hasFnAttribute(llvm::Attribute::Speculatable)) {
        callResult->addFnAttr(llvm::Attribute::Speculatable);
    }
    // Propagate non-negativity from the callee: if the callee's return value
    // has !range [0, INT64_MAX) metadata, mark the result as non-negative.
    if (callee->hasRetAttribute(llvm::Attribute::ZExt))
        nonNegValues_.insert(callResult);
    return callResult;
}

} // namespace omscript
