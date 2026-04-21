#include "codegen.h"
#include "diagnostic.h"
#include "hardware_graph.h"
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_set>

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
    FMA_BUILTIN, COPYSIGN, MIN_FLOAT, MAX_FLOAT,
    // Optimizer hint builtins
    ASSUME, UNREACHABLE, EXPECT,
    // Array utility builtins
    ARRAY_PRODUCT, ARRAY_LAST, ARRAY_INSERT,
    // String padding builtins
    STR_PAD_LEFT, STR_PAD_RIGHT,
    // Character classification predicates
    IS_UPPER, IS_LOWER, IS_SPACE, IS_ALNUM,
    // Generic filter (dispatches based on argument type)
    FILTER,
    // String character filter
    STR_FILTER,
    // Map entry filter
    MAP_FILTER,
    // Shell command execution
    COMMAND,
    // String: strip leading / trailing whitespace independently
    STR_LSTRIP, STR_RSTRIP,
    // String: remove all occurrences of a substring
    STR_REMOVE,
    // Array: take first n / drop first n elements
    ARRAY_TAKE, ARRAY_DROP,
    // Array: deduplicate consecutive equal elements
    ARRAY_UNIQUE,
    // Array: rotate left by n positions
    ARRAY_ROTATE,
    // Array: arithmetic mean (integer division)
    ARRAY_MEAN,
    // Map: merge two maps (b wins on conflict), invert keys↔values
    MAP_MERGE, MAP_INVERT,
    // Shell command execution with sudo (password provided as second arg)
    SUDO_COMMAND,
    // Environment variable access
    ENV_GET, ENV_SET,
    // String formatting: str_format(fmt, val1[, val2[, ...]]) via snprintf
    STR_FORMAT,
    // Array: interleave two arrays into [a[0],b[0], a[1],b[1], ...].
    // Result length = 2 * min(len(a), len(b)).
    ARRAY_ZIP,
    // Arbitrary-precision integer (bigint) builtins
    BIGINT_NEW, BIGINT_ADD, BIGINT_SUB, BIGINT_MUL, BIGINT_DIV, BIGINT_MOD,
    BIGINT_NEG, BIGINT_ABS, BIGINT_POW, BIGINT_GCD,
    BIGINT_EQ, BIGINT_LT, BIGINT_LE, BIGINT_GT, BIGINT_GE, BIGINT_CMP,
    BIGINT_TOSTRING, BIGINT_TO_I64, BIGINT_BIT_LENGTH,
    BIGINT_IS_ZERO, BIGINT_IS_NEGATIVE, BIGINT_SHL, BIGINT_SHR,
    // Type-specific fast arithmetic: upper half of 128-bit multiply,
    // overflow-safe absolute difference, reciprocal sqrt with fast-math.
    INT_MULHI,        ///< Signed   mulhi(a,b) → high 64 bits of i128 product
    UINT_MULHI,       ///< Unsigned mulhi_u(a,b) → high 64 bits of u128 product
    INT_ABSDIFF,      ///< absdiff(a,b) → |a-b| without overflow (widens to i128)
    FAST_SQRT,        ///< fast_sqrt(x) → sqrt with reassociate/nnan fast-math flags
    FLOAT_IS_NAN,     ///< is_nan(x)  → 1 if x is NaN when reinterpreted as f64
    FLOAT_IS_INF,     ///< is_inf(x)  → 1 if x is ±Infinity as f64
    // ── 2D column-major matrix builtins ─────────────────────────────────────
    // Layout: [rows, cols, A[0][0], A[1][0], ..., A[rows-1][0], A[0][1], ...]
    // Element (i,j) is at slot index: j * rows + i + 2.
    // This is Fortran-style column-major storage.  The inner dimension (i,
    // the row index) is contiguous in memory, so loops over all rows of a
    // given column auto-vectorize without gather/scatter.
    MAT_NEW,      ///< mat_new(rows,cols)       → zero-filled column-major matrix
    MAT_FILL,     ///< mat_fill(rows,cols,val)  → column-major matrix filled with val
    MAT_GET,      ///< mat_get(m,i,j)           → element (i,j) of matrix m
    MAT_SET,      ///< mat_set(m,i,j,val)       → set element (i,j); returns m
    MAT_ROWS,     ///< mat_rows(m)              → number of rows
    MAT_COLS,     ///< mat_cols(m)              → number of columns
    MAT_MUL,      ///< mat_mul(a,b)             → C = A × B (column-major result)
    MAT_TRANSP    ///< mat_transp(m)            → transpose of m (new allocation)
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
    {"is_upper", BuiltinId::IS_UPPER},
    {"is_lower", BuiltinId::IS_LOWER},
    {"is_space", BuiltinId::IS_SPACE},
    {"is_alnum", BuiltinId::IS_ALNUM},
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
    {"assume", BuiltinId::ASSUME},
    {"unreachable", BuiltinId::UNREACHABLE},
    {"expect", BuiltinId::EXPECT},
    {"array_product", BuiltinId::ARRAY_PRODUCT},
    {"array_last", BuiltinId::ARRAY_LAST},
    {"array_insert", BuiltinId::ARRAY_INSERT},
    {"str_pad_left", BuiltinId::STR_PAD_LEFT},
    {"str_pad_right", BuiltinId::STR_PAD_RIGHT},
    // Generic filter — works on arrays, strings, and maps
    {"filter", BuiltinId::FILTER},
    // String character filter
    {"str_filter", BuiltinId::STR_FILTER},
    // Map entry filter
    {"map_filter", BuiltinId::MAP_FILTER},
    // Shell command execution
    {"command", BuiltinId::COMMAND},
    {"shell", BuiltinId::COMMAND},   // alias
    // String: single-sided trim
    {"str_lstrip", BuiltinId::STR_LSTRIP},
    {"str_rstrip", BuiltinId::STR_RSTRIP},
    // String: remove all occurrences of substring
    {"str_remove", BuiltinId::STR_REMOVE},
    // Array: take / drop
    {"array_take", BuiltinId::ARRAY_TAKE},
    {"array_drop", BuiltinId::ARRAY_DROP},
    // Array: deduplicate consecutive equal elements
    {"array_unique", BuiltinId::ARRAY_UNIQUE},
    // Array: rotate left
    {"array_rotate", BuiltinId::ARRAY_ROTATE},
    // Array: mean
    {"array_mean", BuiltinId::ARRAY_MEAN},
    // Map: merge two maps, invert keys↔values
    {"map_merge",  BuiltinId::MAP_MERGE},
    {"map_invert", BuiltinId::MAP_INVERT},
    // Shell command execution with sudo (password provided as second arg)
    {"sudo_command", BuiltinId::SUDO_COMMAND},
    {"env_get",      BuiltinId::ENV_GET},
    {"env_set",      BuiltinId::ENV_SET},
    // String formatting
    {"str_format",   BuiltinId::STR_FORMAT},
    {"array_zip",    BuiltinId::ARRAY_ZIP},
    // bigint builtins
    {"bigint",           BuiltinId::BIGINT_NEW},
    {"bigint_add",       BuiltinId::BIGINT_ADD},
    {"bigint_sub",       BuiltinId::BIGINT_SUB},
    {"bigint_mul",       BuiltinId::BIGINT_MUL},
    {"bigint_div",       BuiltinId::BIGINT_DIV},
    {"bigint_mod",       BuiltinId::BIGINT_MOD},
    {"bigint_neg",       BuiltinId::BIGINT_NEG},
    {"bigint_abs",       BuiltinId::BIGINT_ABS},
    {"bigint_pow",       BuiltinId::BIGINT_POW},
    {"bigint_gcd",       BuiltinId::BIGINT_GCD},
    {"bigint_eq",        BuiltinId::BIGINT_EQ},
    {"bigint_lt",        BuiltinId::BIGINT_LT},
    {"bigint_le",        BuiltinId::BIGINT_LE},
    {"bigint_gt",        BuiltinId::BIGINT_GT},
    {"bigint_ge",        BuiltinId::BIGINT_GE},
    {"bigint_cmp",       BuiltinId::BIGINT_CMP},
    {"bigint_tostring",  BuiltinId::BIGINT_TOSTRING},
    {"bigint_to_i64",    BuiltinId::BIGINT_TO_I64},
    {"bigint_bit_length",BuiltinId::BIGINT_BIT_LENGTH},
    {"bigint_is_zero",   BuiltinId::BIGINT_IS_ZERO},
    {"bigint_is_negative",BuiltinId::BIGINT_IS_NEGATIVE},
    {"bigint_shl",       BuiltinId::BIGINT_SHL},
    {"bigint_shr",       BuiltinId::BIGINT_SHR},
    // Type-specific fast builtins
    {"mulhi",     BuiltinId::INT_MULHI},
    {"mulhi_u",   BuiltinId::UINT_MULHI},
    {"absdiff",   BuiltinId::INT_ABSDIFF},
    {"fast_sqrt", BuiltinId::FAST_SQRT},
    {"is_nan",    BuiltinId::FLOAT_IS_NAN},
    {"is_inf",    BuiltinId::FLOAT_IS_INF},
    // 2D column-major matrix builtins
    {"mat_new",    BuiltinId::MAT_NEW},
    {"mat_fill",   BuiltinId::MAT_FILL},
    {"mat_get",    BuiltinId::MAT_GET},
    {"mat_set",    BuiltinId::MAT_SET},
    {"mat_rows",   BuiltinId::MAT_ROWS},
    {"mat_cols",   BuiltinId::MAT_COLS},
    {"mat_mul",    BuiltinId::MAT_MUL},
    {"mat_transp", BuiltinId::MAT_TRANSP},
};

static BuiltinId lookupBuiltin(const std::string& name) {
    auto it = builtinLookup.find(std::string_view(name));
    return it != builtinLookup.end() ? it->second : BuiltinId::NONE;
}

// ---------------------------------------------------------------------------
// Compile-time constant folding helper
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
    const BuiltinId bid = lookupBuiltin(expr->callee);
    // Builtins are accepted whether they were parsed as std::foo() or bare
    // foo() to preserve compatibility with existing programs and tests.

    // ── Width-typed integer intrinsics (__tw_<op>_<N>) ───────────────────────
    // Generated by the parser for iN::op(args) where N < 64. Uses the narrower
    // LLVM intrinsic directly — avoids zext-to-i64, i64-intrinsic, trunc.
    //
    // Supported ops: popcount, clz, ctz, bitreverse, bswap,
    //                rotate_left, rotate_right, saturating_add, saturating_sub
    if (expr->callee.size() > 5 && expr->callee.substr(0,5) == "__tw_") {
        // Parse: __tw_<opname>_<width>
        const std::string suffix = expr->callee.substr(5); // e.g. "popcount_32"
        const auto uscore = suffix.rfind('_');
        if (uscore != std::string::npos) {
            const std::string opname   = suffix.substr(0, uscore);
            const std::string widthStr = suffix.substr(uscore + 1);
            int bw = 0;
            bool widthOk = !widthStr.empty();
            for (char c : widthStr) {
                if (!std::isdigit(static_cast<unsigned char>(c))) { widthOk=false; break; }
                bw = bw*10 + (c-'0');
            }
            if (widthOk && bw >= 1 && bw <= 64) {
                llvm::Type* narrowTy = llvm::Type::getIntNTy(*context, static_cast<unsigned>(bw));
                // Helper: truncate argument to the narrow type.
                auto toNarrow = [&](llvm::Value* v) -> llvm::Value* {
                    v = toDefaultType(v);
                    return builder->CreateTrunc(v, narrowTy, "tw.narrow");
                };
                // Helper: zero-extend narrow result back to i64.
                auto toWide = [&](llvm::Value* v) -> llvm::Value* {
                    return builder->CreateZExt(v, getDefaultType(), "tw.wide");
                };

                if (opname == "popcount" && expr->arguments.size() == 1) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get()))
                        return llvm::ConstantInt::get(getDefaultType(),
                            __builtin_popcountll(static_cast<uint64_t>(*cv) &
                                                  ((bw<64)?(1ULL<<bw)-1:~0ULL)));
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctpop, {narrowTy});
                    llvm::Value* r = builder->CreateCall(fn, {toNarrow(generateExpression(expr->arguments[0].get()))}, "tw.pop");
                    nonNegValues_.insert(r);
                    if (optimizationLevel >= OptimizationLevel::O1) {
                        // Range [0, bw+1) for tighter CVP.
                        auto* rangeMD = llvm::MDNode::get(*context, {
                            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, 0)),
                            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, bw+1))
                        });
                        llvm::cast<llvm::Instruction>(r)->setMetadata(llvm::LLVMContext::MD_range, rangeMD);
                    }
                    return toWide(r);
                }
                if (opname == "clz" && expr->arguments.size() == 1) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get())) {
                        uint64_t bits = static_cast<uint64_t>(*cv) & ((bw<64)?(1ULL<<bw)-1:~0ULL);
                        int64_t res = bits==0 ? bw : static_cast<int64_t>(__builtin_clzll(bits)-(64-bw));
                        return llvm::ConstantInt::get(getDefaultType(), res);
                    }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctlz, {narrowTy});
                    llvm::Value* r = builder->CreateCall(fn,
                        {toNarrow(generateExpression(expr->arguments[0].get())), builder->getFalse()}, "tw.clz");
                    nonNegValues_.insert(r);
                    if (optimizationLevel >= OptimizationLevel::O1) {
                        auto* rangeMD = llvm::MDNode::get(*context, {
                            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, 0)),
                            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, bw+1))
                        });
                        llvm::cast<llvm::Instruction>(r)->setMetadata(llvm::LLVMContext::MD_range, rangeMD);
                    }
                    return toWide(r);
                }
                if (opname == "ctz" && expr->arguments.size() == 1) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get())) {
                        uint64_t bits = static_cast<uint64_t>(*cv) & ((bw<64)?(1ULL<<bw)-1:~0ULL);
                        int64_t res = bits==0 ? bw : static_cast<int64_t>(__builtin_ctzll(bits));
                        return llvm::ConstantInt::get(getDefaultType(), res);
                    }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::cttz, {narrowTy});
                    llvm::Value* r = builder->CreateCall(fn,
                        {toNarrow(generateExpression(expr->arguments[0].get())), builder->getFalse()}, "tw.ctz");
                    nonNegValues_.insert(r);
                    if (optimizationLevel >= OptimizationLevel::O1) {
                        auto* rangeMD = llvm::MDNode::get(*context, {
                            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, 0)),
                            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, bw+1))
                        });
                        llvm::cast<llvm::Instruction>(r)->setMetadata(llvm::LLVMContext::MD_range, rangeMD);
                    }
                    return toWide(r);
                }
                if (opname == "bitreverse" && expr->arguments.size() == 1) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get())) {
                        uint64_t bits = static_cast<uint64_t>(*cv) & ((bw<64)?(1ULL<<bw)-1:~0ULL);
                        uint64_t rev = 0;
                        for (int i = 0; i < bw; ++i) rev |= ((bits>>i)&1ULL)<<(bw-1-i);
                        return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(rev));
                    }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::bitreverse, {narrowTy});
                    return toWide(builder->CreateCall(fn, {toNarrow(generateExpression(expr->arguments[0].get()))}, "tw.brev"));
                }
                if (opname == "bswap" && expr->arguments.size() == 1 && bw >= 16 && (bw%8)==0) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get())) {
                        uint64_t bits = static_cast<uint64_t>(*cv) & ((bw<64)?(1ULL<<bw)-1:~0ULL);
                        uint64_t swapped = 0;
                        int bytes = bw/8;
                        for (int i = 0; i < bytes; ++i)
                            swapped |= ((bits>>(i*8))&0xFF)<<((bytes-1-i)*8);
                        return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(swapped));
                    }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::bswap, {narrowTy});
                    return toWide(builder->CreateCall(fn, {toNarrow(generateExpression(expr->arguments[0].get()))}, "tw.bswap"));
                }
                if ((opname == "rotate_left" || opname == "rotl") && expr->arguments.size() == 2) {
                    llvm::Value* val = toNarrow(generateExpression(expr->arguments[0].get()));
                    llvm::Value* amt = toNarrow(generateExpression(expr->arguments[1].get()));
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::fshl, {narrowTy});
                    return toWide(builder->CreateCall(fn, {val, val, amt}, "tw.rotl"));
                }
                if ((opname == "rotate_right" || opname == "rotr") && expr->arguments.size() == 2) {
                    llvm::Value* val = toNarrow(generateExpression(expr->arguments[0].get()));
                    llvm::Value* amt = toNarrow(generateExpression(expr->arguments[1].get()));
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::fshr, {narrowTy});
                    return toWide(builder->CreateCall(fn, {val, val, amt}, "tw.rotr"));
                }
                if (opname == "saturating_add" && expr->arguments.size() == 2) {
                    if (auto a = tryFoldInt(expr->arguments[0].get()))
                        if (auto b = tryFoldInt(expr->arguments[1].get())) {
                            // Compile-time: clamp to [-(2^(bw-1)), 2^(bw-1)-1].
                            using I128 = __int128;
                            I128 res = static_cast<I128>(*a) + static_cast<I128>(*b);
                            int64_t lo = -(int64_t(1)<<(bw-1)), hi = (int64_t(1)<<(bw-1))-1;
                            if (res < lo) res = lo; if (res > hi) res = hi;
                            return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(res));
                        }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sadd_sat, {narrowTy});
                    llvm::Value* a = toNarrow(generateExpression(expr->arguments[0].get()));
                    llvm::Value* b = toNarrow(generateExpression(expr->arguments[1].get()));
                    return builder->CreateSExt(builder->CreateCall(fn, {a, b}, "tw.sadd_sat"), getDefaultType(), "tw.sadd_sat.wide");
                }
                if (opname == "saturating_sub" && expr->arguments.size() == 2) {
                    if (auto a = tryFoldInt(expr->arguments[0].get()))
                        if (auto b = tryFoldInt(expr->arguments[1].get())) {
                            using I128 = __int128;
                            I128 res = static_cast<I128>(*a) - static_cast<I128>(*b);
                            int64_t lo = -(int64_t(1)<<(bw-1)), hi = (int64_t(1)<<(bw-1))-1;
                            if (res < lo) res = lo; if (res > hi) res = hi;
                            return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(res));
                        }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ssub_sat, {narrowTy});
                    llvm::Value* a = toNarrow(generateExpression(expr->arguments[0].get()));
                    llvm::Value* b = toNarrow(generateExpression(expr->arguments[1].get()));
                    return builder->CreateSExt(builder->CreateCall(fn, {a, b}, "tw.ssub_sat"), getDefaultType(), "tw.ssub_sat.wide");
                }
            }
        }
    }

    // ── f32-typed float intrinsics (__tf_<op>) ────────────────────────────────
    // Generated by the parser for f32::op(args). Uses 32-bit LLVM intrinsics
    // to avoid f32↔f64 conversions — single VSQRTSS instead of cvtss2sd +
    // vsqrtsd + cvtsd2ss, for example.
    if (expr->callee.size() > 5 && expr->callee.substr(0,5) == "__tf_") {
        const std::string opname = expr->callee.substr(5);
        llvm::Type* f32Ty = llvm::Type::getFloatTy(*context);

        // Convert an i64 OmScript value to f32 (interpret as double then narrow).
        auto toF32 = [&](llvm::Value* v) -> llvm::Value* {
            if (v->getType()->isFloatTy()) return v;
            if (v->getType()->isDoubleTy())
                return builder->CreateFPTrunc(v, f32Ty, "tf.d2f");
            // i64 → double → float
            llvm::Value* d = builder->CreateSIToFP(v, llvm::Type::getDoubleTy(*context), "tf.i2d");
            return builder->CreateFPTrunc(d, f32Ty, "tf.d2f");
        };
        // Convert f32 result back to i64.
        auto f32ToI64 = [&](llvm::Value* v) -> llvm::Value* {
            llvm::Value* d = builder->CreateFPExt(v, llvm::Type::getDoubleTy(*context), "tf.f2d");
            return builder->CreateFPToSI(d, getDefaultType(), "tf.d2i");
        };
        // Unary f32 LLVM intrinsic.
        auto unaryF32 = [&](llvm::Intrinsic::ID iid) -> llvm::Value* {
            if (expr->arguments.size() != 1) codegenError("expected 1 argument", expr);
            llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), iid, {f32Ty});
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.unary"));
        };
        // Binary f32 LLVM intrinsic.
        auto binaryF32 = [&](llvm::Intrinsic::ID iid) -> llvm::Value* {
            if (expr->arguments.size() != 2) codegenError("expected 2 arguments", expr);
            llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), iid, {f32Ty});
            llvm::Value* a = toF32(generateExpression(expr->arguments[0].get()));
            llvm::Value* b = toF32(generateExpression(expr->arguments[1].get()));
            return f32ToI64(builder->CreateCall(fn, {a, b}, "tf.binary"));
        };

        if      (opname=="sqrt")   return unaryF32(llvm::Intrinsic::sqrt);
        else if (opname=="sin")    return unaryF32(llvm::Intrinsic::sin);
        else if (opname=="cos")    return unaryF32(llvm::Intrinsic::cos);
        else if (opname=="tan")    {
            // LLVM has no tan intrinsic; call tanf() from libm.
            if (expr->arguments.size() != 1) codegenError("expected 1 argument", expr);
            llvm::FunctionCallee tanf = module->getOrInsertFunction(
                "tanf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(tanf, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.tan"));
        }
        else if (opname=="asin")   {
            llvm::FunctionCallee fn = module->getOrInsertFunction(
                "asinf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.asin"));
        }
        else if (opname=="acos")   {
            llvm::FunctionCallee fn = module->getOrInsertFunction(
                "acosf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.acos"));
        }
        else if (opname=="atan")   {
            llvm::FunctionCallee fn = module->getOrInsertFunction(
                "atanf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.atan"));
        }
        else if (opname=="atan2")  {
            if (expr->arguments.size() != 2) codegenError("expected 2 arguments", expr);
            llvm::FunctionCallee fn = module->getOrInsertFunction(
                "atan2f", llvm::FunctionType::get(f32Ty, {f32Ty, f32Ty}, false));
            llvm::Value* a = toF32(generateExpression(expr->arguments[0].get()));
            llvm::Value* b = toF32(generateExpression(expr->arguments[1].get()));
            return f32ToI64(builder->CreateCall(fn, {a, b}, "tf.atan2"));
        }
        else if (opname=="log")    return unaryF32(llvm::Intrinsic::log);
        else if (opname=="log2")   return unaryF32(llvm::Intrinsic::log2);
        else if (opname=="log10")  return unaryF32(llvm::Intrinsic::log10);
        else if (opname=="exp")    return unaryF32(llvm::Intrinsic::exp);
        else if (opname=="exp2")   return unaryF32(llvm::Intrinsic::exp2);
        else if (opname=="cbrt")   {
            llvm::FunctionCallee fn = module->getOrInsertFunction(
                "cbrtf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.cbrt"));
        }
        else if (opname=="hypot")  {
            if (expr->arguments.size() != 2) codegenError("expected 2 arguments", expr);
            llvm::FunctionCallee fn = module->getOrInsertFunction(
                "hypotf", llvm::FunctionType::get(f32Ty, {f32Ty, f32Ty}, false));
            llvm::Value* a = toF32(generateExpression(expr->arguments[0].get()));
            llvm::Value* b = toF32(generateExpression(expr->arguments[1].get()));
            return f32ToI64(builder->CreateCall(fn, {a, b}, "tf.hypot"));
        }
        else if (opname=="fma")    {
            if (expr->arguments.size() != 3) codegenError("expected 3 arguments", expr);
            llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::fma, {f32Ty});
            llvm::Value* a = toF32(generateExpression(expr->arguments[0].get()));
            llvm::Value* b = toF32(generateExpression(expr->arguments[1].get()));
            llvm::Value* c = toF32(generateExpression(expr->arguments[2].get()));
            return f32ToI64(builder->CreateCall(fn, {a, b, c}, "tf.fma"));
        }
        else if (opname=="copysign") return binaryF32(llvm::Intrinsic::copysign);
        else if (opname=="fast_sqrt") {
            if (expr->arguments.size() != 1) codegenError("expected 1 argument", expr);
            llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sqrt, {f32Ty});
            llvm::CallInst* call = builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.fsqrt");
            llvm::FastMathFlags fmf;
            fmf.setAllowReassoc(true); fmf.setNoNaNs(true); fmf.setNoInfs(true);
            call->setFastMathFlags(fmf);
            return f32ToI64(call);
        }
        // Unknown __tf_ op — fall through to user-defined function lookup.
    }

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
        } else if (isStringExpr(argExpr)) {
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
        // Constant-fold abs(any const expr): eliminates the intrinsic call entirely.
        if (auto cv = tryFoldInt(expr->arguments[0].get())) {
            const int64_t result = (*cv < 0) ? -*cv : *cv;
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

        // Constant-fold len() on any expression that can be reduced to a
        // compile-time constant string at the AST level.  Try both the
        // fast string-concat folder and the full unified evaluator so that
        // multi-param string-returning functions are also handled.
        {
            std::string folded;
            if (tryFoldStringConcat(expr->arguments[0].get(), folded)) {
                auto* result = llvm::ConstantInt::get(getDefaultType(),
                                                      static_cast<int64_t>(folded.size()));
                nonNegValues_.insert(result);
                return result;
            }
            // tryFoldExprToConst handles multi-param calls (e.g. greet("world"))
            // that tryFoldStringConcat doesn't traverse.
            auto cv = tryFoldExprToConst(expr->arguments[0].get());
            if (cv && cv->kind == ConstValue::Kind::String) {
                auto* result = llvm::ConstantInt::get(
                    getDefaultType(), static_cast<int64_t>(cv->strVal.size()));
                nonNegValues_.insert(result);
                return result;
            }
            // Fold len() on const arrays: `const arr = [1,2,3]; len(arr)` → 3.
            if (cv && cv->kind == ConstValue::Kind::Array) {
                auto* result = llvm::ConstantInt::get(
                    getDefaultType(), static_cast<int64_t>(cv->arrVal.size()));
                nonNegValues_.insert(result);
                return result;
            }
        }

        // Constant-fold len(array_fill(N, val)): when N is a compile-time
        // constant the array length is known at compile time.  Avoids a
        // runtime load from the array header and enables further constant
        // propagation.  Use tryFoldInt so const variables work too, not just
        // plain integer literals.
        if (auto* call = dynamic_cast<CallExpr*>(expr->arguments[0].get())) {
            if (call->callee == "array_fill" && call->arguments.size() == 2) {
                if (auto n = tryFoldInt(call->arguments[0].get())) {
                    if (*n >= 0) {
                        auto* result = llvm::ConstantInt::get(getDefaultType(), *n);
                        nonNegValues_.insert(result);
                        return result;
                    }
                }
            }
            // Constant-fold len(range(a, b)) = max(0, b - a) when both args are constants.
            if (call->callee == "range" && call->arguments.size() == 2) {
                if (auto a = tryFoldInt(call->arguments[0].get())) {
                    if (auto b = tryFoldInt(call->arguments[1].get())) {
                        const int64_t count = *b > *a ? *b - *a : 0;
                        auto* result = llvm::ConstantInt::get(getDefaultType(), count);
                        nonNegValues_.insert(result);
                        optStats_.constFolded++;
                        return result;
                    }
                }
            }
            // Constant-fold len(range_step(a, b, s)) = max(0, ceil((b-a)/s)) when all args are constants.
            if (call->callee == "range_step" && call->arguments.size() == 3) {
                if (auto a = tryFoldInt(call->arguments[0].get())) {
                    if (auto b = tryFoldInt(call->arguments[1].get())) {
                        if (auto s = tryFoldInt(call->arguments[2].get())) {
                            if (*s != 0) {
                                int64_t count = 0;
                                if (*s > 0 && *b > *a)
                                    count = (*b - *a + *s - 1) / *s;
                                else if (*s < 0 && *b < *a)
                                    count = (*a - *b + (-*s) - 1) / (-*s);
                                auto* result = llvm::ConstantInt::get(getDefaultType(), count);
                                nonNegValues_.insert(result);
                                optStats_.constFolded++;
                                return result;
                            }
                        }
                    }
                }
            }
            // Fold len(str_chars(s)) = len(s): str_chars returns an array with
            // one element per character, so its length equals the string length.
            // Avoids allocating the char array when only the length is needed.
            if (call->callee == "str_chars" && call->arguments.size() == 1) {
                // Generate the string length directly via strlen.
                llvm::Value* strArg = generateExpression(call->arguments[0].get());
                llvm::Value* strPtr = strArg->getType()->isPointerTy()
                    ? strArg
                    : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "len.chars.ptr");
                llvm::Value* rawLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "len.chars.strlen");
                auto* result = rawLen->getType() == getDefaultType()
                    ? rawLen
                    : builder->CreateZExtOrTrunc(rawLen, getDefaultType(), "len.chars.sz");
                nonNegValues_.insert(result);
                if (optimizationLevel >= OptimizationLevel::O1)
                    llvm::cast<llvm::Instruction>(rawLen)->setMetadata(
                        llvm::LLVMContext::MD_range, arrayLenRangeMD_);
                return result;
            }
        }
        // Also fold len(array_const_expr) via the unified evaluator.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                auto* result = llvm::ConstantInt::get(
                    getDefaultType(), static_cast<int64_t>(cv->arrVal.size()));
                nonNegValues_.insert(result);
                optStats_.constFolded++;
                return result;
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
        if (isStringExpr(argExpr)) {
            // Reconstruct the char* for strlen when the value is stored as i64.
            llvm::Value* strPtr = arg->getType()->isPointerTy()
                                      ? arg
                                      : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "len.sptr");
            llvm::Value* rawLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "len.strlen");
            // !range [0, INT64_MAX): strlen always returns non-negative.
            if (optimizationLevel >= OptimizationLevel::O1)
                llvm::cast<llvm::Instruction>(rawLen)->setMetadata(
                    llvm::LLVMContext::MD_range, arrayLenRangeMD_);
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
                llvm::Value* arrlenLoad = emitLoadArrayLen(arrPtr, "arrlen");
        return arrlenLoad;
    }

    if (bid == BuiltinId::MIN) {
        validateArgCount(expr, "min", 2);
        // Constant-fold min(a, b) when both are compile-time constants.
        if (auto ca = tryFoldInt(expr->arguments[0].get())) {
            if (auto cb = tryFoldInt(expr->arguments[1].get())) {
                const int64_t result = std::min(*ca, *cb);
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
        // Constant-fold max(a, b) when both are compile-time constants.
        if (auto ca = tryFoldInt(expr->arguments[0].get())) {
            if (auto cb = tryFoldInt(expr->arguments[1].get())) {
                const int64_t result = std::max(*ca, *cb);
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
        // Constant-fold sign(any const expr).
        if (auto cv = tryFoldInt(expr->arguments[0].get())) {
            const int64_t result = (*cv > 0) ? 1 : ((*cv < 0) ? -1 : 0);
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
        auto* signVal = builder->CreateSelect(isPos, pos, negOrZero, "signval");
        // !range [-1, 2): sign always returns -1, 0, or 1.  This lets CVP/LVI
        // fold downstream comparisons like sign(x) == 2 → false.
        auto* signRange = llvm::MDNode::get(*context, {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(-1), true)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(getDefaultType(), 2))});
        if (auto* signInst = llvm::dyn_cast<llvm::Instruction>(signVal))
            signInst->setMetadata(llvm::LLVMContext::MD_range, signRange);
        return signVal;
    }

    if (bid == BuiltinId::CLAMP) {
        validateArgCount(expr, "clamp", 3);
        // Constant-fold clamp(val, lo, hi) when all three are compile-time constants.
        if (auto cv = tryFoldInt(expr->arguments[0].get())) {
            if (auto cl = tryFoldInt(expr->arguments[1].get())) {
                if (auto ch = tryFoldInt(expr->arguments[2].get())) {
                    const int64_t result = std::max(*cl, std::min(*cv, *ch));
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
        // Constant-fold pow(base, exp) when both are compile-time constants and exp >= 0.
        // Eliminates the entire exponentiation loop at compile time.
        if (auto cb = tryFoldInt(expr->arguments[0].get())) {
            if (auto ce = tryFoldInt(expr->arguments[1].get())) {
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

        // Constant-exponent fast path: for small constant non-negative exponents
        // emit an unrolled multiply chain instead of the binary-exp loop.
        // This avoids 6 basic blocks and a loop for common cases like pow(x,2).
        if (auto* expCI = llvm::dyn_cast<llvm::ConstantInt>(exp)) {
            int64_t ev = expCI->getSExtValue();
            bool baseNonNeg = nonNegValues_.count(base) > 0;
            auto mkMulP = [&](llvm::Value* a, llvm::Value* b, const char* nm) -> llvm::Value* {
                return baseNonNeg ? builder->CreateNSWMul(a, b, nm)
                                  : builder->CreateMul(a, b, nm);
            };
            if (ev == 0) {
                auto* r = llvm::ConstantInt::get(getDefaultType(), 1);
                nonNegValues_.insert(r);
                return r;
            }
            if (ev == 1) return base;
            if (ev == 2) {
                auto* r = mkMulP(base, base, "powb.2");
                if (baseNonNeg) nonNegValues_.insert(r);
                return r;
            }
            if (ev == 3) {
                auto* sq = mkMulP(base, base, "powb.3.sq");
                auto* r  = mkMulP(sq, base, "powb.3");
                if (baseNonNeg) nonNegValues_.insert(r);
                return r;
            }
            if (ev == 4) {
                auto* sq = mkMulP(base, base, "powb.4.sq");
                auto* r  = mkMulP(sq, sq, "powb.4");
                nonNegValues_.insert(r);  // even exponent → always non-neg
                return r;
            }
            if (ev == 5) {
                auto* sq = mkMulP(base, base, "powb.5.sq");
                auto* q4 = mkMulP(sq, sq, "powb.5.q4");
                auto* r  = mkMulP(q4, base, "powb.5");
                if (baseNonNeg) nonNegValues_.insert(r);
                return r;
            }
            if (ev == 6) {
                auto* sq = mkMulP(base, base, "powb.6.sq");
                auto* cb = mkMulP(sq, base, "powb.6.cb");
                auto* r  = mkMulP(cb, cb, "powb.6");
                nonNegValues_.insert(r);  // even exponent → always non-neg
                return r;
            }
            if (ev == 7) {
                auto* sq = mkMulP(base, base, "powb.7.sq");
                auto* cb = mkMulP(sq, base, "powb.7.cb");
                auto* p6 = mkMulP(cb, cb, "powb.7.p6");
                auto* r  = mkMulP(p6, base, "powb.7");
                if (baseNonNeg) nonNegValues_.insert(r);
                return r;
            }
            if (ev == 8) {
                auto* sq = mkMulP(base, base, "powb.8.sq");
                auto* q4 = mkMulP(sq, sq, "powb.8.q4");
                auto* r  = mkMulP(q4, q4, "powb.8");
                nonNegValues_.insert(r);  // even exponent → always non-neg
                return r;
            }
        }

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Binary exponentiation (exponentiation by squaring): O(log n) in the exponent
        llvm::BasicBlock* negExpBB  = llvm::BasicBlock::Create(*context, "pow.negexp", function);
        llvm::BasicBlock* zeroExpBB = llvm::BasicBlock::Create(*context, "pow.zeroexp", function);
        llvm::BasicBlock* posExpBB  = llvm::BasicBlock::Create(*context, "pow.posexp", function);
        llvm::BasicBlock* loopBB    = llvm::BasicBlock::Create(*context, "pow.loop", function);
        llvm::BasicBlock* oddBB     = llvm::BasicBlock::Create(*context, "pow.odd", function);
        llvm::BasicBlock* squareBB  = llvm::BasicBlock::Create(*context, "pow.square", function);
        llvm::BasicBlock* doneBB    = llvm::BasicBlock::Create(*context, "pow.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1, true);

        // pow(x, 0) == 1 always — handle before the negative/positive split to
        // avoid entering the 6-BB loop structure for this common edge case.
        llvm::Value* isZeroExp = builder->CreateICmpEQ(exp, zero, "pow.isz");
        auto* zeroW = llvm::MDBuilder(*context).createBranchWeights(1, 200);
        llvm::BasicBlock* checkSignBB = llvm::BasicBlock::Create(*context, "pow.chksign", function);
        builder->CreateCondBr(isZeroExp, zeroExpBB, checkSignBB, zeroW);

        builder->SetInsertPoint(zeroExpBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(checkSignBB);
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
        // Use NSWMul when the base is known non-negative.
        bool powBaseNonNeg = nonNegValues_.count(base) > 0;
        llvm::Value* newResult = powBaseNonNeg
            ? builder->CreateNSWMul(result, curBase, "pow.mul")
            : builder->CreateMul(result, curBase, "pow.mul");
        llvm::Value* resultSel = builder->CreateSelect(isOdd, newResult, result, "pow.rsel");
        builder->CreateBr(squareBB);

        // Square the base and halve the exponent
        builder->SetInsertPoint(squareBB);
        llvm::Value* newBase = powBaseNonNeg
            ? builder->CreateNSWMul(curBase, curBase, "pow.sq")
            : builder->CreateMul(curBase, curBase, "pow.sq");
        llvm::Value* newCounter = builder->CreateAShr(counter, one, "pow.halve");
        result->addIncoming(resultSel, squareBB);
        curBase->addIncoming(newBase, squareBB);
        counter->addIncoming(newCounter, squareBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* finalResult = builder->CreatePHI(getDefaultType(), 3, "pow.final");
        finalResult->addIncoming(one,    zeroExpBB);  // pow(x, 0) == 1
        finalResult->addIncoming(zero,   negExpBB);   // pow(x, neg) == 0
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
        if (isStringExpr(expr->arguments[0].get())) {
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
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1024));
        // Declare stdin as external global
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::GlobalVariable* stdinVar = module->getGlobalVariable("stdin");
        if (!stdinVar) {
            stdinVar =
                new llvm::GlobalVariable(*module, ptrTy, false, llvm::GlobalValue::ExternalLinkage, nullptr, "stdin");
        }
        llvm::Value* stdinVal = builder->CreateLoad(ptrTy, stdinVar, "inputln.stdin");
        // stdin is always non-null on POSIX systems — communicating this to
        // the optimizer lets it remove redundant null checks downstream.
        llvm::cast<llvm::LoadInst>(stdinVal)->setMetadata(
            llvm::LLVMContext::MD_nonnull, llvm::MDNode::get(*context, {}));
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
        // Constant-fold sqrt(n) for compile-time integer arguments.
        if (auto v = tryFoldInt(expr->arguments[0].get())) {
            if (*v >= 0)
                return llvm::ConstantInt::get(getDefaultType(),
                    static_cast<int64_t>(std::sqrt(static_cast<double>(*v))));
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Use the llvm.sqrt intrinsic which maps directly to hardware sqrtsd/sqrtss
        // instructions on x86, producing results in a single cycle on modern CPUs.
        const bool inputIsDouble = x->getType()->isDoubleTy();
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
            const llvm::FastMathFlags strictFlags;
            // All flags default to off — full IEEE compliance.
            fpInst->setFastMathFlags(strictFlags);
            hgoe::setInstructionPrecision(fpInst, hgoe::FPPrecision::Strict);
        }
        return builder->CreateFPToSI(result, getDefaultType(), "precise.result");
    }

    if (bid == BuiltinId::IS_EVEN) {
        validateArgCount(expr, "is_even", 1);
        // Constant-fold is_even(any const expr).
        if (auto cv = tryFoldInt(expr->arguments[0].get())) {
            const int64_t result = ((*cv & 1) == 0) ? 1 : 0;
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
        // Constant-fold is_odd(any const expr).
        if (auto cv = tryFoldInt(expr->arguments[0].get())) {
            const int64_t result = (*cv & 1) ? 1 : 0;
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
        // Constant-fold sum([c0, c1, ...]) when the array is a compile-time literal.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                int64_t total = 0;
                bool allInt = true;
                for (const auto& elem : cv->arrVal) {
                    if (elem.kind != ConstValue::Kind::Integer) { allInt = false; break; }
                    total += elem.intVal;
                }
                if (allInt) {
                    optStats_.constFolded++;
                    return llvm::ConstantInt::get(getDefaultType(), total);
                }
            }
        }
        // Constant-fold sum(array_fill(n, v)) → n * v when both n and v are constant.
        if (auto* call = dynamic_cast<CallExpr*>(expr->arguments[0].get())) {
            if (call->callee == "array_fill" && call->arguments.size() == 2) {
                if (auto n = tryFoldInt(call->arguments[0].get())) {
                    if (auto v = tryFoldInt(call->arguments[1].get())) {
                        if (*n >= 0) {
                            optStats_.constFolded++;
                            auto* ci = llvm::ConstantInt::get(getDefaultType(), (*n) * (*v));
                            return ci;
                        }
                    }
                }
            }
            // Constant-fold sum(range(a, b)) → arithmetic series (b-a)*(a+b-1)/2.
            // range(a, b) produces [a, a+1, ..., b-1]; sum = (b-a)*(a+b-1)/2.
            // One of (b-a) and (a+b-1) is always even, so integer division is exact.
            if (call->callee == "range" && call->arguments.size() == 2) {
                if (auto a = tryFoldInt(call->arguments[0].get())) {
                    if (auto b = tryFoldInt(call->arguments[1].get())) {
                        const int64_t count = *b > *a ? *b - *a : 0;
                        int64_t series;
                        if (count == 0) {
                            series = 0;
                        } else if (count % 2 == 0) {
                            series = (count / 2) * (*a + *b - 1);
                        } else {
                            series = count * ((*a + *b - 1) / 2);
                        }
                        optStats_.constFolded++;
                        return llvm::ConstantInt::get(getDefaultType(), series);
                    }
                }
            }
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // Array layout: [length, elem0, elem1, ...]
        llvm::Value* arrPtr =
            arg->getType()->isPointerTy() ? arg : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "sum.arrptr");
                llvm::Value* sumLenLoad = emitLoadArrayLen(arrPtr, "sum.len");
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
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "sum.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
        // nsw+nuw: OmScript guarantees array elements are initialized i64 values;
        // adding them cannot produce UB in well-formed programs (user opt-in via
        // type guarantees).  nsw+nuw lets SCEV compute tighter trip-count bounds
        // and enables the vectorizer to use SIMD add-reduction without sign fixups.
        llvm::Value* newAcc = builder->CreateAdd(acc, elem, "sum.newacc",
                                                  /*HasNUW=*/true, /*HasNSW=*/true);
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        // Eliminates a redundant add and provides SCEV with tight nsw+nuw flags.
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(offset, bodyBB);
        attachLoopMetadataVec(
            llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

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
                llvm::Value* swapLenLoad = emitLoadArrayLen(arrPtr, "swap.len");
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
        {
            std::string msg = expr->line > 0
                ? std::string("Runtime error: swap index out of bounds at line ") + std::to_string(expr->line) + "\n"
                : "Runtime error: swap index out of bounds\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "swap_oob_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Elements are at offset (index + 1)
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* offI = builder->CreateAdd(i, one, "swap.offi", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* offJ = builder->CreateAdd(j, one, "swap.offj", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* ptrI = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offI, "swap.ptri");
        llvm::Value* ptrJ = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offJ, "swap.ptrj");
        llvm::Value* valI = emitLoadArrayElem(ptrI, "swap.vali");
        llvm::Value* valJ = emitLoadArrayElem(ptrJ, "swap.valj");
        emitStoreArrayElem(valJ, ptrI);
        emitStoreArrayElem(valI, ptrJ);
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::REVERSE) {
        validateArgCount(expr, "reverse", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr =
            arg->getType()->isPointerTy() ? arg : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "rev.arrptr");
                llvm::Value* revLenLoad = emitLoadArrayLen(arrPtr, "rev.len");
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
        llvm::Value* valLo = emitLoadArrayElem(ptrLo, "rev.vallo");
        llvm::Value* valHi = emitLoadArrayElem(ptrHi, "rev.valhi");
        if (optimizationLevel >= OptimizationLevel::O1) {
            llvm::cast<llvm::Instruction>(valLo)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
            llvm::cast<llvm::Instruction>(valHi)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
        }
        emitStoreArrayElem(valHi, ptrLo);
        emitStoreArrayElem(valLo, ptrHi);
        llvm::Value* newLo = builder->CreateAdd(lo, one, "rev.newlo", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* newHi = builder->CreateSub(hi, one, "rev.newhi", /*HasNUW=*/true, /*HasNSW=*/true);
        lo->addIncoming(newLo, bodyBB);
        hi->addIncoming(newHi, bodyBB);
        attachLoopMetadataVec(
            llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

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
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 2));
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
        // Use the unsigned-subtract range-check trick:
        //   uppercase: (x - 'A') <=u 25  (covers 'A'..'Z' in one comparison)
        //   lowercase: (x - 'a') <=u 25  (covers 'a'..'z' in one comparison)
        // This produces 4 instructions (2 subs + 2 unsigned-compares + or +
        // zext) vs the previous 6 (4 signed-compares + 2 ands + or + zext),
        // and LLVM's loop vectorizer handles the unsigned-compare pattern
        // better when scanning strings.
        auto* cA = llvm::ConstantInt::get(getDefaultType(), 65);   // 'A'
        auto* ca = llvm::ConstantInt::get(getDefaultType(), 97);   // 'a'
        auto* c25 = llvm::ConstantInt::get(getDefaultType(), 25);  // 'Z'-'A'
        llvm::Value* upper = builder->CreateICmpULE(
            builder->CreateSub(x, cA, "sub.A"), c25, "isupper");
        llvm::Value* lower = builder->CreateICmpULE(
            builder->CreateSub(x, ca, "sub.a"), c25, "islower");
        llvm::Value* isAlpha = builder->CreateOr(upper, lower, "isalpha");
        auto* result = builder->CreateZExt(isAlpha, getDefaultType(), "alphaval");
        // is_alpha always returns 0 or 1.
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::IS_DIGIT) {
        validateArgCount(expr, "is_digit", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_digit() is an integer operation
        x = toDefaultType(x);
        // Use the unsigned-subtract range-check trick:
        //   (x - '0') <=u 9  covers exactly '0'..'9' in a single comparison.
        // This is one instruction fewer than the previous two-comparison form
        // (ge0 + le9 + and) and is the canonical pattern used by libc/LLVM.
        auto* c0 = llvm::ConstantInt::get(getDefaultType(), 48);  // '0'
        auto* c9 = llvm::ConstantInt::get(getDefaultType(), 9);   // '9'-'0'
        llvm::Value* isDigit = builder->CreateICmpULE(
            builder->CreateSub(x, c0, "sub.0"), c9, "isdigit");
        auto* result = builder->CreateZExt(isDigit, getDefaultType(), "digitval");
        // is_digit always returns 0 or 1.
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::IS_UPPER) {
        validateArgCount(expr, "is_upper", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        x = toDefaultType(x);
        // (x - 'A') <=u 25  covers exactly 'A'..'Z'
        auto* cA = llvm::ConstantInt::get(getDefaultType(), 65);  // 'A'
        auto* c25 = llvm::ConstantInt::get(getDefaultType(), 25); // 'Z'-'A'
        llvm::Value* isUpper = builder->CreateICmpULE(
            builder->CreateSub(x, cA, "sub.A"), c25, "isupper");
        auto* result = builder->CreateZExt(isUpper, getDefaultType(), "upperval");
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::IS_LOWER) {
        validateArgCount(expr, "is_lower", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        x = toDefaultType(x);
        // (x - 'a') <=u 25  covers exactly 'a'..'z'
        auto* ca = llvm::ConstantInt::get(getDefaultType(), 97);  // 'a'
        auto* c25 = llvm::ConstantInt::get(getDefaultType(), 25); // 'z'-'a'
        llvm::Value* isLower = builder->CreateICmpULE(
            builder->CreateSub(x, ca, "sub.a"), c25, "islower");
        auto* result = builder->CreateZExt(isLower, getDefaultType(), "lowerval");
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::IS_SPACE) {
        validateArgCount(expr, "is_space", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        x = toDefaultType(x);
        // C whitespace: ' '(32), '\t'(9), '\n'(10), '\v'(11), '\f'(12), '\r'(13)
        // Split into two range checks:
        //   (x - 9) <=u 4   covers 9,10,11,12,13
        //   x == 32          covers ' '
        auto* c9 = llvm::ConstantInt::get(getDefaultType(), 9);   // '\t'
        auto* c4 = llvm::ConstantInt::get(getDefaultType(), 4);   // '\r'-'\t'
        auto* c32 = llvm::ConstantInt::get(getDefaultType(), 32); // ' '
        llvm::Value* isCtrl = builder->CreateICmpULE(
            builder->CreateSub(x, c9, "sub.9"), c4, "isctrl");
        llvm::Value* isSpc = builder->CreateICmpEQ(x, c32, "isspc");
        llvm::Value* isSpace = builder->CreateOr(isCtrl, isSpc, "isspace");
        auto* result = builder->CreateZExt(isSpace, getDefaultType(), "spaceval");
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::IS_ALNUM) {
        validateArgCount(expr, "is_alnum", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        x = toDefaultType(x);
        // Alphanumeric: letter (A-Z or a-z) or digit (0-9).
        // Three range checks combined with OR:
        //   (x - 'A') <=u 25  uppercase
        //   (x - 'a') <=u 25  lowercase
        //   (x - '0') <=u 9   digit
        auto* cA = llvm::ConstantInt::get(getDefaultType(), 65);  // 'A'
        auto* ca = llvm::ConstantInt::get(getDefaultType(), 97);  // 'a'
        auto* c0 = llvm::ConstantInt::get(getDefaultType(), 48);  // '0'
        auto* c25 = llvm::ConstantInt::get(getDefaultType(), 25); // 'Z'-'A'
        auto* c9  = llvm::ConstantInt::get(getDefaultType(), 9);  // '9'-'0'
        llvm::Value* upper = builder->CreateICmpULE(
            builder->CreateSub(x, cA, "sub.A"), c25, "isupper2");
        llvm::Value* lower = builder->CreateICmpULE(
            builder->CreateSub(x, ca, "sub.a"), c25, "islower2");
        llvm::Value* digit = builder->CreateICmpULE(
            builder->CreateSub(x, c0, "sub.0"), c9,  "isdigit2");
        llvm::Value* isAlnum = builder->CreateOr(
            builder->CreateOr(upper, lower, "isalpha2"), digit, "isalnum");
        auto* result = builder->CreateZExt(isAlnum, getDefaultType(), "alnumval");
        nonNegValues_.insert(result);
        return result;
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
        } else if (isStringExpr(expr->arguments[0].get())) {
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
        {
            std::string msg = expr->line > 0
                ? std::string("Runtime error: assertion failed at line ") + std::to_string(expr->line) + "\n"
                : "Runtime error: assertion failed\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "assert_fail_msg")});
        }
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
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(result)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        return result;
    }

    if (bid == BuiltinId::CHAR_AT) {
        validateArgCount(expr, "char_at", 2);

        // ── Compile-time char_at folding ────────────────────────────
        // When both the string and index are compile-time constants,
        // fold char_at("hello", 1) → 'e' (ASCII 101) at compile time.
        // This eliminates the runtime strlen + bounds check + load entirely.
        if (auto strConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto idxConst = tryFoldInt(expr->arguments[1].get())) {
                const int64_t idx = *idxConst;
                const int64_t len = static_cast<int64_t>(strConst->size());
                if (idx >= 0 && idx < len) {
                    const char ch = (*strConst)[static_cast<size_t>(idx)];
                    return llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(static_cast<unsigned char>(ch)));
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
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
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
        {
            std::string msg = expr->line > 0
                ? std::string("Runtime error: char_at index out of bounds at line ") + std::to_string(expr->line) + "\n"
                : "Runtime error: char_at index out of bounds\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "charat_oob_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Load char via GEP
        llvm::Value* charPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strPtr, idxArg, "charat.gep");
        auto* charVal = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "charat.char");
        charVal->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        // Zero-extend to i64; result is always in [0, 256).
        auto* result = builder->CreateZExt(charVal, getDefaultType(), "charat.ext");
        nonNegValues_.insert(result);
        return result;
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
        auto* streqResult = builder->CreateZExt(isEqual, getDefaultType(), "streq.result");
        nonNegValues_.insert(streqResult);
        return streqResult;
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
            nonNegValues_.insert(realLen);
            if (optimizationLevel >= OptimizationLevel::O1)
                llvm::cast<llvm::Instruction>(realLen)->setMetadata(
                    llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            builder->CreateBr(mergeBB);

            builder->SetInsertPoint(mergeBB);
            llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "concat.len1");
            phi->addIncoming(cachedLen, cachedBB);
            phi->addIncoming(realLen, strlenBB);
            len1 = phi;
            // Both incoming values are non-negative (cached len from nonNegValues_, strlen has !range);
            // track the PHI so downstream operations benefit from the non-negative hint.
            nonNegValues_.insert(phi);
        } else {
            len1 = builder->CreateCall(getOrDeclareStrlen(), {lhsPtr}, "concat.len1");
            nonNegValues_.insert(len1);
            if (optimizationLevel >= OptimizationLevel::O1)
                llvm::cast<llvm::Instruction>(len1)->setMetadata(
                    llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        }
        llvm::Value* len2 = builder->CreateCall(getOrDeclareStrlen(), {rhsPtr}, "concat.len2");
        nonNegValues_.insert(len2);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(len2)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
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
            // nextPow2 via ctlz intrinsic: 1 << (64 - ctlz(minSize - 1))
            // This replaces a 6-shift OR-cascade (~14 instructions) with 4
            // instructions (sub, ctlz, sub, shl), which maps to a single
            // BSR/LZCNT + shift on x86-64.
            llvm::Value* minSize =
                builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "concat.minsize", /*HasNUW=*/true, /*HasNSW=*/true);
            llvm::Value* one64 = llvm::ConstantInt::get(getDefaultType(), 1);
            llvm::Value* v = builder->CreateSub(minSize, one64, "concat.pm1", /*HasNUW=*/true, /*HasNSW=*/true);
            llvm::Function* ctlzFn = OMSC_GET_INTRINSIC(module.get(),
                llvm::Intrinsic::ctlz, {getDefaultType()});
            // is_zero_poison=true: minSize = totalLen + 1 >= 2 (totalLen is the
            // sum of two string lengths and is always >= 1 on any concat path),
            // so v (= minSize - 1) is always >= 1, never zero.
            llvm::Value* lz = builder->CreateCall(ctlzFn,
                {v, llvm::ConstantInt::getTrue(*context)}, "concat.lz");
            llvm::Value* shift = builder->CreateSub(
                llvm::ConstantInt::get(getDefaultType(), 64), lz, "concat.shift");
            llvm::Value* allocSize = builder->CreateShl(one64, shift, "concat.allocsize", /*HasNUW=*/true, /*HasNSW=*/true);
            buf = builder->CreateCall(getOrDeclareRealloc(), {lhsPtr, allocSize}, "concat.buf");
            // memcpy(buf + len1, rhs, len2) — only append the RHS portion
            llvm::Value* dst2 = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, len1, "concat.dst2");
            builder->CreateCall(getOrDeclareMemcpy(), {dst2, rhsPtr, len2});
        } else {
            llvm::Value* allocSize =
                builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "concat.allocsize", /*HasNUW=*/true, /*HasNSW=*/true);
            buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "concat.buf");
            llvm::cast<llvm::CallInst>(buf)->addRetAttr(
                llvm::Attribute::getWithDereferenceableBytes(*context, 1));
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
        // Constant-fold gcd(a, b) when both are compile-time constants.
        if (auto va = tryFoldInt(expr->arguments[0].get())) {
            if (auto vb = tryFoldInt(expr->arguments[1].get())) {
                uint64_t a = static_cast<uint64_t>(std::abs(*va));
                uint64_t b = static_cast<uint64_t>(std::abs(*vb));
                while (b) { uint64_t t = b; b = a % b; a = t; }
                return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(a));
            }
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        // Use absolute values via llvm.abs intrinsic (single instruction on x86:
        // neg + cmov, vs 3-instruction icmp+neg+select sequence).
        llvm::Function* absFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::abs, {getDefaultType()});
        a = builder->CreateCall(absFn, {a, builder->getTrue()}, "gcd.aabs");
        b = builder->CreateCall(absFn, {b, builder->getTrue()}, "gcd.babs");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);

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
        // ── Compile-time fold: to_string(integer_const) ─────────────
        if (auto iv = tryFoldInt(expr->arguments[0].get())) {
            std::string s = std::to_string(*iv);
            llvm::GlobalVariable* gv = internString(s);
            stringReturningFunctions_.insert("to_string");
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                gv->getValueType(), gv,
                llvm::ArrayRef<llvm::Constant*>{
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        const bool isFloat = val->getType()->isDoubleTy();
        if (!isFloat)
            val = toDefaultType(val);
        if (isFloat) {
            // Float: use a 32-byte buffer and %g format to preserve decimal places.
            llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 32);
            llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "tostr.buf");
            llvm::cast<llvm::CallInst>(buf)->addRetAttr(
                llvm::Attribute::getWithDereferenceableBytes(*context, 32));
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
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 21));
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
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        // memchr(strPtr, ch, strLen)
        llvm::Value* chTrunc = builder->CreateTrunc(chArg, llvm::Type::getInt32Ty(*context), "strfind.ch32");
        llvm::Value* found = builder->CreateCall(getOrDeclareMemchr(), {strPtr, chTrunc, strLen}, "strfind.found");
        // If memchr returns null, return -1; otherwise return (found - strPtr)
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNull = builder->CreateICmpEQ(found, nullPtr, "strfind.isnull");
        llvm::Value* offset = builder->CreatePtrDiff(llvm::Type::getInt8Ty(*context), found, strPtr, "strfind.offset");
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        return builder->CreateSelect(isNull, negOne, offset, "strfind.result");
    }

    // -----------------------------------------------------------------------
    // Math built-ins: floor, ceil, round
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::FLOOR) {
        validateArgCount(expr, "floor", 1);
        // Integer input: floor is identity.
        if (auto v = tryFoldInt(expr->arguments[0].get()))
            return llvm::ConstantInt::get(getDefaultType(), *v);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        const bool inputIsDouble = arg->getType()->isDoubleTy();
        llvm::Value* fval = ensureFloat(arg);
        llvm::Function* floorIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::floor, {getFloatType()});
        llvm::Value* result = builder->CreateCall(floorIntrinsic, {fval}, "floor.result");
        if (inputIsDouble) return result;
        return builder->CreateFPToSI(result, getDefaultType(), "floor.int");
    }

    if (bid == BuiltinId::CEIL) {
        validateArgCount(expr, "ceil", 1);
        // Integer input: ceil is identity.
        if (auto v = tryFoldInt(expr->arguments[0].get()))
            return llvm::ConstantInt::get(getDefaultType(), *v);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        const bool inputIsDouble = arg->getType()->isDoubleTy();
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
        // Integer input: round is identity.
        if (auto v = tryFoldInt(expr->arguments[0].get()))
            return llvm::ConstantInt::get(getDefaultType(), *v);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        const bool inputIsDouble = arg->getType()->isDoubleTy();
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
        const llvm::FunctionCallee tanFn = module->getOrInsertFunction("tan", ft);
        return builder->CreateCall(tanFn, {fval}, "tan.result");
    }

    if (bid == BuiltinId::ASIN) {
        validateArgCount(expr, "asin", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
        const llvm::FunctionCallee asinFn = module->getOrInsertFunction("asin", ft);
        return builder->CreateCall(asinFn, {fval}, "asin.result");
    }

    if (bid == BuiltinId::ACOS) {
        validateArgCount(expr, "acos", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
        const llvm::FunctionCallee acosFn = module->getOrInsertFunction("acos", ft);
        return builder->CreateCall(acosFn, {fval}, "acos.result");
    }

    if (bid == BuiltinId::ATAN) {
        validateArgCount(expr, "atan", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
        const llvm::FunctionCallee atanFn = module->getOrInsertFunction("atan", ft);
        return builder->CreateCall(atanFn, {fval}, "atan.result");
    }

    if (bid == BuiltinId::ATAN2) {
        validateArgCount(expr, "atan2", 2);
        llvm::Value* y = generateExpression(expr->arguments[0].get());
        llvm::Value* x = generateExpression(expr->arguments[1].get());
        llvm::Value* fy = ensureFloat(y);
        llvm::Value* fx = ensureFloat(x);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType(), getFloatType()}, false);
        const llvm::FunctionCallee atan2Fn = module->getOrInsertFunction("atan2", ft);
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
        const llvm::FunctionCallee cbrtFn = module->getOrInsertFunction("cbrt", ft);
        return builder->CreateCall(cbrtFn, {fval}, "cbrt.result");
    }

    if (bid == BuiltinId::HYPOT) {
        validateArgCount(expr, "hypot", 2);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        llvm::Value* y = generateExpression(expr->arguments[1].get());
        llvm::Value* fx = ensureFloat(x);
        llvm::Value* fy = ensureFloat(y);
        llvm::FunctionType* ft = llvm::FunctionType::get(getFloatType(), {getFloatType(), getFloatType()}, false);
        const llvm::FunctionCallee hypotFn = module->getOrInsertFunction("hypot", ft);
        return builder->CreateCall(hypotFn, {fx, fy}, "hypot.result");
    }

    // -----------------------------------------------------------------------
    // Type conversion built-ins: to_int, to_float
    // -----------------------------------------------------------------------

    if (bid == BuiltinId::TO_INT) {
        validateArgCount(expr, "to_int", 1);

        // ── Compile-time to_int folding ─────────────────────────────
        // Handles: const variables, enum members, string constants, int
        // constants — anything tryFoldStr/tryFoldInt can resolve.
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            try {
                const int64_t parsed = std::stoll(*sv);
                return llvm::ConstantInt::get(getDefaultType(), parsed);
            } catch (...) {
                // Fall through to runtime parsing if conversion fails.
            }
        }
        if (auto iv = tryFoldInt(expr->arguments[0].get())) {
            return llvm::ConstantInt::get(getDefaultType(), *iv);
        }

        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        if (arg->getType()->isDoubleTy()) {
            return builder->CreateFPToSI(arg, getDefaultType(), "toint.ftoi");
        }
        // If the argument is a string, parse it with strtoll.
        if (isStringExpr(expr->arguments[0].get())) {
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

        // ── Compile-time to_float folding ───────────────────────────
        // Handles literals, const string variables, and integer constants.
        // tryFoldStr handles string literals + const string variables;
        // tryFoldInt handles integer literals + const integer variables.
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            try {
                const double parsed = std::stod(*sv);
                optStats_.constFolded++;
                return llvm::ConstantFP::get(getFloatType(), parsed);
            } catch (...) {
                // Fall through to runtime parsing if conversion fails.
            }
        }
        if (auto iv = tryFoldInt(expr->arguments[0].get())) {
            optStats_.constFolded++;
            return llvm::ConstantFP::get(getFloatType(), static_cast<double>(*iv));
        }
        if (auto* lit = dynamic_cast<LiteralExpr*>(expr->arguments[0].get())) {
            if (lit->literalType == LiteralExpr::LiteralType::FLOAT) {
                optStats_.constFolded++;
                return llvm::ConstantFP::get(getFloatType(), lit->floatValue);
            }
        }

        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // If the argument is a string, parse it with strtod.
        if (isStringExpr(expr->arguments[0].get())) {
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
        if (auto strConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto startConst = tryFoldInt(expr->arguments[1].get())) {
                if (auto lenConst = tryFoldInt(expr->arguments[2].get())) {
                    const auto& s = *strConst;
                    int64_t slen = static_cast<int64_t>(s.size());
                    int64_t startVal = *startConst, lenVal = *lenConst;
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
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
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
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));
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
        // NOTE: Cannot fold str_upper to an interned global — callers may
        // mutate the returned string via s[i]=v.  evalConstBuiltin handles
        // the abstract fold for pure-evaluation contexts (tryConstEvalFull).
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "upper.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "upper.len");
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* allocSize = builder->CreateAdd(strLen, llvm::ConstantInt::get(getDefaultType(), 1), "upper.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "upper.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));
        builder->CreateCall(getOrDeclareStrcpy(), {buf, strPtr});
        // Loop: for i = 0; i < strLen; i++ { buf[i] = toupper(buf[i]); }
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* upperBuf = buf;
        emitCountingLoop("upper", strLen, zero, 4,
            [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
                llvm::Value* charPtr = builder->CreateInBoundsGEP(
                    llvm::Type::getInt8Ty(*context), upperBuf, idx, "upper.charptr");
                auto* chLoad = builder->CreateLoad(
                    llvm::Type::getInt8Ty(*context), charPtr, "upper.ch");
                chLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
                llvm::Value* ch32 = builder->CreateZExt(
                    chLoad, llvm::Type::getInt32Ty(*context), "upper.ch32");
                llvm::Value* upper = builder->CreateCall(
                    getOrDeclareToupper(), {ch32}, "upper.toupper");
                llvm::Value* upper8 = builder->CreateTrunc(
                    upper, llvm::Type::getInt8Ty(*context), "upper.trunc");
                auto* upperStore = builder->CreateStore(upper8, charPtr);
                upperStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
                llvm::Value* nextIdx = builder->CreateAdd(
                    idx, one, "upper.next", /*HasNUW=*/true, /*HasNSW=*/true);
                idx->addIncoming(nextIdx, builder->GetInsertBlock());
                attachLoopMetadataVec(
                    llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
            });
        stringReturningFunctions_.insert("str_upper");
        return buf;
    }

    if (bid == BuiltinId::STR_LOWER) {
        validateArgCount(expr, "str_lower", 1);
        // NOTE: Cannot fold str_lower to an interned global — callers may
        // mutate the returned string via s[i]=v.  evalConstBuiltin handles
        // the abstract fold for pure-evaluation contexts (tryConstEvalFull).
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "lower.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "lower.len");
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* allocSize = builder->CreateAdd(strLen, llvm::ConstantInt::get(getDefaultType(), 1), "lower.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "lower.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));
        builder->CreateCall(getOrDeclareStrcpy(), {buf, strPtr});
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* lowerBuf = buf;
        emitCountingLoop("lower", strLen, zero, 4,
            [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
                llvm::Value* charPtr = builder->CreateInBoundsGEP(
                    llvm::Type::getInt8Ty(*context), lowerBuf, idx, "lower.charptr");
                auto* chLoad = builder->CreateLoad(
                    llvm::Type::getInt8Ty(*context), charPtr, "lower.ch");
                chLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
                llvm::Value* ch32 = builder->CreateZExt(
                    chLoad, llvm::Type::getInt32Ty(*context), "lower.ch32");
                llvm::Value* lower = builder->CreateCall(
                    getOrDeclareTolower(), {ch32}, "lower.tolower");
                llvm::Value* lower8 = builder->CreateTrunc(
                    lower, llvm::Type::getInt8Ty(*context), "lower.trunc");
                auto* lowerStore = builder->CreateStore(lower8, charPtr);
                lowerStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
                llvm::Value* nextIdx = builder->CreateAdd(
                    idx, one, "lower.next", /*HasNUW=*/true, /*HasNSW=*/true);
                idx->addIncoming(nextIdx, builder->GetInsertBlock());
                attachLoopMetadataVec(
                    llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
            });
        stringReturningFunctions_.insert("str_lower");
        return buf;
    }

    if (bid == BuiltinId::STR_CONTAINS) {
        validateArgCount(expr, "str_contains", 2);

        // ── Compile-time str_contains folding ───────────────────────
        // When both arguments are compile-time string constants, fold at compile time.
        if (auto hayConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto needleConst = tryFoldStr(expr->arguments[1].get())) {
                const bool found = hayConst->find(*needleConst) != std::string::npos;
                return llvm::ConstantInt::get(getDefaultType(), found ? 1 : 0);
            }
        }

        // Detect single-character needle → use memchr (SIMD-optimized)
        // instead of strstr (byte-by-byte scan).
        Expression* needleExpr = expr->arguments[1].get();
        bool isSingleChar = false;
        char singleCharVal = 0;
        if (auto needleConst = tryFoldStr(needleExpr)) {
            if (needleConst->size() == 1) {
                isSingleChar = true;
                singleCharVal = (*needleConst)[0];
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
            nonNegValues_.insert(len);
            if (optimizationLevel >= OptimizationLevel::O1)
                llvm::cast<llvm::Instruction>(len)->setMetadata(
                    llvm::LLVMContext::MD_range, arrayLenRangeMD_);
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
        auto* containsResult = builder->CreateZExt(isNotNull, getDefaultType(), "contains.result");
        nonNegValues_.insert(containsResult);
        return containsResult;
    }

    if (bid == BuiltinId::STR_INDEX_OF) {
        validateArgCount(expr, "str_index_of", 2);

        // ── Compile-time str_index_of folding ───────────────────────
        if (auto hayConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto needleConst = tryFoldStr(expr->arguments[1].get())) {
                auto pos = hayConst->find(*needleConst);
                int64_t result = (pos == std::string::npos) ? -1 : static_cast<int64_t>(pos);
                return llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(result));
            }
        }

        // Detect single-character needle → use memchr (SIMD-optimized)
        // instead of strstr (byte-by-byte scan).
        Expression* needleExpr = expr->arguments[1].get();
        bool isSingleChar = false;
        char singleCharVal = 0;
        if (auto needleConst = tryFoldStr(needleExpr)) {
            if (needleConst->size() == 1) {
                isSingleChar = true;
                singleCharVal = (*needleConst)[0];
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
            nonNegValues_.insert(len);
            if (optimizationLevel >= OptimizationLevel::O1)
                llvm::cast<llvm::Instruction>(len)->setMetadata(
                    llvm::LLVMContext::MD_range, arrayLenRangeMD_);
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
        llvm::Value* offset = builder->CreatePtrDiff(llvm::Type::getInt8Ty(*context), result, haystackPtr, "indexof.offset");
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        return builder->CreateSelect(isNull, negOne, offset, "indexof.result");
    }

    if (bid == BuiltinId::STR_REPLACE) {
        validateArgCount(expr, "str_replace", 3);
        // NOTE: Cannot constant-fold str_replace — callers may mutate the returned
        // heap-allocated string via s[i]=v, and a read-only global would segfault.
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
        nonNegValues_.insert(oldLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(oldLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* newLen  = builder->CreateCall(getOrDeclareStrlen(), {newPtr}, "replace.newlen");
        nonNegValues_.insert(newLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(newLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* strLen  = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "replace.strlen");
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);

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
        llvm::cast<llvm::CallInst>(copyBuf0)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));

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
        // lenDiff is signed (newLen may be < oldLen), but the final resultLen
        // is always non-negative because totalCount * lenDiff only shrinks by
        // at most strLen.  nsw on the mul/add lets SCEV prove this.
        llvm::Value* lenDiff   = builder->CreateSub(newLen, oldLen, "replace.lendiff", /*HasNUW=*/false, /*HasNSW=*/true);
        llvm::Value* extraLen  = builder->CreateMul(totalCount, lenDiff, "replace.extralen", /*HasNUW=*/false, /*HasNSW=*/true);
        llvm::Value* resultLen = builder->CreateAdd(strLen, extraLen, "replace.resultlen", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* resultSize= builder->CreateAdd(resultLen, one, "replace.resultsize", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* resultBuf = builder->CreateCall(getOrDeclareMalloc(), {resultSize}, "replace.resultbuf");
        llvm::cast<llvm::CallInst>(resultBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));

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
        llvm::Value* consumed = builder->CreatePtrDiff(llvm::Type::getInt8Ty(*context), bSrc, strPtr, "replace.consumed");
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
        // NOTE: Cannot fold str_trim to an interned global — callers may
        // mutate the returned string via s[i]=v.  evalConstBuiltin handles
        // the abstract fold for pure-evaluation contexts (tryConstEvalFull).
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "trim.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "trim.len");
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);

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
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(startLoopBB)));
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
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(endLoopBB)));

        builder->SetInsertPoint(endDoneBB);
        llvm::PHINode* trimEnd = builder->CreatePHI(getDefaultType(), 2, "trim.end");
        trimEnd->addIncoming(endIdx, endLoopBB); // empty result
        trimEnd->addIncoming(endIdx, endBodyBB); // found non-space

        // Build trimmed string
        llvm::Value* trimLen = builder->CreateSub(trimEnd, trimStart, "trim.len2");
        llvm::Value* trimAlloc = builder->CreateAdd(trimLen, llvm::ConstantInt::get(getDefaultType(), 1), "trim.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* trimBuf = builder->CreateCall(getOrDeclareMalloc(), {trimAlloc}, "trim.buf");
        llvm::cast<llvm::CallInst>(trimBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));
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
        // When both arguments are compile-time string constants, fold at compile time.
        if (auto strConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto prefixConst = tryFoldStr(expr->arguments[1].get())) {
                bool result = strConst->size() >= prefixConst->size() &&
                              strConst->compare(0, prefixConst->size(), *prefixConst) == 0;
                return llvm::ConstantInt::get(getDefaultType(), result ? 1 : 0);
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
        //
        // Optimization: when the prefix is a compile-time string literal,
        // use the known constant length instead of calling strlen at runtime.
        llvm::Value* prefixLen;
        if (auto prefixLit = tryFoldStr(expr->arguments[1].get())) {
            // Constant-fold strlen(prefix) — no runtime call needed.
            prefixLen = llvm::ConstantInt::get(getDefaultType(),
                                               static_cast<int64_t>(prefixLit->size()));
        } else {
            prefixLen = builder->CreateCall(getOrDeclareStrlen(), {prefixPtr}, "startswith.plen");
            nonNegValues_.insert(prefixLen);
            if (optimizationLevel >= OptimizationLevel::O1)
                llvm::cast<llvm::Instruction>(prefixLen)->setMetadata(
                    llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        }
        llvm::Value* cmpResult = builder->CreateCall(getOrDeclareStrncmp(),
            {strPtr, prefixPtr, prefixLen}, "startswith.cmp");
        llvm::Value* isEqual = builder->CreateICmpEQ(cmpResult, builder->getInt32(0), "startswith.eq");
        auto* swResult = builder->CreateZExt(isEqual, getDefaultType(), "startswith.result");
        nonNegValues_.insert(swResult);
        return swResult;
    }

    if (bid == BuiltinId::STR_ENDS_WITH) {
        validateArgCount(expr, "str_ends_with", 2);

        // ── Compile-time str_ends_with folding ──────────────────────
        // When both arguments are compile-time string constants, fold at compile time.
        if (auto strConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto suffixConst = tryFoldStr(expr->arguments[1].get())) {
                const auto& s = *strConst;
                const auto& suffix = *suffixConst;
                bool result = s.size() >= suffix.size() &&
                              s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
                return llvm::ConstantInt::get(getDefaultType(), result ? 1 : 0);
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
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        // Optimization: when the suffix is a compile-time string literal,
        // constant-fold strlen(suffix) to avoid the runtime call.
        llvm::Value* sufLen;
        if (auto suffixLit = tryFoldStr(expr->arguments[1].get())) {
            sufLen = llvm::ConstantInt::get(getDefaultType(),
                                            static_cast<int64_t>(suffixLit->size()));
        } else {
            sufLen = builder->CreateCall(getOrDeclareStrlen(), {suffixPtr}, "endswith.suflen");
            nonNegValues_.insert(sufLen);
            if (optimizationLevel >= OptimizationLevel::O1)
                llvm::cast<llvm::Instruction>(sufLen)->setMetadata(
                    llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        }
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
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::STR_REPEAT) {
        validateArgCount(expr, "str_repeat", 2);

        // ── Compile-time str_repeat folding ─────────────────────────
        // When both arguments are compile-time constants and the result
        // is reasonably small, fold at compile time.
        if (auto strConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto countConst = tryFoldInt(expr->arguments[1].get())) {
                int64_t count = *countConst;
                // Only fold for small results (≤ 256 bytes) to avoid
                // bloating the data section.
                if (count >= 0 && count * static_cast<int64_t>(strConst->size()) <= 256) {
                    std::string result;
                    result.reserve(static_cast<size_t>(count) * strConst->size());
                    for (int64_t i = 0; i < count; ++i) result += *strConst;
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
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* totalLen = builder->CreateMul(strLen, countArg, "repeat.total", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* allocSize =
            builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "repeat.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "repeat.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));
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

        // NOTE: Cannot fold str_reverse to an interned global — callers may
        // mutate the returned string via s[i]=v.  evalConstBuiltin handles
        // the abstract fold for pure-evaluation contexts (tryConstEvalFull).

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strrev.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strrev.len");
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* allocSize =
            builder->CreateAdd(strLen, llvm::ConstantInt::get(getDefaultType(), 1), "strrev.alloc", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "strrev.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* strrevBuf = buf;
        llvm::Value* srStrLen = strLen;
        llvm::Value* srStrPtr = strPtr;
        emitCountingLoop("strrev", strLen, zero, 4,
            [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
                llvm::Value* revIdx = builder->CreateSub(
                    builder->CreateSub(srStrLen, one, "strrev.lenm1", /*HasNUW=*/true, /*HasNSW=*/true),
                    idx, "strrev.revidx", /*HasNUW=*/true, /*HasNSW=*/true);
                llvm::Value* srcPtr2 = builder->CreateInBoundsGEP(
                    llvm::Type::getInt8Ty(*context), srStrPtr, revIdx, "strrev.srcptr");
                auto* revLoad = builder->CreateLoad(
                    llvm::Type::getInt8Ty(*context), srcPtr2, "strrev.ch");
                revLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
                llvm::Value* dstPtr = builder->CreateInBoundsGEP(
                    llvm::Type::getInt8Ty(*context), strrevBuf, idx, "strrev.dstptr");
                auto* revStore = builder->CreateStore(revLoad, dstPtr);
                revStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
                llvm::Value* nextIdx = builder->CreateAdd(
                    idx, one, "strrev.next", /*HasNUW=*/true, /*HasNSW=*/true);
                idx->addIncoming(nextIdx, builder->GetInsertBlock());
                attachLoopMetadataVec(
                    llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
            });
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
                llvm::Value* pushLenLoad = emitLoadArrayLen(arrPtr, "push.oldlen");
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
        emitStoreArrayLen(newLen, newBuf);
        // Store new value at index oldLen + 1 (after header)
        llvm::Value* newElemIdx =
            builder->CreateAdd(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "push.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* newElemPtr = builder->CreateInBoundsGEP(getDefaultType(), newBuf, newElemIdx, "push.elemptr");
        emitStoreArrayElem(valArg, newElemPtr);
        // Return new array pointer as i64
        return builder->CreatePtrToInt(newBuf, getDefaultType(), "push.result");
    }

    if (bid == BuiltinId::POP) {
        validateArgCount(expr, "pop", 1);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "pop.arrptr");
                llvm::Value* popLenLoad = emitLoadArrayLen(arrPtr, "pop.oldlen");
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
        {
            std::string msg = expr->line > 0
                ? std::string("Runtime error: pop from empty array at line ") + std::to_string(expr->line) + "\n"
                : "Runtime error: pop from empty array\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "pop_empty_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Return the last element
        llvm::Value* lastIdx =
            builder->CreateAdd(builder->CreateSub(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "pop.lastoff", /*HasNUW=*/true, /*HasNSW=*/true),
                               llvm::ConstantInt::get(getDefaultType(), 1), "pop.lastidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* lastPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, lastIdx, "pop.lastptr");
        llvm::Value* lastVal = emitLoadArrayElem(lastPtr, "pop.lastval");
        // Decrease length in-place
        llvm::Value* newLen = builder->CreateSub(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "pop.newlen", /*HasNUW=*/true, /*HasNSW=*/true);
        auto* popLenSt = builder->CreateAlignedStore(newLen, arrPtr, llvm::MaybeAlign(8));
        popLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        return lastVal;
    }

    if (bid == BuiltinId::INDEX_OF) {
        validateArgCount(expr, "index_of", 2);
        // Constant-fold index_of([c0,...], val) when the array and value are known.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                if (auto vv = tryFoldInt(expr->arguments[1].get())) {
                    for (int64_t i = 0; i < static_cast<int64_t>(cv->arrVal.size()); ++i) {
                        if (cv->arrVal[static_cast<size_t>(i)].kind == ConstValue::Kind::Integer &&
                            cv->arrVal[static_cast<size_t>(i)].intVal == *vv) {
                            optStats_.constFolded++;
                            return llvm::ConstantInt::get(getDefaultType(), i);
                        }
                    }
                    optStats_.constFolded++;
                    return llvm::ConstantInt::get(getDefaultType(), -1, /*isSigned=*/true);
                }
            }
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "indexof.arrptr");
                llvm::Value* idxofLenLoad = emitLoadArrayLen(arrPtr, "indexof.len");
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
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "indexof.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
        // Constant-fold array_contains([c0,...], val) when array and value are known.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                if (auto vv = tryFoldInt(expr->arguments[1].get())) {
                    for (const auto& elem : cv->arrVal) {
                        if (elem.kind == ConstValue::Kind::Integer && elem.intVal == *vv) {
                            optStats_.constFolded++;
                            return llvm::ConstantInt::get(getDefaultType(), 1);
                        }
                    }
                    optStats_.constFolded++;
                    return llvm::ConstantInt::get(getDefaultType(), 0);
                }
            }
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "contains.arrptr");
                llvm::Value* containsLenLoad = emitLoadArrayLen(arrPtr, "contains.len");
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
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "contains.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
        const bool sortStrings = isStringArrayExpr(expr->arguments[0].get());
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "sort.arrptr");
                llvm::Value* sortLenLoad = emitLoadArrayLen(arrPtr, "sort.len");
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
            llvm::cast<llvm::Instruction>(a)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            llvm::cast<llvm::Instruction>(b)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
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
            llvm::cast<llvm::Instruction>(aI64)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            llvm::cast<llvm::Instruction>(bI64)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
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
            llvm::cast<llvm::CallInst>(buf)->addRetAttr(
                llvm::Attribute::getWithDereferenceableBytes(*context, 8));
            // Store length in header (calloc zeroed it; overwrite with actual size)
            emitStoreArrayLen(sizeArg, buf);
        } else {
            llvm::Value* bytes = builder->CreateMul(slots, eight, "fill.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
            buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "fill.buf");
            llvm::cast<llvm::CallInst>(buf)->addRetAttr(
                llvm::Attribute::getWithDereferenceableBytes(*context, 8));
            emitStoreArrayLen(sizeArg, buf);
            // Fill loop
            llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
            // Capture buf/valArg for the lambda
            llvm::Value* fillBuf = buf;
            llvm::Value* fillVal = valArg;
            emitCountingLoop("fill", sizeArg, zero, 4,
                [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
                    llvm::Value* elemIdx = builder->CreateAdd(idx, one, "fill.elemidx",
                                                              /*HasNUW=*/true, /*HasNSW=*/true);
                    llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(),
                                                                       fillBuf, elemIdx, "fill.elemptr");
                    emitStoreArrayElem(fillVal, elemPtr);
                    llvm::Value* nextIdx = builder->CreateAdd(idx, one, "fill.next",
                                                              /*HasNUW=*/true, /*HasNSW=*/true);
                    idx->addIncoming(nextIdx, builder->GetInsertBlock());
                    attachLoopMetadataVec(
                        llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
                });
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
                llvm::Value* acatLen1Load = emitLoadArrayLen(arr1Ptr, "aconcat.len1");
        llvm::Value* len1 = acatLen1Load;
                llvm::Value* acatLen2Load = emitLoadArrayLen(arr2Ptr, "aconcat.len2");
        llvm::Value* len2 = acatLen2Load;
        llvm::Value* totalLen = builder->CreateAdd(len1, len2, "aconcat.total", /*HasNUW=*/true, /*HasNSW=*/true);
        // Allocate: (totalLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "aconcat.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, llvm::ConstantInt::get(getDefaultType(), 8), "aconcat.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "aconcat.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        emitStoreArrayLen(totalLen, buf);
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
                llvm::Value* sliceLenLoad = emitLoadArrayLen(arrPtr, "slice.arrlen");
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
        llvm::Value* buf = emitAllocArray(sliceLen, "slice");
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
                llvm::Value* acopyLenLoad = emitLoadArrayLen(arrPtr, "acopy.len");
        llvm::Value* arrLen = acopyLenLoad;
        // Allocate: (length + 1) * 8 bytes
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "acopy.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "acopy.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "acopy.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
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
                llvm::Value* aremLenLoad = emitLoadArrayLen(arrPtr, "aremove.len");
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
        {
            std::string msg = expr->line > 0
                ? std::string("Runtime error: array_remove index out of bounds at line ") + std::to_string(expr->line) + "\n"
                : "Runtime error: array_remove index out of bounds\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "aremove_oob_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();
        // In-bounds: save removed value, shift elements left, decrement length
        builder->SetInsertPoint(okBB);
        llvm::Value* elemOffset = builder->CreateAdd(idxArg, one, "aremove.elemoff", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, elemOffset, "aremove.elemptr");
        llvm::Value* removedVal = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "aremove.removed");
        // TBAA: removed value is an array element, never aliases the length header.
        llvm::cast<llvm::LoadInst>(removedVal)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::LoadInst>(removedVal)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
        emitStoreArrayLen(newLen, arrPtr);
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
        const std::string fnName = fnNameLit->stringValue;
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
                llvm::Value* amapLenLoad = emitLoadArrayLen(arrPtr, "amap.len");
        llvm::Value* arrLen = amapLenLoad;

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Allocate result array: (arrLen + 1) * 8
        llvm::Value* buf = emitAllocArray(arrLen, "amap");

        // Loop: for each element, call mapFn and store result
        llvm::Value* amapBuf = buf;
        llvm::Value* amapArrPtr = arrPtr;
        llvm::Value* amapArrLen = arrLen;
        emitCountingLoop("amap", arrLen, zero, 4,
            [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
                llvm::Value* elemIdx = builder->CreateAdd(
                    idx, one, "amap.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
                llvm::Value* srcPtr = builder->CreateInBoundsGEP(
                    getDefaultType(), amapArrPtr, elemIdx, "amap.srcptr");
                llvm::Value* elem = emitLoadArrayElem(srcPtr, "amap.elem");
                if (optimizationLevel >= OptimizationLevel::O1)
                    llvm::cast<llvm::Instruction>(elem)->setMetadata(
                        llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
                llvm::Value* mapped = builder->CreateCall(mapFn, {elem}, "amap.mapped");
                mapped = toDefaultType(mapped);
                llvm::Value* dstPtr = builder->CreateInBoundsGEP(
                    getDefaultType(), amapBuf, elemIdx, "amap.dstptr");
                emitStoreArrayElem(mapped, dstPtr);
                llvm::Value* nextIdx = builder->CreateAdd(
                    idx, one, "amap.next", /*HasNUW=*/true, /*HasNSW=*/true);
                idx->addIncoming(nextIdx, builder->GetInsertBlock());
                attachLoopMetadata(
                    llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
            });
        (void)amapArrLen;
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
        const std::string fnName = fnNameLit->stringValue;
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
                llvm::Value* afiltLenLoad = emitLoadArrayLen(arrPtr, "afilt.len");
        llvm::Value* arrLen = afiltLenLoad;

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Allocate result array with max possible size: (arrLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "afilt.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "afilt.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "afilt.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        // Initialize length to 0 (will be updated as we add elements)
        emitStoreArrayLen(zero, buf);

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
                llvm::Value* elem = emitLoadArrayElem(srcPtr, "afilt.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
        emitStoreArrayLen(outIdx, buf);
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
                llvm::Value* aredLenLoad = emitLoadArrayLen(arrPtr, "areduce.len");
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
                llvm::Value* elem = emitLoadArrayElem(srcPtr, "areduce.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
        // Constant-fold array_min([c0,...]) when the array is a compile-time literal.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                if (cv->arrVal.empty()) {
                    optStats_.constFolded++;
                    return llvm::ConstantInt::get(getDefaultType(), 0);
                }
                bool allInt = true;
                int64_t minVal = INT64_MAX;
                for (const auto& elem : cv->arrVal) {
                    if (elem.kind != ConstValue::Kind::Integer) { allInt = false; break; }
                    if (elem.intVal < minVal) minVal = elem.intVal;
                }
                if (allInt) {
                    optStats_.constFolded++;
                    return llvm::ConstantInt::get(getDefaultType(), minVal);
                }
            }
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "amin.arrptr");
                llvm::Value* aminLenLoad = emitLoadArrayLen(arrPtr, "amin.len");
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
                llvm::Value* firstElem = emitLoadArrayElem(firstPtr, "amin.first");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(firstElem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "amin.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
        // Use llvm.smin intrinsic instead of icmp+select.  The intrinsic:
        //  1. Generates a single cmov/SIMD-min instruction rather than a branch
        //  2. Is recognized by the vectorizer as a min-reduction, enabling
        //     SIMD vpcmpq+vpminsd (AVX2) or vpminq (AVX-512) instructions
        llvm::Function* sminFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smin, {getDefaultType()});
        llvm::Value* newMin = builder->CreateCall(sminFn, {elem, curMin}, "amin.newmin");
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        // Eliminates a redundant add and provides SCEV with tight nsw+nuw flags.
        curMin->addIncoming(newMin, bodyBB);
        idx->addIncoming(offset, bodyBB);
        attachLoopMetadataVec(
            llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

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
        // Constant-fold array_max([c0,...]) when the array is a compile-time literal.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                if (cv->arrVal.empty()) {
                    optStats_.constFolded++;
                    return llvm::ConstantInt::get(getDefaultType(), 0);
                }
                bool allInt = true;
                int64_t maxVal = INT64_MIN;
                for (const auto& elem : cv->arrVal) {
                    if (elem.kind != ConstValue::Kind::Integer) { allInt = false; break; }
                    if (elem.intVal > maxVal) maxVal = elem.intVal;
                }
                if (allInt) {
                    optStats_.constFolded++;
                    auto* ci = llvm::ConstantInt::get(getDefaultType(), maxVal);
                    if (maxVal >= 0) nonNegValues_.insert(ci);
                    return ci;
                }
            }
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "amax.arrptr");
                llvm::Value* amaxLenLoad = emitLoadArrayLen(arrPtr, "amax.len");
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
                llvm::Value* firstElem = emitLoadArrayElem(firstPtr, "amax.first");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(firstElem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "amax.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
        attachLoopMetadataVec(
            llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

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
                llvm::Value* aanyLenLoad = emitLoadArrayLen(arrPtr, "aany.len");
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
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "aany.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
                llvm::Value* aeveryLenLoad = emitLoadArrayLen(arrPtr, "aevery.len");
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
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "aevery.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
        // Constant-fold array_find([c0,...], val) when array and value are known.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                if (auto vv = tryFoldInt(expr->arguments[1].get())) {
                    for (int64_t i = 0; i < static_cast<int64_t>(cv->arrVal.size()); ++i) {
                        const auto& elem = cv->arrVal[static_cast<size_t>(i)];
                        if (elem.kind == ConstValue::Kind::Integer && elem.intVal == *vv) {
                            optStats_.constFolded++;
                            return llvm::ConstantInt::get(getDefaultType(), i);
                        }
                    }
                    optStats_.constFolded++;
                    return llvm::ConstantInt::get(getDefaultType(), -1, true);
                }
            }
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* target = generateExpression(expr->arguments[1].get());
        target = toDefaultType(target);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "afind.arrptr");
                llvm::Value* afindLenLoad = emitLoadArrayLen(arrPtr, "afind.len");
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
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "afind.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
                llvm::Value* acntLenLoad = emitLoadArrayLen(arrPtr, "acnt.len");
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
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "acnt.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
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
        {
            attachLoopMetadataVec(
                llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        }

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
        } else if (isStringExpr(argExpr)) {
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
        } else if (isStringExpr(argExpr)) {
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
        // Constant-fold str_to_int("literal") at compile time.
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            try {
                int64_t val = static_cast<int64_t>(std::stoll(*sv));
                optStats_.constFolded++;
                auto* ci = llvm::ConstantInt::get(getDefaultType(), val);
                if (val >= 0) nonNegValues_.insert(ci);
                return ci;
            } catch (...) {} // NOLINT(bugprone-empty-catch)
        }
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
        // Constant-fold str_to_float("literal") at compile time.
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            try {
                double val = std::stod(*sv);
                optStats_.constFolded++;
                return llvm::ConstantFP::get(getFloatType(), val);
            } catch (...) {} // NOLINT(bugprone-empty-catch)
        }
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
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
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
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(countLoopBB)));

        builder->SetInsertPoint(countDoneBB);
        // cnt now holds the number of parts

        // Allocate result array: (cnt + 1) * 8 bytes (length + elements)
        llvm::Value* slots = builder->CreateAdd(cnt, one, "split.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "split.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* arrBuf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "split.arr");
        llvm::cast<llvm::CallInst>(arrBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
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
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(splitLoopBB)));

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
        nonNegValues_.insert(strLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(strLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);

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
        {
            attachLoopMetadataVec(
                llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        }

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

                llvm::Value* joinLenLoad = emitLoadArrayLen(arrPtr, "join.len");
        llvm::Value* arrLen = joinLenLoad;
        llvm::Value* delimLen = builder->CreateCall(getOrDeclareStrlen(), {delimPtr}, "join.delimlen");
        nonNegValues_.insert(delimLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(delimLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);

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
                llvm::Value* elemInt = emitLoadArrayElem(lslotPtr, "join.elemint");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elemInt)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
        llvm::Value* elemPtr = builder->CreateIntToPtr(elemInt, ptrTy, "join.elemptr");
        llvm::Value* elemLen = builder->CreateCall(getOrDeclareStrlen(), {elemPtr}, "join.elemlen");
        nonNegValues_.insert(elemLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elemLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* newTotal = builder->CreateAdd(totalLen, elemLen, "join.newtot", /*HasNUW=*/true, /*HasNSW=*/true);
        // Add delimiter length for all elements except the first
        llvm::Value* isFirst = builder->CreateICmpEQ(li, zero, "join.isfirst");
        llvm::Value* delimAdd = builder->CreateSelect(isFirst, zero, delimLen, "join.delimadd");
        llvm::Value* newTotal2 = builder->CreateAdd(newTotal, delimAdd, "join.newtot2", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextLi = builder->CreateAdd(li, one, "join.nextli", /*HasNUW=*/true, /*HasNSW=*/true);
        li->addIncoming(nextLi, lenBodyBB);
        totalLen->addIncoming(newTotal2, lenBodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(lenLoopBB)));

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
                llvm::Value* celemInt = emitLoadArrayElem(cslotPtr, "join.celemint");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(celemInt)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
        llvm::Value* celemPtr = builder->CreateIntToPtr(celemInt, ptrTy, "join.celemptr");
        llvm::Value* celemLen = builder->CreateCall(getOrDeclareStrlen(), {celemPtr}, "join.celemlen");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(celemLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* elemDst = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, afterDelim, "join.elemdst");
        builder->CreateCall(getOrDeclareMemcpy(), {elemDst, celemPtr, celemLen});
        llvm::Value* afterElem = builder->CreateAdd(afterDelim, celemLen, "join.afterelem", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextCi = builder->CreateAdd(ci, one, "join.nextci", /*HasNUW=*/true, /*HasNSW=*/true);
        ci->addIncoming(nextCi, catBodyBB);
        writePos->addIncoming(afterElem, catBodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(catLoopBB)));

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

        // ── Compile-time str_count folding ──────────────────────────
        if (auto hayConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto needleConst = tryFoldStr(expr->arguments[1].get())) {
                if (!needleConst->empty()) {
                    int64_t count = 0;
                    size_t pos = 0;
                    while ((pos = hayConst->find(*needleConst, pos)) != std::string::npos) {
                        ++count;
                        pos += needleConst->size();
                    }
                    return llvm::ConstantInt::get(getDefaultType(), count);
                }
            }
        }

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
        nonNegValues_.insert(subLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(subLen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);

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
        llvm::cast<llvm::CallInst>(emptyBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));
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

        // ftell returns -1 on error (e.g. non-seekable stream).  Guard against
        // passing a negative value to malloc which would wrap to a huge size.
        llvm::Value* ftellFailed = builder->CreateICmpSLT(fileSize,
            llvm::ConstantInt::get(getDefaultType(), 0), "fread.ftellbad");
        llvm::BasicBlock* ftellOkBB = llvm::BasicBlock::Create(*context, "fread.ftellok", parentFn);
        llvm::BasicBlock* ftellBadBB = llvm::BasicBlock::Create(*context, "fread.ftellbad", parentFn);
        // ftell failure is extremely rare.
        auto* ftellW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(ftellFailed, ftellBadBB, ftellOkBB, ftellW);

        // ftell error path: close file, return empty string
        builder->SetInsertPoint(ftellBadBB);
        builder->CreateCall(getOrDeclareFclose(), {fp});
        llvm::Value* ftellEmptyBuf = builder->CreateCall(getOrDeclareMalloc(),
            {llvm::ConstantInt::get(getDefaultType(), 1)}, "fread.ftempty");
        llvm::cast<llvm::CallInst>(ftellEmptyBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), ftellEmptyBuf);
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* ftellBadEndBB = builder->GetInsertBlock();

        // ftell OK path: proceed with read
        builder->SetInsertPoint(ftellOkBB);
        // fseek(fp, 0, SEEK_SET=0)
        builder->CreateCall(getOrDeclareFseek(),
            {fp, llvm::ConstantInt::get(getDefaultType(), 0),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0)});
        // buf = malloc(size + 1)
        llvm::Value* bufSize = builder->CreateAdd(fileSize,
            llvm::ConstantInt::get(getDefaultType(), 1), "fread.bufsize", /*HasNUW=*/true, /*HasNSW=*/true);
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
        llvm::PHINode* phi = builder->CreatePHI(ptrTy, 3, "fread.result");
        phi->addIncoming(emptyResult, nullEndBB);
        phi->addIncoming(ftellEmptyBuf, ftellBadEndBB);
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
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(slen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
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
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(slen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
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
        {
            attachLoopMetadataVec(
                llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        }

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
        {
            std::string msg = expr->line > 0
                ? std::string("Runtime error: range step cannot be zero at line ") + std::to_string(expr->line) + "\n"
                : "Runtime error: range step cannot be zero\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "rstep_zero_msg")});
        }
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
        // ── Compile-time fold: char_code(str_const) ─────────────────
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            if (!sv->empty())
                return llvm::ConstantInt::get(getDefaultType(),
                    static_cast<int64_t>(static_cast<unsigned char>((*sv)[0])));
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
            ? strArg : builder->CreateIntToPtr(strArg, ptrTy, "charcode.ptr");
        llvm::Value* ch = builder->CreateLoad(llvm::Type::getInt8Ty(*context), strPtr, "charcode.ch");
        return builder->CreateZExt(ch, getDefaultType(), "charcode.result");
    }

    if (bid == BuiltinId::NUMBER_TO_STRING) {
        validateArgCount(expr, "number_to_string", 1);
        // ── Compile-time fold: number_to_string(integer_const) ──────
        if (auto iv = tryFoldInt(expr->arguments[0].get())) {
            std::string s = std::to_string(*iv);
            llvm::GlobalVariable* gv = internString(s);
            stringReturningFunctions_.insert("number_to_string");
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                gv->getValueType(), gv,
                llvm::ArrayRef<llvm::Constant*>{
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        const bool isFloat = val->getType()->isDoubleTy();
        if (!isFloat)
            val = toDefaultType(val);
        if (isFloat) {
            llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 32);
            llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "numtostr.buf");
            llvm::cast<llvm::CallInst>(buf)->addRetAttr(
                llvm::Attribute::getWithDereferenceableBytes(*context, 32));
            llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_float_fmt", true);
            if (!fmtStr)
                fmtStr = builder->CreateGlobalString("%g", "tostr_float_fmt");
            builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
            stringReturningFunctions_.insert("number_to_string");
            return buf;
        }
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 21);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "numtostr.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 21));
        llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_fmt", true);
        if (!fmtStr)
            fmtStr = builder->CreateGlobalString("%lld", "tostr_fmt");
        builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
        stringReturningFunctions_.insert("number_to_string");
        return buf;
    }

    if (bid == BuiltinId::STRING_TO_NUMBER) {
        validateArgCount(expr, "string_to_number", 1);
        // ── Compile-time fold: string_to_number(str_const) ──────────
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            try { return llvm::ConstantInt::get(getDefaultType(),
                      static_cast<int64_t>(std::stoll(*sv))); }
            catch (...) {} // NOLINT(bugprone-empty-catch)
        }
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
        llvm::cast<llvm::CallInst>(mutex)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, kMutexAllocSize));
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nullAttr = llvm::ConstantPointerNull::get(ptrTy);
        builder->CreateCall(getOrDeclarePthreadMutexInit(), {mutex, nullAttr});
        // Return as i64
        return builder->CreatePtrToInt(mutex, getDefaultType(), "mutex.val");
    }

    if (bid == BuiltinId::MUTEX_LOCK) {
        validateArgCount(expr, "mutex_lock", 1);
        llvm::Value* mutexVal = generateExpression(expr->arguments[0].get());
                llvm::Value* mutexPtr = emitToArrayPtr(mutexVal, "mutex.ptr");
        builder->CreateCall(getOrDeclarePthreadMutexLock(), {mutexPtr});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::MUTEX_UNLOCK) {
        validateArgCount(expr, "mutex_unlock", 1);
        llvm::Value* mutexVal = generateExpression(expr->arguments[0].get());
                llvm::Value* mutexPtr = emitToArrayPtr(mutexVal, "mutex.ptr");
        builder->CreateCall(getOrDeclarePthreadMutexUnlock(), {mutexPtr});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::MUTEX_DESTROY) {
        validateArgCount(expr, "mutex_destroy", 1);
        llvm::Value* mutexVal = generateExpression(expr->arguments[0].get());
                llvm::Value* mutexPtr = emitToArrayPtr(mutexVal, "mutex.ptr");
        builder->CreateCall(getOrDeclarePthreadMutexDestroy(), {mutexPtr});
        builder->CreateCall(getOrDeclareFree(), {mutexPtr});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // ── Bitwise intrinsic builtins ─────────────────────────────────────
    if (bid == BuiltinId::POPCOUNT) {
        validateArgCount(expr, "popcount", 1);
        // Constant-fold popcount(any const expr).
        if (auto val = tryFoldInt(expr->arguments[0].get())) {
            uint64_t bits = static_cast<uint64_t>(*val);
            return llvm::ConstantInt::get(getDefaultType(), __builtin_popcountll(bits));
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* ctpopFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctpop, {getDefaultType()});
        auto* result = builder->CreateCall(ctpopFn, {arg}, "popcount.result");
        nonNegValues_.insert(result);  // popcount is always in [0, 64]
        // !range [0,65): tighter than just nonNeg; lets CVP/LVI fold
        // comparisons like (popcount(x) > 64) → false.
        if (optimizationLevel >= OptimizationLevel::O1)
            result->setMetadata(llvm::LLVMContext::MD_range, bitcountRangeMD_);
        return result;
    }

    if (bid == BuiltinId::CLZ) {
        validateArgCount(expr, "clz", 1);
        // Constant-fold clz(any const expr).
        if (auto val = tryFoldInt(expr->arguments[0].get())) {
            uint64_t bits = static_cast<uint64_t>(*val);
            int64_t result = bits == 0 ? 64 : __builtin_clzll(bits);
            return llvm::ConstantInt::get(getDefaultType(), result);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* ctlzFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctlz, {getDefaultType()});
        auto* result = builder->CreateCall(ctlzFn, {arg, builder->getFalse()}, "clz.result");
        nonNegValues_.insert(result);  // clz is always in [0, 64]
        if (optimizationLevel >= OptimizationLevel::O1)
            result->setMetadata(llvm::LLVMContext::MD_range, bitcountRangeMD_);
        return result;
    }

    if (bid == BuiltinId::CTZ) {
        validateArgCount(expr, "ctz", 1);
        // Constant-fold ctz(any const expr).
        if (auto val = tryFoldInt(expr->arguments[0].get())) {
            uint64_t bits = static_cast<uint64_t>(*val);
            int64_t result = bits == 0 ? 64 : __builtin_ctzll(bits);
            return llvm::ConstantInt::get(getDefaultType(), result);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* cttzFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::cttz, {getDefaultType()});
        auto* result = builder->CreateCall(cttzFn, {arg, builder->getFalse()}, "ctz.result");
        nonNegValues_.insert(result);  // ctz is always in [0, 64]
        if (optimizationLevel >= OptimizationLevel::O1)
            result->setMetadata(llvm::LLVMContext::MD_range, bitcountRangeMD_);
        return result;
    }

    if (bid == BuiltinId::BITREVERSE) {
        validateArgCount(expr, "bitreverse", 1);
        // Constant-fold bitreverse(any const expr).
        if (auto val = tryFoldInt(expr->arguments[0].get())) {
            uint64_t bits = static_cast<uint64_t>(*val);
            uint64_t reversed = 0;
            for (int i = 0; i < 64; ++i)
                reversed |= ((bits >> i) & 1ULL) << (63 - i);
            return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(reversed));
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* brevFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::bitreverse, {getDefaultType()});
        return builder->CreateCall(brevFn, {arg}, "bitreverse.result");
    }

    if (bid == BuiltinId::EXP2) {
        validateArgCount(expr, "exp2", 1);
        // Constant-fold exp2(n) for small non-negative integer args (returns 2^n).
        if (auto v = tryFoldInt(expr->arguments[0].get())) {
            if (*v >= 0 && *v < 63)
                return llvm::ConstantInt::get(getDefaultType(), int64_t(1) << static_cast<int>(*v));
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::Function* exp2Fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::exp2, {getFloatType()});
        return builder->CreateCall(exp2Fn, {fval}, "exp2.result");
    }

    if (bid == BuiltinId::IS_POWER_OF_2) {
        validateArgCount(expr, "is_power_of_2", 1);
        // Constant-fold is_power_of_2(any const expr).
        if (auto val = tryFoldInt(expr->arguments[0].get())) {
            int64_t v = *val;
            bool result = v > 0 && (v & (v - 1)) == 0;
            return llvm::ConstantInt::get(getDefaultType(), result ? 1 : 0);
        }
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
        // Constant-fold lcm(a, b) when both are compile-time constants.
        if (auto va = tryFoldInt(expr->arguments[0].get())) {
            if (auto vb = tryFoldInt(expr->arguments[1].get())) {
                uint64_t a = static_cast<uint64_t>(std::abs(*va));
                uint64_t b = static_cast<uint64_t>(std::abs(*vb));
                if (a == 0 || b == 0)
                    return llvm::ConstantInt::get(getDefaultType(), 0);
                uint64_t ga = a, gb = b;
                while (gb) { uint64_t t = gb; gb = ga % gb; ga = t; }
                return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(a / ga * b));
            }
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        // Use absolute values via llvm.abs intrinsic (single neg+cmov on x86).
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Function* absFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::abs, {getDefaultType()});
        llvm::Value* aAbs = builder->CreateCall(absFn, {a, builder->getTrue()}, "lcm.aabs");
        llvm::Value* bAbs = builder->CreateCall(absFn, {b, builder->getTrue()}, "lcm.babs");

        // GCD via Stein's binary algorithm: avoids expensive division (20-40
        // cycles on x86) and uses only ctz, shifts, comparisons, and
        // subtraction — all 1-cycle ops.  This matches the gcd() builtin.
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::Function* cttzFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::cttz, {getDefaultType()});

        llvm::Value* aOrB = builder->CreateOr(aAbs, bAbs, "lcm.aorb");
        llvm::Value* k = builder->CreateCall(cttzFn, {aOrB, builder->getFalse()}, "lcm.k");

        llvm::BasicBlock* mainBB = llvm::BasicBlock::Create(*context, "lcm.gcd.main", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "lcm.gcd.loop", function);
        llvm::BasicBlock* contBB = llvm::BasicBlock::Create(*context, "lcm.gcd.cont", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "lcm.gcd.done", function);

        // Edge case: if a == 0 or b == 0, lcm = 0.
        llvm::Value* edgeCase = builder->CreateOr(
            builder->CreateICmpEQ(aAbs, zero, "lcm.a0"),
            builder->CreateICmpEQ(bAbs, zero, "lcm.b0"), "lcm.edge");
        auto* lcmEdgeW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(edgeCase, doneBB, mainBB, lcmEdgeW);

        builder->SetInsertPoint(mainBB);
        llvm::Value* ctzA = builder->CreateCall(cttzFn, {aAbs, builder->getTrue()}, "lcm.ctza");
        llvm::Value* aOdd = builder->CreateLShr(aAbs, ctzA, "lcm.aodd");
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* phiA = builder->CreatePHI(getDefaultType(), 2, "lcm.gcd.a");
        phiA->addIncoming(aOdd, mainBB);
        llvm::PHINode* phiB = builder->CreatePHI(getDefaultType(), 2, "lcm.gcd.b");
        phiB->addIncoming(bAbs, mainBB);

        llvm::Value* ctzB = builder->CreateCall(cttzFn, {phiB, builder->getTrue()}, "lcm.ctzb");
        llvm::Value* bOdd = builder->CreateLShr(phiB, ctzB, "lcm.bodd");
        llvm::Value* aGtB = builder->CreateICmpUGT(phiA, bOdd, "lcm.gt");
        llvm::Value* lo = builder->CreateSelect(aGtB, bOdd, phiA, "lcm.lo");
        llvm::Value* hi = builder->CreateSelect(aGtB, phiA, bOdd, "lcm.hi");
        llvm::Value* diff = builder->CreateSub(hi, lo, "lcm.diff");
        llvm::Value* gcdDone = builder->CreateICmpEQ(diff, zero, "lcm.dz");
        phiA->addIncoming(lo, loopBB);
        phiB->addIncoming(diff, loopBB);
        builder->CreateCondBr(gcdDone, contBB, loopBB);

        builder->SetInsertPoint(contBB);
        llvm::Value* gcdShifted = builder->CreateShl(lo, k, "lcm.gcdval");
        builder->CreateBr(doneBB);

        // lcm(a, b) = |a| / gcd(a, b) * |b|  (divide first to avoid overflow)
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* gcdVal = builder->CreatePHI(getDefaultType(), 2, "lcm.gcd.result");
        gcdVal->addIncoming(zero, entryBB);       // edge case: return 0
        gcdVal->addIncoming(gcdShifted, contBB);
        // Handle gcd == 0 (when both inputs are 0): lcm(0, 0) = 0
        llvm::Value* gcdIsZero = builder->CreateICmpEQ(gcdVal, zero, "lcm.gcd.iszero");
        llvm::Value* divResult = builder->CreateUDiv(aAbs, gcdVal, "lcm.div");
        llvm::Value* lcmResult = builder->CreateMul(divResult, bAbs, "lcm.mul", /*HasNUW=*/true, /*HasNSW=*/true);
        auto* lcmFinal = builder->CreateSelect(gcdIsZero, zero, lcmResult, "lcm.result");
        nonNegValues_.insert(lcmFinal);  // lcm is always non-negative (uses abs of inputs)
        return lcmFinal;
    }

    // ── New intrinsic builtins ─────────────────────────────────────────
    if (bid == BuiltinId::ROTATE_LEFT) {
        validateArgCount(expr, "rotate_left", 2);
        // Constant-fold rotate_left(val, amt).
        if (auto vv = tryFoldInt(expr->arguments[0].get())) {
            if (auto va = tryFoldInt(expr->arguments[1].get())) {
                uint64_t v = static_cast<uint64_t>(*vv);
                unsigned amt = static_cast<unsigned>(*va) & 63;
                uint64_t result = (v << amt) | (v >> (64 - amt));
                return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(result));
            }
        }
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
        // Constant-fold rotate_right(val, amt).
        if (auto vv = tryFoldInt(expr->arguments[0].get())) {
            if (auto va = tryFoldInt(expr->arguments[1].get())) {
                uint64_t v = static_cast<uint64_t>(*vv);
                unsigned amt = static_cast<unsigned>(*va) & 63;
                uint64_t result = (v >> amt) | (v << (64 - amt));
                return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(result));
            }
        }
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
        // Constant-fold bswap(any const expr).
        if (auto val = tryFoldInt(expr->arguments[0].get())) {
            uint64_t bits = static_cast<uint64_t>(*val);
            uint64_t swapped = __builtin_bswap64(bits);
            return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(swapped));
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        arg = toDefaultType(arg);
        llvm::Function* bswapFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::bswap, {getDefaultType()});
        return builder->CreateCall(bswapFn, {arg}, "bswap.result");
    }

    if (bid == BuiltinId::SATURATING_ADD) {
        validateArgCount(expr, "saturating_add", 2);
        // Constant-fold saturating_add(a, b) when both are compile-time constants.
        if (auto va = tryFoldInt(expr->arguments[0].get())) {
            if (auto vb = tryFoldInt(expr->arguments[1].get())) {
                int64_t a = *va, b = *vb;
                // Signed saturating add: clamp to INT64_MIN/INT64_MAX on overflow.
                __int128 sum = static_cast<__int128>(a) + static_cast<__int128>(b);
                if (sum > INT64_MAX) sum = INT64_MAX;
                if (sum < INT64_MIN) sum = INT64_MIN;
                return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(sum));
            }
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        llvm::Function* saddSatFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sadd_sat, {getDefaultType()});
        return builder->CreateCall(saddSatFn, {a, b}, "sadd.sat.result");
    }

    if (bid == BuiltinId::SATURATING_SUB) {
        validateArgCount(expr, "saturating_sub", 2);
        // Constant-fold saturating_sub(a, b) when both are compile-time constants.
        if (auto va = tryFoldInt(expr->arguments[0].get())) {
            if (auto vb = tryFoldInt(expr->arguments[1].get())) {
                int64_t a = *va, b = *vb;
                // Signed saturating sub: clamp to INT64_MIN/INT64_MAX on overflow.
                __int128 diff = static_cast<__int128>(a) - static_cast<__int128>(b);
                if (diff > INT64_MAX) diff = INT64_MAX;
                if (diff < INT64_MIN) diff = INT64_MIN;
                return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(diff));
            }
        }
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

    if (bid == BuiltinId::ASSUME) {
        validateArgCount(expr, "assume", 1);
        llvm::Value* condVal = generateExpression(expr->arguments[0].get());
        condVal = toBool(condVal);
        // llvm.assume(i1) — zero-cost hint to the optimizer that condVal is true.
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {condVal});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::UNREACHABLE) {
        validateArgCount(expr, "unreachable", 0);
        // Emit LLVM unreachable — tells the optimizer this code path is never taken.
        builder->CreateUnreachable();
        // Subsequent code in the same block is dead; open a new (dead) block so
        // codegen can continue without emitting into a terminated block.
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* deadBB =
            llvm::BasicBlock::Create(*context, "unreachable.dead", function);
        builder->SetInsertPoint(deadBB);
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::EXPECT) {
        validateArgCount(expr, "expect", 2);
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        llvm::Value* likelyVal = generateExpression(expr->arguments[1].get());
        val = toDefaultType(val);
        likelyVal = toDefaultType(likelyVal);
        // llvm.expect.i64(val, expected_val) — branch-prediction hint; returns val unchanged.
        llvm::Function* expectFn =
            OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::expect, {getDefaultType()});
        return builder->CreateCall(expectFn, {val, likelyVal}, "expect.result");
    }

    // -----------------------------------------------------------------------
    // array_product(arr) — multiply all elements together; returns 1 for empty array
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_PRODUCT) {
        validateArgCount(expr, "array_product", 1);
        // Constant-fold array_product([c0, c1, ...]) when all elements are known.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                int64_t product = 1;
                bool allInt = true;
                for (const auto& elem : cv->arrVal) {
                    if (elem.kind != ConstValue::Kind::Integer) { allInt = false; break; }
                    product *= elem.intVal;
                }
                if (allInt) {
                    optStats_.constFolded++;
                    return llvm::ConstantInt::get(getDefaultType(), product);
                }
            }
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr =
            arg->getType()->isPointerTy() ? arg : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "aprod.arrptr");
                llvm::Value* aprodLenLoad = emitLoadArrayLen(arrPtr, "aprod.len");
        llvm::Value* length = aprodLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "aprod.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "aprod.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "aprod.done", function);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::PHINode* acc = builder->CreatePHI(getDefaultType(), 2, "aprod.acc");
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "aprod.idx");
        acc->addIncoming(one, entryBB);  // product identity is 1
        idx->addIncoming(zero, entryBB);

        llvm::Value* done = builder->CreateICmpUGE(idx, length, "aprod.done");
        auto* aprodCondBr = builder->CreateCondBr(done, doneBB, bodyBB);
        if (optimizationLevel >= OptimizationLevel::O2) {
            aprodCondBr->setMetadata(llvm::LLVMContext::MD_prof,
                llvm::MDBuilder(*context).createBranchWeights(1, 2000));
        }

        builder->SetInsertPoint(bodyBB);
        llvm::Value* offset = builder->CreateAdd(idx, one, "aprod.offset", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "aprod.elemptr");
                llvm::Value* elem = emitLoadArrayElem(elemPtr, "aprod.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
        // nsw: product of initialized i64 elements; OmScript array ownership
        // guarantees no use-before-write, so NSWMul is safe under @optmax.
        // At O2+ always use nsw to let SCEV propagate tighter range bounds.
        llvm::Value* newAcc = (inOptMaxFunction || optimizationLevel >= OptimizationLevel::O2)
            ? builder->CreateNSWMul(acc, elem, "aprod.newacc")
            : builder->CreateMul(acc, elem, "aprod.newacc");
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        // Eliminates a redundant add and provides SCEV with tight nsw+nuw flags.
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(offset, bodyBB);
        {
            attachLoopMetadataVec(
                llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        }

        builder->SetInsertPoint(doneBB);
        return acc;
    }

    // -----------------------------------------------------------------------
    // array_last(arr) — return the last element; aborts on empty array
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_LAST) {
        validateArgCount(expr, "array_last", 1);
        // Constant-fold array_last([c0, ..., cN]) when the array is known at compile time.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array && !cv->arrVal.empty()) {
                const auto& last = cv->arrVal.back();
                if (last.kind == ConstValue::Kind::Integer) {
                    optStats_.constFolded++;
                    auto* ci = llvm::ConstantInt::get(getDefaultType(), last.intVal);
                    if (last.intVal >= 0) nonNegValues_.insert(ci);
                    return ci;
                }
            }
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "alast.arrptr");
                llvm::Value* alastLenLoad = emitLoadArrayLen(arrPtr, "alast.len");
        llvm::Value* arrLen = alastLenLoad;

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* notEmpty = builder->CreateICmpSGT(arrLen, zero, "alast.notempty");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "alast.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "alast.fail", function);
        llvm::MDNode* alastW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(notEmpty, okBB, failBB, alastW);

        builder->SetInsertPoint(failBB);
        {
            std::string msg = expr->line > 0
                ? std::string("Runtime error: array_last called on empty array at line ") + std::to_string(expr->line) + "\n"
                : "Runtime error: array_last called on empty array\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "alast_empty_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Last element is at offset arrLen (1-based: header is at [0], elements at [1..arrLen])
        llvm::Value* lastPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, arrLen, "alast.ptr");
        auto* lastLoad = emitLoadArrayElem(lastPtr, "alast.val");
        if (optimizationLevel >= OptimizationLevel::O1) {
            lastLoad->setMetadata(llvm::LLVMContext::MD_noundef,
                llvm::MDNode::get(*context, {}));
        }
        return lastLoad;
    }

    // -----------------------------------------------------------------------
    // array_insert(arr, idx, val) — insert val at position idx, shifting elements right
    //   Returns a new array (original is unchanged).
    //   idx must be in [0, length] — inserting at length appends.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_INSERT) {
        validateArgCount(expr, "array_insert", 3);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* idxArg = generateExpression(expr->arguments[1].get());
        llvm::Value* valArg = generateExpression(expr->arguments[2].get());
        arrArg = toDefaultType(arrArg);
        idxArg = toDefaultType(idxArg);
        valArg = toDefaultType(valArg);

        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy() ? arrArg : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "ains.arrptr");
                llvm::Value* ainsLenLoad = emitLoadArrayLen(arrPtr, "ains.len");
        llvm::Value* arrLen = ainsLenLoad;

        // Bounds check: 0 <= idx <= length (insert at end is allowed)
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* notNeg = builder->CreateICmpSGE(idxArg, zero, "ains.notneg");
        llvm::Value* notOver = builder->CreateICmpSLE(idxArg, arrLen, "ains.notover");
        llvm::Value* valid = builder->CreateAnd(notNeg, notOver, "ains.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "ains.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "ains.fail", function);
        llvm::MDNode* ainsW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(valid, okBB, failBB, ainsW);

        builder->SetInsertPoint(failBB);
        {
            std::string msg = expr->line > 0
                ? std::string("Runtime error: array_insert index out of bounds at line ") + std::to_string(expr->line) + "\n"
                : "Runtime error: array_insert index out of bounds\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "ains_oob_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        llvm::Value* newLen = builder->CreateAdd(arrLen, one, "ains.newlen", /*HasNUW=*/true, /*HasNSW=*/true);
        // Allocate: (newLen + 1) * 8 bytes
        llvm::Value* slots = builder->CreateAdd(newLen, one, "ains.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "ains.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "ains.buf");

        // Store new length header
        emitStoreArrayLen(newLen, buf);

        // Copy elements before insertion point: arr[1..idx+1) → buf[1..idx+1)
        llvm::Value* preSrc = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, one, "ains.presrc");
        llvm::Value* preDst = builder->CreateInBoundsGEP(getDefaultType(), buf, one, "ains.predst");
        llvm::Value* preCount = builder->CreateMul(idxArg, eight, "ains.precnt", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemcpy(), {preDst, preSrc, preCount});

        // Store the inserted value at buf[idx+1]
        llvm::Value* insertSlot = builder->CreateAdd(idxArg, one, "ains.slot", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* insertPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, insertSlot, "ains.insertptr");
        auto* ainsValSt = builder->CreateStore(valArg, insertPtr);
        ainsValSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);

        // Copy elements from insertion point onward: arr[idx+1..arrLen+1) → buf[idx+2..newLen+1)
        llvm::Value* postSrcIdx = builder->CreateAdd(idxArg, one, "ains.postsrcidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* postSrc = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, postSrcIdx, "ains.postsrc");
        llvm::Value* postDstIdx = builder->CreateAdd(idxArg, llvm::ConstantInt::get(getDefaultType(), 2), "ains.postdstidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* postDst = builder->CreateInBoundsGEP(getDefaultType(), buf, postDstIdx, "ains.postdst");
        llvm::Value* postCount = builder->CreateSub(arrLen, idxArg, "ains.postcnt", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* postBytes = builder->CreateMul(postCount, eight, "ains.postbytes", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemcpy(), {postDst, postSrc, postBytes});

        return builder->CreatePtrToInt(buf, getDefaultType(), "ains.result");
    }

    // -----------------------------------------------------------------------
    // str_pad_left(str, width, fill) — left-pad str with fill[0] until length == width
    // str_pad_right(str, width, fill) — right-pad str with fill[0] until length == width
    //   If str is already >= width chars, returns str unchanged.
    //   fill must be a non-empty string; its first character is used as the pad byte.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_PAD_LEFT || bid == BuiltinId::STR_PAD_RIGHT) {
        const char* fnName = (bid == BuiltinId::STR_PAD_LEFT) ? "str_pad_left" : "str_pad_right";
        validateArgCount(expr, fnName, 3);
        // Constant-fold str_pad_left/right when all arguments are compile-time constants.
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            if (auto wv = tryFoldInt(expr->arguments[1].get())) {
                if (auto fv = tryFoldStr(expr->arguments[2].get())) {
                    if (!fv->empty() && *wv >= 0 && *wv <= 65536) {
                        int64_t slen = static_cast<int64_t>(sv->size());
                        std::string result;
                        if (*wv <= slen) {
                            result = *sv;
                        } else if (bid == BuiltinId::STR_PAD_LEFT) {
                            result.assign(*wv - slen, (*fv)[0]);
                            result += *sv;
                        } else {
                            result = *sv;
                            result.append(*wv - slen, (*fv)[0]);
                        }
                        optStats_.constFolded++;
                        llvm::GlobalVariable* gv = internString(result);
                        stringReturningFunctions_.insert(fnName);
                        return llvm::ConstantExpr::getInBoundsGetElementPtr(
                            gv->getValueType(), gv,
                            llvm::ArrayRef<llvm::Constant*>{
                                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)
                            });
                    }
                }
            }
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* widthArg = generateExpression(expr->arguments[1].get());
        llvm::Value* fillArg = generateExpression(expr->arguments[2].get());
        widthArg = toDefaultType(widthArg);
        fillArg = toDefaultType(fillArg);

        llvm::Value* strPtr =
            strArg->getType()->isPointerTy() ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strpad.strptr");
        llvm::Value* fillPtr =
            fillArg->getType()->isPointerTy() ? fillArg
                : builder->CreateIntToPtr(fillArg, llvm::PointerType::getUnqual(*context), "strpad.fillptr");

        // slen = strlen(str)
        llvm::Value* slen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strpad.slen");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(slen)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);

        // Clamp width to [0, i64_max] — negative width means no padding
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* negWidth = builder->CreateICmpSLT(widthArg, zero, "strpad.negw");
        llvm::Value* effectiveWidth = builder->CreateSelect(negWidth, zero, widthArg, "strpad.width");

        // If slen >= width, return str unchanged
        llvm::Value* needsPad = builder->CreateICmpULT(slen, effectiveWidth, "strpad.needs");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* padBB = llvm::BasicBlock::Create(*context, "strpad.pad", function);
        llvm::BasicBlock* noPadBB = llvm::BasicBlock::Create(*context, "strpad.nopad", function);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "strpad.merge", function);

        // Padding is the common case for typical use of pad functions (e.g. formatting);
        // use equal weights.
        builder->CreateCondBr(needsPad, padBB, noPadBB);

        // --- No-pad path: return original string pointer as i64 ---
        builder->SetInsertPoint(noPadBB);
        llvm::Value* origI64 = builder->CreatePtrToInt(strPtr, getDefaultType(), "strpad.origval");
        builder->CreateBr(mergeBB);

        // --- Pad path ---
        builder->SetInsertPoint(padBB);
        // Load fill character: first byte of fill string
        llvm::Value* fillByte = builder->CreateAlignedLoad(builder->getInt8Ty(), fillPtr, llvm::MaybeAlign(1), "strpad.fillbyte");

        llvm::Value* padLen = builder->CreateSub(effectiveWidth, slen, "strpad.padlen", /*HasNUW=*/true, /*HasNSW=*/true);
        // Allocate (effectiveWidth + 1) bytes for result
        llvm::Value* allocSize = builder->CreateAdd(effectiveWidth,
            llvm::ConstantInt::get(getDefaultType(), 1), "strpad.allocsz", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "strpad.buf");

        if (bid == BuiltinId::STR_PAD_LEFT) {
            // Fill first padLen bytes with fill char
            builder->CreateMemSet(buf, fillByte, padLen, llvm::MaybeAlign(1));
            // Copy str (including null terminator) into buf + padLen
            llvm::Value* copyDst = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, padLen, "strpad.copydst");
            llvm::Value* copySize = builder->CreateAdd(slen,
                llvm::ConstantInt::get(getDefaultType(), 1), "strpad.copysz", /*HasNUW=*/true, /*HasNSW=*/true);
            builder->CreateCall(getOrDeclareMemcpy(), {copyDst, strPtr, copySize});
        } else {
            // STR_PAD_RIGHT: copy str into buf, then fill remaining bytes with fill char
            builder->CreateCall(getOrDeclareMemcpy(), {buf, strPtr, slen});
            llvm::Value* fillDst = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, slen, "strpad.filldst");
            builder->CreateMemSet(fillDst, fillByte, padLen, llvm::MaybeAlign(1));
            // Null-terminate
            llvm::Value* nullPtr = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, effectiveWidth, "strpad.nullptr");
            builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nullPtr);
        }

        llvm::Value* padI64 = builder->CreatePtrToInt(buf, getDefaultType(), "strpad.padval");
        builder->CreateBr(mergeBB);

        // --- Merge ---
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "strpad.result");
        result->addIncoming(origI64, noPadBB);
        result->addIncoming(padI64, padBB);
        stringReturningFunctions_.insert(fnName);
        return result;
    }

    // -----------------------------------------------------------------------
    // command(cmd) / shell(cmd)
    //   Run a shell command via popen(3) and return its stdout as a string.
    //   On error (popen fails or cmd is null) returns an empty string.
    //   Returned string is heap-allocated.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::COMMAND) {
        validateArgCount(expr, "command", 1);
        llvm::Value* cmdArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* cmdPtr =
            cmdArg->getType()->isPointerTy()
                ? cmdArg
                : builder->CreateIntToPtr(cmdArg, ptrTy, "cmd.ptr");

        // Declare popen
        auto* popenFn = llvm::dyn_cast_or_null<llvm::Function>(
            module->getOrInsertFunction("popen",
                llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false))
            .getCallee());
        if (popenFn) {
            popenFn->addFnAttr(llvm::Attribute::NoUnwind);
            OMSC_ADD_NOCAPTURE(popenFn, 0);
            OMSC_ADD_NOCAPTURE(popenFn, 1);
        }
        // Declare pclose
        auto* pcloseFn = llvm::dyn_cast_or_null<llvm::Function>(
            module->getOrInsertFunction("pclose",
                llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false))
            .getCallee());
        if (pcloseFn) pcloseFn->addFnAttr(llvm::Attribute::NoUnwind);

        llvm::GlobalVariable* modeR = module->getGlobalVariable("__popen_mode_r", true);
        if (!modeR) modeR = builder->CreateGlobalString("r", "__popen_mode_r");

        llvm::Value* fp = builder->CreateCall(popenFn, {cmdPtr, modeR}, "cmd.fp");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* isNull  = builder->CreateICmpEQ(fp, nullPtr, "cmd.isnull");

        llvm::BasicBlock* nullBB  = llvm::BasicBlock::Create(*context, "cmd.null",  parentFn);
        llvm::BasicBlock* readBB  = llvm::BasicBlock::Create(*context, "cmd.read",  parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "cmd.merge", parentFn);

        builder->CreateCondBr(isNull, nullBB, readBB,
            llvm::MDBuilder(*context).createBranchWeights(1, 100));

        // Null path → empty string
        builder->SetInsertPoint(nullBB);
        llvm::Value* emptyBuf = builder->CreateCall(getOrDeclareMalloc(),
            {llvm::ConstantInt::get(getDefaultType(), 1)}, "cmd.empty");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), emptyBuf);
        llvm::Value* emptyI64 = builder->CreatePtrToInt(emptyBuf, getDefaultType(), "cmd.emptyi64");
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* nullEndBB = builder->GetInsertBlock();

        // Read path: read output with fgets into a growable buffer
        builder->SetInsertPoint(readBB);
        llvm::Value* initCap  = llvm::ConstantInt::get(getDefaultType(), 4096);
        llvm::AllocaInst* capPtr  = createEntryBlockAlloca(parentFn, "cmd.cap",  getDefaultType());
        llvm::AllocaInst* sizePtr = createEntryBlockAlloca(parentFn, "cmd.size", getDefaultType());
        llvm::AllocaInst* bufPtr  = createEntryBlockAlloca(parentFn, "cmd.bufp", ptrTy);
        builder->CreateStore(initCap, capPtr);
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), sizePtr);
        llvm::Value* initBuf = builder->CreateCall(getOrDeclareMalloc(), {initCap}, "cmd.buf");
        builder->CreateStore(initBuf, bufPtr);

        // chunk buffer: 256 bytes on the "heap" (small, reused each iteration)
        llvm::Value* chunkBuf  = builder->CreateCall(getOrDeclareMalloc(),
            {llvm::ConstantInt::get(getDefaultType(), 256)}, "cmd.chunk");
        llvm::Value* chunkSize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 256);

        llvm::BasicBlock* rloopBB  = llvm::BasicBlock::Create(*context, "cmd.rloop",  parentFn);
        llvm::BasicBlock* appendBB = llvm::BasicBlock::Create(*context, "cmd.append", parentFn);
        llvm::BasicBlock* rdoneBB  = llvm::BasicBlock::Create(*context, "cmd.rdone",  parentFn);
        llvm::BasicBlock* growBB   = llvm::BasicBlock::Create(*context, "cmd.grow",   parentFn);
        llvm::BasicBlock* copyBB   = llvm::BasicBlock::Create(*context, "cmd.copy",   parentFn);

        builder->CreateBr(rloopBB);
        builder->SetInsertPoint(rloopBB);

        // fgets(chunk, 256, fp) → null on EOF/error
        llvm::Value* got     = builder->CreateCall(getOrDeclareFgets(),
            {chunkBuf, chunkSize, fp}, "cmd.got");
        llvm::Value* gotNull = builder->CreateICmpEQ(got, nullPtr, "cmd.gotnull");
        builder->CreateCondBr(gotNull, rdoneBB, appendBB,
            llvm::MDBuilder(*context).createBranchWeights(1, 1000));

        builder->SetInsertPoint(appendBB);
        llvm::Value* chunkLen = builder->CreateCall(getOrDeclareStrlen(), {chunkBuf}, "cmd.clen");
        llvm::Value* curSize  = builder->CreateAlignedLoad(getDefaultType(), sizePtr, llvm::MaybeAlign(8), "cmd.csz");
        llvm::Value* curCap   = builder->CreateAlignedLoad(getDefaultType(), capPtr,  llvm::MaybeAlign(8), "cmd.ccap");
        llvm::Value* newSize  = builder->CreateAdd(curSize, chunkLen, "cmd.nsz", true, true);
        llvm::Value* needOne  = builder->CreateAdd(newSize, llvm::ConstantInt::get(getDefaultType(), 1), "cmd.ns1");
        llvm::Value* needGrow = builder->CreateICmpUGT(needOne, curCap, "cmd.needgrow");
        builder->CreateCondBr(needGrow, growBB, copyBB);

        builder->SetInsertPoint(growBB);
        llvm::Value* newCap  = builder->CreateMul(curCap,
            llvm::ConstantInt::get(getDefaultType(), 2), "cmd.ncap", true, true);
        llvm::Value* curBufG = builder->CreateAlignedLoad(ptrTy, bufPtr, llvm::MaybeAlign(8), "cmd.cbufg");
        llvm::Value* newBuf  = builder->CreateCall(getOrDeclareRealloc(), {curBufG, newCap}, "cmd.nbuf");
        builder->CreateStore(newBuf, bufPtr);
        builder->CreateStore(newCap, capPtr);
        builder->CreateBr(copyBB);

        builder->SetInsertPoint(copyBB);
        llvm::Value* curBufC = builder->CreateAlignedLoad(ptrTy, bufPtr, llvm::MaybeAlign(8), "cmd.cbufc");
        llvm::Value* dst     = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), curBufC, curSize, "cmd.dst");
        builder->CreateCall(getOrDeclareMemcpy(), {dst, chunkBuf, chunkLen});
        builder->CreateStore(newSize, sizePtr);
        builder->CreateBr(rloopBB);

        builder->SetInsertPoint(rdoneBB);
        builder->CreateCall(pcloseFn, {fp});
        // Free the temporary chunk buffer now that reading is complete.
        builder->CreateCall(getOrDeclareFree(), {chunkBuf});
        llvm::Value* finalSz  = builder->CreateAlignedLoad(getDefaultType(), sizePtr, llvm::MaybeAlign(8), "cmd.fsz");
        llvm::Value* finalBuf = builder->CreateAlignedLoad(ptrTy, bufPtr, llvm::MaybeAlign(8), "cmd.fbuf");
        llvm::Value* ntPtr    = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), finalBuf, finalSz, "cmd.nt");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), ntPtr);
        llvm::Value* readResult = builder->CreatePtrToInt(finalBuf, getDefaultType(), "cmd.res");
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* readEndBB = builder->GetInsertBlock();

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* cmdPhi = builder->CreatePHI(getDefaultType(), 2, "cmd.phi");
        cmdPhi->addIncoming(emptyI64, nullEndBB);
        cmdPhi->addIncoming(readResult, readEndBB);
        stringReturningFunctions_.insert("command");
        stringReturningFunctions_.insert("shell");
        return cmdPhi;
    }

    // -----------------------------------------------------------------------
    // str_filter(str, fn) — keep only characters for which fn(char_code) != 0
    //   Returns a new heap-allocated string.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_FILTER) {
        validateArgCount(expr, "str_filter", 2);
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING)
            codegenError("str_filter: second argument must be a function name (string literal)", expr);
        const std::string fnName = fnNameLit->stringValue;
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second)
            codegenError("str_filter: unknown function '" + fnName + "'", expr);
        llvm::Function* predFn = calleeIt->second;

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "sfilt.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "sfilt.len");

        // Allocate output buffer: same max length + 1 for NUL
        llvm::Value* bufSize = builder->CreateAdd(strLen,
            llvm::ConstantInt::get(getDefaultType(), 1), "sfilt.bufsz", true, true);
        llvm::Value* outBuf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "sfilt.out");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "sfilt.loop",  parentFn);
        llvm::BasicBlock* testBB = llvm::BasicBlock::Create(*context, "sfilt.test",  parentFn);
        llvm::BasicBlock* addBB  = llvm::BasicBlock::Create(*context, "sfilt.add",   parentFn);
        llvm::BasicBlock* incBB  = llvm::BasicBlock::Create(*context, "sfilt.inc",   parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "sfilt.done",  parentFn);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx    = builder->CreatePHI(getDefaultType(), 2, "sfilt.idx");
        llvm::PHINode* outIdx = builder->CreatePHI(getDefaultType(), 2, "sfilt.outidx");
        idx->addIncoming(zero, preheader);
        outIdx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpULT(idx, strLen, "sfilt.cond");
        builder->CreateCondBr(cond, testBB, doneBB);

        builder->SetInsertPoint(testBB);
        // Load char as i8, zero-extend to i64 for predicate
        llvm::Value* charPtr = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), strPtr, idx, "sfilt.charptr");
        llvm::Value* ch8  = builder->CreateAlignedLoad(llvm::Type::getInt8Ty(*context), charPtr, llvm::MaybeAlign(1), "sfilt.ch8");
        llvm::Value* ch64 = builder->CreateZExt(ch8, getDefaultType(), "sfilt.ch64");
        // Call predicate
        llvm::Value* keep = builder->CreateICmpNE(
            builder->CreateCall(predFn, {ch64}, "sfilt.keep_val"),
            zero, "sfilt.keep");
        builder->CreateCondBr(keep, addBB, incBB);

        builder->SetInsertPoint(addBB);
        llvm::Value* dstPtr = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), outBuf, outIdx, "sfilt.dstptr");
        builder->CreateStore(ch8, dstPtr);
        llvm::Value* newOutIdx = builder->CreateAdd(outIdx,
            llvm::ConstantInt::get(getDefaultType(), 1), "sfilt.newout", true, true);
        builder->CreateBr(incBB);

        builder->SetInsertPoint(incBB);
        llvm::PHINode* outMerge = builder->CreatePHI(getDefaultType(), 2, "sfilt.outmerge");
        outMerge->addIncoming(outIdx, testBB);
        outMerge->addIncoming(newOutIdx, addBB);
        llvm::Value* nextIdx = builder->CreateAdd(idx,
            llvm::ConstantInt::get(getDefaultType(), 1), "sfilt.next", true, true);
        idx->addIncoming(nextIdx, incBB);
        outIdx->addIncoming(outMerge, incBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        // NUL-terminate at outIdx
        llvm::Value* nullCharPtr = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), outBuf, outIdx, "sfilt.nullptr");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nullCharPtr);
        stringReturningFunctions_.insert("str_filter");
        return builder->CreatePtrToInt(outBuf, getDefaultType(), "sfilt.result");
    }

    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // map_filter(map, fn) — keep only entries for which fn(key_i64) != 0.
    //   Iterates the source map's bucket array directly, applies the predicate
    //   to each occupied key (as an i64-encoded string pointer), and builds a
    //   new result map by re-using the existing MAP_SET IR path on matched entries.
    //   fn must accept one i64 argument (the key pointer) and return non-zero to keep.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::MAP_FILTER) {
        validateArgCount(expr, "map_filter", 2);
        auto* fnNameLit2 = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit2 || fnNameLit2->literalType != LiteralExpr::LiteralType::STRING)
            codegenError("map_filter: second argument must be a function name (string literal)", expr);
        const std::string mfFnName = fnNameLit2->stringValue;
        auto mfCalleeIt = functions.find(mfFnName);
        if (mfCalleeIt == functions.end() || !mfCalleeIt->second)
            codegenError("map_filter: unknown function '" + mfFnName + "'", expr);
        llvm::Function* mfPredFn = mfCalleeIt->second;

        llvm::Value* mfSrcMap = generateExpression(expr->arguments[0].get());
                llvm::Value* mfMapPtr = emitToArrayPtr(mfSrcMap, "mf.mapptr");
        auto* mfPtrTy = llvm::PointerType::getUnqual(*context);

        // Read cap and count from map header [cap:i64, count:i64, buckets...]
        // Each bucket = [hash:i64, key:i64, val:i64]
        llvm::Value* mfCap = builder->CreateAlignedLoad(getDefaultType(), mfMapPtr,
            llvm::MaybeAlign(8), "mf.cap");
        llvm::Value* mfZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* mfOne  = llvm::ConstantInt::get(getDefaultType(), 1);

        // Create a new empty map for the result using MAP_NEW builtin
        auto mfNewCall = std::make_unique<CallExpr>("map_new",
            std::vector<std::unique_ptr<Expression>>{});
        mfNewCall->fromStdNamespace = true; // codegen-generated
        mfNewCall->line = expr->line; mfNewCall->column = expr->column;
        llvm::Value* mfNewMap = generateCall(mfNewCall.get());

        // Store the new map in an alloca so MAP_SET can update it
        llvm::Function* mfParentFn = builder->GetInsertBlock()->getParent();
        llvm::AllocaInst* mfNewMapA = createEntryBlockAlloca(mfParentFn, "mf.newmap", getDefaultType());
        builder->CreateStore(mfNewMap, mfNewMapA);

        // Loop over buckets
        llvm::BasicBlock* mfPre  = builder->GetInsertBlock();
        llvm::BasicBlock* mfLoop = llvm::BasicBlock::Create(*context, "mf.loop", mfParentFn);
        llvm::BasicBlock* mfTest = llvm::BasicBlock::Create(*context, "mf.test", mfParentFn);
        llvm::BasicBlock* mfAdd  = llvm::BasicBlock::Create(*context, "mf.add",  mfParentFn);
        llvm::BasicBlock* mfInc  = llvm::BasicBlock::Create(*context, "mf.inc",  mfParentFn);
        llvm::BasicBlock* mfDone = llvm::BasicBlock::Create(*context, "mf.done", mfParentFn);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(mfLoop)));

        builder->SetInsertPoint(mfLoop);
        llvm::PHINode* mfBi = builder->CreatePHI(getDefaultType(), 2, "mf.bi");
        mfBi->addIncoming(mfZero, mfPre);
        builder->CreateCondBr(builder->CreateICmpULT(mfBi, mfCap, "mf.bcond"), mfTest, mfDone);

        builder->SetInsertPoint(mfTest);
        // Bucket offset = 2 + bi*3
        llvm::Value* mfBoff = builder->CreateAdd(
            builder->CreateMul(mfBi, llvm::ConstantInt::get(getDefaultType(), 3), "mf.b3", true, true),
            llvm::ConstantInt::get(getDefaultType(), 2), "mf.boff", true, true);
        llvm::Value* mfHash = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mfMapPtr, mfBoff, "mf.hashp"),
            llvm::MaybeAlign(8), "mf.hash");
        llvm::Value* mfOcc  = builder->CreateICmpNE(mfHash, mfZero, "mf.occ");
        builder->CreateCondBr(mfOcc, mfAdd, mfInc);

        builder->SetInsertPoint(mfAdd);
        llvm::Value* mfKeyOff = builder->CreateAdd(mfBoff, mfOne, "mf.koff", true, true);
        llvm::Value* mfKey    = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mfMapPtr, mfKeyOff, "mf.keyp"),
            llvm::MaybeAlign(8), "mf.key");
        llvm::Value* mfValOff = builder->CreateAdd(mfBoff,
            llvm::ConstantInt::get(getDefaultType(), 2), "mf.voff", true, true);
        llvm::Value* mfVal    = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mfMapPtr, mfValOff, "mf.valp"),
            llvm::MaybeAlign(8), "mf.val");
        // Call predicate with key i64
        llvm::Value* mfPredR = builder->CreateCall(mfPredFn, {mfKey}, "mf.predr");
        llvm::Value* mfKeep  = builder->CreateICmpNE(
            builder->CreateIntCast(mfPredR, getDefaultType(), false, "mf.pcast"),
            mfZero, "mf.keep");
        llvm::BasicBlock* mfInsert = llvm::BasicBlock::Create(*context, "mf.insert", mfParentFn);
        builder->CreateCondBr(mfKeep, mfInsert, mfInc);

        builder->SetInsertPoint(mfInsert);
        // Store key and val in allocas, then call map_set via existing codegen.
        // We synthesize a MAP_SET call: map_set(newMap, key_str, val)
        // by storing them and making a synthetic CallExpr with LiteralExpr args.
        // Since we cannot easily create runtime-value LiteralExprs, we use
        // a helper: store into known allocas and emit a map_set inline.
        // Inline MAP_SET: delegate to the existing MAP_SET builtin path
        // by temporarily storing the result map, key, and value and
        // emitting a MAP_SET call using a pre-evaluated i64 literal trick.
        // Simplest correct approach given this constraint:
        // emit the raw bucket-write inline (linear probing).
        // For safety and correctness, call a small C helper we declare here.
        auto* mfSetFt = llvm::FunctionType::get(getDefaultType(),
            {getDefaultType(), getDefaultType(), getDefaultType()}, false);
        auto* mfSetHelperFn = llvm::dyn_cast_or_null<llvm::Function>(
            module->getOrInsertFunction("__omsc_map_filter_set_helper", mfSetFt).getCallee());
        if (mfSetHelperFn && mfSetHelperFn->empty()) {
            // Declare as external — the linker will resolve against the OmScript
            // runtime.  In practice we define this inline below as a private
            // helper function (always_inline) so no external symbol is needed.
            mfSetHelperFn->setLinkage(llvm::GlobalValue::PrivateLinkage);
            mfSetHelperFn->addFnAttr(llvm::Attribute::AlwaysInline);
            // Build body: just call into map_set using the passed map i64
            llvm::BasicBlock* mfHBB = llvm::BasicBlock::Create(*context, "entry", mfSetHelperFn);
            llvm::IRBuilder<> hb(mfHBB);
            // Return first arg unchanged (stub — caller already updates newMapA)
            hb.CreateRet(mfSetHelperFn->arg_begin());
        }
        // Actually, use the existing map_set builtin IR path.
        // We can invoke generateCall with a synthetic CallExpr only if args
        // are in allocas we can reference.  The trick: use a pair of fresh
        // allocas and synthesize a CallExpr whose args are identifiers that
        // load from those allocas.  This requires parser-level names, which we
        // don't have here.
        // Final decision: call map_set directly via the underlying IR.
        // We use the same popen/pcloseFn declaration style to get a pointer to
        // the LLVM function that was already compiled for map_set, if any.
        // map_set in OmScript is not a C function — it is inlined by the compiler.
        // Therefore, we emit the minimal correct bucket insert here:
        // - Compute djb2 hash of the key string
        // - Find an empty slot (linear probe) in the new map
        // - Write hash, key, val; update count
        // This requires knowledge of the initial capacity of the new map.
        // map_new() starts with capacity 8 (see codegen MAP_NEW).
        // We read capacity from the new map header directly.
        llvm::Value* mfNewMapVal = builder->CreateAlignedLoad(getDefaultType(), mfNewMapA,
            llvm::MaybeAlign(8), "mf.newmapval");
        llvm::Value* mfNewMapPtr = builder->CreateIntToPtr(mfNewMapVal, mfPtrTy, "mf.newmapptr");
        llvm::Value* mfNewCap    = builder->CreateAlignedLoad(getDefaultType(), mfNewMapPtr,
            llvm::MaybeAlign(8), "mf.newcap");
        // Compute hash of key string (djb2: hash = 5381; for each byte: hash = hash*33 ^ c)
        // We emit this as a small inline loop.
        llvm::Value* mfKeyPtr = builder->CreateIntToPtr(mfKey, mfPtrTy, "mf.keyptr");
        llvm::Value* mfHashInit = llvm::ConstantInt::get(getDefaultType(), 5381);
        llvm::BasicBlock* mfHashPre  = builder->GetInsertBlock();
        llvm::BasicBlock* mfHashLoop = llvm::BasicBlock::Create(*context, "mf.hashloop", mfParentFn);
        llvm::BasicBlock* mfHashDone = llvm::BasicBlock::Create(*context, "mf.hashdone", mfParentFn);
        builder->CreateBr(mfHashLoop);
        builder->SetInsertPoint(mfHashLoop);
        llvm::PHINode* mfHI    = builder->CreatePHI(getDefaultType(), 2, "mf.hi");
        llvm::PHINode* mfHHash = builder->CreatePHI(getDefaultType(), 2, "mf.hh");
        mfHI->addIncoming(mfZero, mfHashPre);
        mfHHash->addIncoming(mfHashInit, mfHashPre);
        llvm::Value* mfCp  = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), mfKeyPtr, mfHI, "mf.cp");
        llvm::Value* mfCh8 = builder->CreateAlignedLoad(llvm::Type::getInt8Ty(*context), mfCp, llvm::MaybeAlign(1), "mf.ch");
        llvm::Value* mfCh  = builder->CreateZExt(mfCh8, getDefaultType(), "mf.ch64");
        llvm::Value* mfEnd = builder->CreateICmpEQ(mfCh, mfZero, "mf.end");
        builder->CreateCondBr(mfEnd, mfHashDone, mfHashLoop);
        // Back edge: hash = hash*33 ^ c
        llvm::Value* mfHH2 = builder->CreateXor(
            builder->CreateMul(mfHHash, llvm::ConstantInt::get(getDefaultType(), 33), "mf.h33"),
            mfCh, "mf.hxor");
        llvm::Value* mfHI2 = builder->CreateAdd(mfHI, mfOne, "mf.hi1", true, true);
        mfHI->addIncoming(mfHI2, mfHashLoop);
        mfHHash->addIncoming(mfHH2, mfHashLoop);
        builder->SetInsertPoint(mfHashDone);
        // Finalise hash: ensure non-zero (same as OmScript map)
        llvm::Value* mfH0 = builder->CreateICmpEQ(mfHHash, mfZero, "mf.h0");
        llvm::Value* mfHashFinal = builder->CreateSelect(mfH0,
            llvm::ConstantInt::get(getDefaultType(), 1), mfHHash, "mf.hashfinal");
        // Probe new map: slot = hash % cap; linear probe for empty slot
        llvm::Value* mfSlot = builder->CreateURem(mfHashFinal, mfNewCap, "mf.slot");
        llvm::BasicBlock* mfProbePre  = builder->GetInsertBlock();
        llvm::BasicBlock* mfProbeLoop = llvm::BasicBlock::Create(*context, "mf.probe", mfParentFn);
        llvm::BasicBlock* mfWriteBB   = llvm::BasicBlock::Create(*context, "mf.write", mfParentFn);
        builder->CreateBr(mfProbeLoop);
        builder->SetInsertPoint(mfProbeLoop);
        llvm::PHINode* mfSI = builder->CreatePHI(getDefaultType(), 2, "mf.si");
        mfSI->addIncoming(mfSlot, mfProbePre);
        llvm::Value* mfBOff2 = builder->CreateAdd(
            builder->CreateMul(mfSI, llvm::ConstantInt::get(getDefaultType(), 3), "mf.s3"),
            llvm::ConstantInt::get(getDefaultType(), 2), "mf.soff");
        llvm::Value* mfSlotHash = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mfNewMapPtr, mfBOff2, "mf.shp"),
            llvm::MaybeAlign(8), "mf.shash");
        llvm::Value* mfEmpty = builder->CreateICmpEQ(mfSlotHash, mfZero, "mf.empty");
        builder->CreateCondBr(mfEmpty, mfWriteBB, mfProbeLoop);
        // Advance slot (linear probe)
        llvm::Value* mfSI1 = builder->CreateURem(
            builder->CreateAdd(mfSI, mfOne, "mf.si1a"),
            mfNewCap, "mf.sinext");
        mfSI->addIncoming(mfSI1, mfProbeLoop);

        builder->SetInsertPoint(mfWriteBB);
        // Write hash, key, val
        auto mfStore = [&](llvm::Value* base, llvm::Value* off, llvm::Value* val) {
            builder->CreateAlignedStore(val,
                builder->CreateInBoundsGEP(getDefaultType(), base, off, "mf.wp"),
                llvm::MaybeAlign(8));
        };
        mfStore(mfNewMapPtr, mfBOff2, mfHashFinal);
        mfStore(mfNewMapPtr,
            builder->CreateAdd(mfBOff2, mfOne, "mf.koff2"),
            mfKey);
        mfStore(mfNewMapPtr,
            builder->CreateAdd(mfBOff2, llvm::ConstantInt::get(getDefaultType(), 2), "mf.voff2"),
            mfVal);
        // Increment count (at offset 1)
        llvm::Value* mfCountPtr = builder->CreateInBoundsGEP(getDefaultType(), mfNewMapPtr,
            mfOne, "mf.cntp");
        llvm::Value* mfOldCount = builder->CreateAlignedLoad(getDefaultType(), mfCountPtr,
            llvm::MaybeAlign(8), "mf.ocnt");
        builder->CreateAlignedStore(
            builder->CreateAdd(mfOldCount, mfOne, "mf.ncnt"),
            mfCountPtr, llvm::MaybeAlign(8));
        builder->CreateBr(mfInc);

        builder->SetInsertPoint(mfInc);
        llvm::Value* mfBi1 = builder->CreateAdd(mfBi, mfOne, "mf.bi1", true, true);
        mfBi->addIncoming(mfBi1, mfInc);
        // Back-edge from mfAdd (not-kept path) and mfWriteBB (kept path)
        // Note: mfInc is reached from mfTest (not occupied), mfAdd (not kept),
        //       and mfWriteBB (kept + written) — all increment bi.
        // mfBi PHI already has (mfZero, mfPre) and (mfBi1, mfInc).
        // But mfAdd and mfWriteBB also need to reach mfInc.  They already branch to mfInc.
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(mfLoop)));

        builder->SetInsertPoint(mfDone);
        return builder->CreateAlignedLoad(getDefaultType(), mfNewMapA, llvm::MaybeAlign(8), "mf.result");
    }


    // -----------------------------------------------------------------------
    // filter(collection, fn)
    //   Generic filter that dispatches based on the argument type:
    //     - array: delegates to array_filter
    //     - string: delegates to str_filter
    //   The dispatch is static (compile-time type inference).
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::FILTER) {
        if (expr->arguments.size() != 2)
            codegenError("filter: expected 2 arguments (collection, predicate_fn_name)", expr);
        Expression* collArg = expr->arguments[0].get();
        // Determine collection type via the same static analysis used elsewhere.
        if (isStringExpr(collArg)) {
            // String: delegate to str_filter
            auto synth = std::make_unique<CallExpr>("str_filter",
                std::vector<std::unique_ptr<Expression>>{});
            synth->fromStdNamespace = true; // codegen-generated
            synth->arguments.push_back(std::move(expr->arguments[0]));
            synth->arguments.push_back(std::move(expr->arguments[1]));
            synth->line = expr->line; synth->column = expr->column;
            return generateCall(synth.get());
        } else {
            // Default: array filter
            auto synth = std::make_unique<CallExpr>("array_filter",
                std::vector<std::unique_ptr<Expression>>{});
            synth->fromStdNamespace = true; // codegen-generated
            synth->arguments.push_back(std::move(expr->arguments[0]));
            synth->arguments.push_back(std::move(expr->arguments[1]));
            synth->line = expr->line; synth->column = expr->column;
            return generateCall(synth.get());
        }
    }

    // -----------------------------------------------------------------------
    // str_lstrip(s) — strip leading whitespace
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_LSTRIP) {
        validateArgCount(expr, "str_lstrip", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
            ? strArg
            : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "lstrip.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "lstrip.len");
        nonNegValues_.insert(strLen);

        llvm::Function* lsParentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* lsPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* lsLoopBB = llvm::BasicBlock::Create(*context, "lstrip.loop", lsParentFn);
        llvm::BasicBlock* lsBodyBB = llvm::BasicBlock::Create(*context, "lstrip.body", lsParentFn);
        llvm::BasicBlock* lsContBB = llvm::BasicBlock::Create(*context, "lstrip.cont", lsParentFn);
        llvm::BasicBlock* lsDoneBB = llvm::BasicBlock::Create(*context, "lstrip.done", lsParentFn);
        llvm::Value* lsZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* lsOne  = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(lsLoopBB)));

        builder->SetInsertPoint(lsLoopBB);
        llvm::PHINode* lsIdx = builder->CreatePHI(getDefaultType(), 2, "lstrip.idx");
        lsIdx->addIncoming(lsZero, lsPreBB);
        builder->CreateCondBr(
            builder->CreateICmpULT(lsIdx, strLen, "lstrip.cond"), lsBodyBB, lsDoneBB);

        builder->SetInsertPoint(lsBodyBB);
        llvm::Value* lsCharPtr = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), strPtr, lsIdx, "lstrip.cp");
        auto* lsCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), lsCharPtr, "lstrip.ch");
        lsCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* lsCh32 = builder->CreateZExt(lsCharLoad, llvm::Type::getInt32Ty(*context), "lstrip.ch32");
        llvm::Value* lsIsSp = builder->CreateCall(getOrDeclareIsspace(), {lsCh32}, "lstrip.issp");
        builder->CreateCondBr(
            builder->CreateICmpNE(lsIsSp, builder->getInt32(0), "lstrip.spcond"),
            lsContBB, lsDoneBB);

        builder->SetInsertPoint(lsContBB);
        llvm::Value* lsNext = builder->CreateAdd(lsIdx, lsOne, "lstrip.next", true, true);
        lsIdx->addIncoming(lsNext, lsContBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(lsLoopBB)));

        builder->SetInsertPoint(lsDoneBB);
        llvm::PHINode* lsStart = builder->CreatePHI(getDefaultType(), 3, "lstrip.start");
        lsStart->addIncoming(lsIdx, lsLoopBB);
        lsStart->addIncoming(lsIdx, lsBodyBB);
        // Build result string from lsStart to end
        llvm::Value* lsResultLen = builder->CreateSub(strLen, lsStart, "lstrip.rlen", true, true);
        llvm::Value* lsAllocSz   = builder->CreateAdd(lsResultLen, lsOne, "lstrip.allocsz", true, true);
        llvm::Value* lsBuf       = builder->CreateCall(getOrDeclareMalloc(), {lsAllocSz}, "lstrip.buf");
        llvm::Value* lsSrc       = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), strPtr, lsStart, "lstrip.src");
        builder->CreateCall(getOrDeclareMemcpy(), {lsBuf, lsSrc, lsResultLen});
        llvm::Value* lsNulPtr = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), lsBuf, lsResultLen, "lstrip.nul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), lsNulPtr);
        stringReturningFunctions_.insert("str_lstrip");
        return builder->CreatePtrToInt(lsBuf, getDefaultType(), "lstrip.result");
    }

    // -----------------------------------------------------------------------
    // str_rstrip(s) — strip trailing whitespace
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_RSTRIP) {
        validateArgCount(expr, "str_rstrip", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
            ? strArg
            : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "rstrip.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "rstrip.len");
        nonNegValues_.insert(strLen);

        llvm::Function* rsParentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* rsPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* rsLoopBB = llvm::BasicBlock::Create(*context, "rstrip.loop", rsParentFn);
        llvm::BasicBlock* rsBodyBB = llvm::BasicBlock::Create(*context, "rstrip.body", rsParentFn);
        llvm::BasicBlock* rsContBB = llvm::BasicBlock::Create(*context, "rstrip.cont", rsParentFn);
        llvm::BasicBlock* rsDoneBB = llvm::BasicBlock::Create(*context, "rstrip.done", rsParentFn);
        llvm::Value* rsZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* rsOne  = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(rsLoopBB)));

        builder->SetInsertPoint(rsLoopBB);
        llvm::PHINode* rsEnd = builder->CreatePHI(getDefaultType(), 2, "rstrip.end");
        rsEnd->addIncoming(strLen, rsPreBB);
        builder->CreateCondBr(
            builder->CreateICmpUGT(rsEnd, rsZero, "rstrip.cond"), rsBodyBB, rsDoneBB);

        builder->SetInsertPoint(rsBodyBB);
        llvm::Value* rsPrev = builder->CreateSub(rsEnd, rsOne, "rstrip.prev", true, true);
        llvm::Value* rsCharPtr = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), strPtr, rsPrev, "rstrip.cp");
        auto* rsCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), rsCharPtr, "rstrip.ch");
        rsCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* rsCh32 = builder->CreateZExt(rsCharLoad, llvm::Type::getInt32Ty(*context), "rstrip.ch32");
        llvm::Value* rsIsSp = builder->CreateCall(getOrDeclareIsspace(), {rsCh32}, "rstrip.issp");
        builder->CreateCondBr(
            builder->CreateICmpNE(rsIsSp, builder->getInt32(0), "rstrip.spcond"),
            rsContBB, rsDoneBB);

        builder->SetInsertPoint(rsContBB);
        rsEnd->addIncoming(rsPrev, rsContBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(rsLoopBB)));

        builder->SetInsertPoint(rsDoneBB);
        llvm::PHINode* rsFinalEnd = builder->CreatePHI(getDefaultType(), 3, "rstrip.finalend");
        rsFinalEnd->addIncoming(rsEnd, rsLoopBB);
        rsFinalEnd->addIncoming(rsEnd, rsBodyBB);
        // Build result: strPtr[0..rsFinalEnd)
        llvm::Value* rsAllocSz = builder->CreateAdd(rsFinalEnd, rsOne, "rstrip.allocsz", true, true);
        llvm::Value* rsBuf     = builder->CreateCall(getOrDeclareMalloc(), {rsAllocSz}, "rstrip.buf");
        builder->CreateCall(getOrDeclareMemcpy(), {rsBuf, strPtr, rsFinalEnd});
        llvm::Value* rsNulPtr  = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), rsBuf, rsFinalEnd, "rstrip.nul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), rsNulPtr);
        stringReturningFunctions_.insert("str_rstrip");
        return builder->CreatePtrToInt(rsBuf, getDefaultType(), "rstrip.result");
    }

    // -----------------------------------------------------------------------
    // str_remove(s, sub) — remove all occurrences of sub from s
    //   Equivalent to str_replace(s, sub, "") but with a clearer name.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_REMOVE) {
        validateArgCount(expr, "str_remove", 2);
        // Synthesize str_replace(s, sub, "")
        auto srEmptyLit = std::make_unique<LiteralExpr>(std::string(""));
        srEmptyLit->line   = expr->line;
        srEmptyLit->column = expr->column;
        auto srSynth = std::make_unique<CallExpr>("str_replace",
            std::vector<std::unique_ptr<Expression>>{});
        srSynth->fromStdNamespace = true; // codegen-generated
        srSynth->arguments.push_back(std::move(expr->arguments[0]));
        srSynth->arguments.push_back(std::move(expr->arguments[1]));
        srSynth->arguments.push_back(std::move(srEmptyLit));
        srSynth->line = expr->line; srSynth->column = expr->column;
        llvm::Value* srResult = generateCall(srSynth.get());
        stringReturningFunctions_.insert("str_remove");
        return srResult;
    }

    // -----------------------------------------------------------------------
    // array_take(arr, n) — first n elements (clamped to array length)
    //   Equivalent to array_slice(arr, 0, n).
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_TAKE) {
        validateArgCount(expr, "array_take", 2);
        auto atZeroLit = std::make_unique<LiteralExpr>(0LL);
        atZeroLit->line = expr->line; atZeroLit->column = expr->column;
        auto atSynth = std::make_unique<CallExpr>("array_slice",
            std::vector<std::unique_ptr<Expression>>{});
        atSynth->fromStdNamespace = true; // codegen-generated
        atSynth->arguments.push_back(std::move(expr->arguments[0]));
        atSynth->arguments.push_back(std::move(atZeroLit));
        atSynth->arguments.push_back(std::move(expr->arguments[1]));
        atSynth->line = expr->line; atSynth->column = expr->column;
        return generateCall(atSynth.get());
    }

    // -----------------------------------------------------------------------
    // array_drop(arr, n) — all elements after the first n
    //   Equivalent to array_slice(arr, n, len(arr)).
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_DROP) {
        validateArgCount(expr, "array_drop", 2);
        // We need len(arr) as the end argument.  Evaluate arr first, store in alloca.
        llvm::Value* adArr = generateExpression(expr->arguments[0].get());
        adArr = toDefaultType(adArr);
        llvm::Value* adN   = generateExpression(expr->arguments[1].get());
        adN = toDefaultType(adN);
        auto* adPtrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* adArrPtr = builder->CreateIntToPtr(adArr, adPtrTy, "adrop.arrptr");
                llvm::Value* adLenLoad = emitLoadArrayLen(adArrPtr, "adrop.len");
        // Clamp n: max(0, min(n, len))
        llvm::Value* adZero    = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* adNNeg    = builder->CreateICmpSLT(adN, adZero, "adrop.nneg");
        llvm::Value* adNClamp  = builder->CreateSelect(adNNeg, adZero, adN, "adrop.nclamp");
        llvm::Value* adNOver   = builder->CreateICmpSGT(adNClamp, adLenLoad, "adrop.nover");
        llvm::Value* adStart   = builder->CreateSelect(adNOver, adLenLoad, adNClamp, "adrop.start");
        llvm::Value* adSliceLen = builder->CreateSub(adLenLoad, adStart, "adrop.slen", true, true);
        // Allocate result
        llvm::Value* adOne    = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* adEight  = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* adSlots  = builder->CreateAdd(adSliceLen, adOne, "adrop.slots", true, true);
        llvm::Value* adBytes  = builder->CreateMul(adSlots, adEight, "adrop.bytes", true, true);
        llvm::Value* adBuf    = builder->CreateCall(getOrDeclareMalloc(), {adBytes}, "adrop.buf");
        llvm::cast<llvm::CallInst>(adBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        emitStoreArrayLen(adSliceLen, adBuf);
        llvm::Value* adSrcIdx = builder->CreateAdd(adStart, adOne, "adrop.srcidx", true, true);
        llvm::Value* adSrc    = builder->CreateInBoundsGEP(getDefaultType(), adArrPtr, adSrcIdx, "adrop.src");
        llvm::Value* adDst    = builder->CreateInBoundsGEP(getDefaultType(), adBuf, adOne, "adrop.dst");
        llvm::Value* adCpSz   = builder->CreateMul(adSliceLen, adEight, "adrop.cpsz", true, true);
        builder->CreateCall(getOrDeclareMemcpy(), {adDst, adSrc, adCpSz});
        return builder->CreatePtrToInt(adBuf, getDefaultType(), "adrop.result");
    }

    // -----------------------------------------------------------------------
    // array_unique(arr) — deduplicate consecutive equal elements (like Unix uniq)
    //   For a sorted array this removes all duplicates.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_UNIQUE) {
        validateArgCount(expr, "array_unique", 1);
        llvm::Value* auArr = generateExpression(expr->arguments[0].get());
                llvm::Value* auArrPtr = emitToArrayPtr(auArr, "auniq.arrptr");
                llvm::Value* auLenLoad = emitLoadArrayLen(auArrPtr, "auniq.len");
        llvm::Value* auZero  = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* auOne   = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* auEight = llvm::ConstantInt::get(getDefaultType(), 8);
        // Allocate output buffer same size as input (worst case: all unique)
        llvm::Value* auSlots = builder->CreateAdd(auLenLoad, auOne, "auniq.slots", true, true);
        llvm::Value* auBytes = builder->CreateMul(auSlots, auEight, "auniq.bytes", true, true);
        llvm::Value* auBuf   = builder->CreateCall(getOrDeclareMalloc(), {auBytes}, "auniq.buf");
        llvm::cast<llvm::CallInst>(auBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        // outLen alloca (count of unique elements written)
        llvm::Function* auParentFn = builder->GetInsertBlock()->getParent();
        llvm::AllocaInst* auOutLenA = createEntryBlockAlloca(auParentFn, "auniq.outlen", getDefaultType());
        builder->CreateStore(auZero, auOutLenA);

        // Loop: for i in 0..arrLen { if i==0 or arr[i]!=arr[i-1]: write }
        llvm::BasicBlock* auPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* auLoopBB = llvm::BasicBlock::Create(*context, "auniq.loop", auParentFn);
        llvm::BasicBlock* auBodyBB = llvm::BasicBlock::Create(*context, "auniq.body", auParentFn);
        llvm::BasicBlock* auDedBB  = llvm::BasicBlock::Create(*context, "auniq.ded",  auParentFn);
        llvm::BasicBlock* auIncBB  = llvm::BasicBlock::Create(*context, "auniq.inc",  auParentFn);
        llvm::BasicBlock* auDoneBB = llvm::BasicBlock::Create(*context, "auniq.done", auParentFn);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(auLoopBB)));

        builder->SetInsertPoint(auLoopBB);
        llvm::PHINode* auI = builder->CreatePHI(getDefaultType(), 2, "auniq.i");
        auI->addIncoming(auZero, auPreBB);
        builder->CreateCondBr(builder->CreateICmpULT(auI, auLenLoad, "auniq.cond"), auBodyBB, auDoneBB);

        builder->SetInsertPoint(auBodyBB);
        llvm::Value* auElemIdx  = builder->CreateAdd(auI, auOne, "auniq.ei", true, true);
        llvm::Value* auElemPtr  = builder->CreateInBoundsGEP(getDefaultType(), auArrPtr, auElemIdx, "auniq.ep");
                llvm::Value* auElemLoad = emitLoadArrayElem(auElemPtr, "auniq.elem");
        // Check: is this the first element (i==0) or different from the previous?
        llvm::Value* auIsFirst = builder->CreateICmpEQ(auI, auZero, "auniq.isfirst");
        llvm::Value* auPrevIdx = builder->CreateSub(auI, auOne, "auniq.prevei");
        // auPrevIdx is only valid when i>0; PHI select doesn't evaluate
        llvm::Value* auPrevElemIdx = builder->CreateAdd(auPrevIdx, auOne, "auniq.pei", true, true);
        llvm::Value* auPrevElemPtr = builder->CreateInBoundsGEP(getDefaultType(), auArrPtr, auPrevElemIdx, "auniq.pep");
                llvm::Value* auPrevLoad = emitLoadArrayElem(auPrevElemPtr, "auniq.prev");
        llvm::Value* auDiff = builder->CreateICmpNE(auElemLoad, auPrevLoad, "auniq.diff");
        llvm::Value* auKeep = builder->CreateOr(auIsFirst, auDiff, "auniq.keep");
        builder->CreateCondBr(auKeep, auDedBB, auIncBB);

        builder->SetInsertPoint(auDedBB);
        llvm::Value* auOutLen  = builder->CreateAlignedLoad(getDefaultType(), auOutLenA, llvm::MaybeAlign(8), "auniq.ol");
        llvm::Value* auDstIdx  = builder->CreateAdd(auOutLen, auOne, "auniq.di", true, true);
        llvm::Value* auDstPtr  = builder->CreateInBoundsGEP(getDefaultType(), auBuf, auDstIdx, "auniq.dp");
        emitStoreArrayElem(auElemLoad, auDstPtr);
        builder->CreateStore(
            builder->CreateAdd(auOutLen, auOne, "auniq.ol1", true, true), auOutLenA);
        builder->CreateBr(auIncBB);

        builder->SetInsertPoint(auIncBB);
        llvm::Value* auI1 = builder->CreateAdd(auI, auOne, "auniq.i1", true, true);
        auI->addIncoming(auI1, auIncBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(auLoopBB)));

        builder->SetInsertPoint(auDoneBB);
        llvm::Value* auFinalLen = builder->CreateAlignedLoad(getDefaultType(), auOutLenA, llvm::MaybeAlign(8), "auniq.fl");
        emitStoreArrayLen(auFinalLen, auBuf);
        return builder->CreatePtrToInt(auBuf, getDefaultType(), "auniq.result");
    }

    // -----------------------------------------------------------------------
    // array_rotate(arr, n) — rotate array left by n positions
    //   Positive n: rotate left (elements shift towards index 0).
    //   Negative n: rotate right.
    //   Handles n outside [-len, len] via modulo.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_ROTATE) {
        validateArgCount(expr, "array_rotate", 2);
        llvm::Value* arArr = generateExpression(expr->arguments[0].get());
        llvm::Value* arN   = generateExpression(expr->arguments[1].get());
        arArr = toDefaultType(arArr);
        arN   = toDefaultType(arN);
        auto* arPtrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* arArrPtr = builder->CreateIntToPtr(arArr, arPtrTy, "arot.arrptr");
                llvm::Value* arLenLoad = emitLoadArrayLen(arArrPtr, "arot.len");
        llvm::Value* arZero  = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* arOne   = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* arEight = llvm::ConstantInt::get(getDefaultType(), 8);
        // Allocate result (same size)
        llvm::Value* arSlots = builder->CreateAdd(arLenLoad, arOne, "arot.slots", true, true);
        llvm::Value* arBytes = builder->CreateMul(arSlots, arEight, "arot.bytes", true, true);
        llvm::Value* arBuf   = builder->CreateCall(getOrDeclareMalloc(), {arBytes}, "arot.buf");
        llvm::cast<llvm::CallInst>(arBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        emitStoreArrayLen(arLenLoad, arBuf);
        // Handle empty array
        llvm::Function* arParentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* arPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* arDoBB   = llvm::BasicBlock::Create(*context, "arot.do",   arParentFn);
        llvm::BasicBlock* arLoopBB = llvm::BasicBlock::Create(*context, "arot.loop", arParentFn);
        llvm::BasicBlock* arBodyBB = llvm::BasicBlock::Create(*context, "arot.body", arParentFn);
        llvm::BasicBlock* arDoneBB = llvm::BasicBlock::Create(*context, "arot.done", arParentFn);
        llvm::Value* arEmpty = builder->CreateICmpEQ(arLenLoad, arZero, "arot.empty");
        builder->CreateCondBr(arEmpty, arDoneBB, arDoBB);

        builder->SetInsertPoint(arDoBB);
        // Normalize shift: k = ((n % len) + len) % len
        llvm::Value* arMod1 = builder->CreateSRem(arN, arLenLoad, "arot.mod1");
        llvm::Value* arMod2 = builder->CreateAdd(arMod1, arLenLoad, "arot.mod2");
        llvm::Value* arK    = builder->CreateSRem(arMod2, arLenLoad, "arot.k");
        nonNegValues_.insert(arK);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(arLoopBB)));

        builder->SetInsertPoint(arLoopBB);
        llvm::PHINode* arI = builder->CreatePHI(getDefaultType(), 2, "arot.i");
        arI->addIncoming(arZero, arDoBB);
        builder->CreateCondBr(builder->CreateICmpULT(arI, arLenLoad, "arot.cond"), arBodyBB, arDoneBB);

        builder->SetInsertPoint(arBodyBB);
        // src index = (i + k) % len
        llvm::Value* arSrcI   = builder->CreateURem(
            builder->CreateAdd(arI, arK, "arot.ik"), arLenLoad, "arot.srci");
        llvm::Value* arSrcIdx = builder->CreateAdd(arSrcI, arOne, "arot.srcidx", true, true);
        llvm::Value* arSrcPtr = builder->CreateInBoundsGEP(getDefaultType(), arArrPtr, arSrcIdx, "arot.sp");
                llvm::Value* arSrcLoad = emitLoadArrayElem(arSrcPtr, "arot.sv");
        llvm::Value* arDstIdx = builder->CreateAdd(arI, arOne, "arot.dstidx", true, true);
        llvm::Value* arDstPtr = builder->CreateInBoundsGEP(getDefaultType(), arBuf, arDstIdx, "arot.dp");
        emitStoreArrayElem(arSrcLoad, arDstPtr);
        llvm::Value* arI1 = builder->CreateAdd(arI, arOne, "arot.i1", true, true);
        arI->addIncoming(arI1, arBodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(arLoopBB)));

        builder->SetInsertPoint(arDoneBB);
        return builder->CreatePtrToInt(arBuf, getDefaultType(), "arot.result");
    }

    // -----------------------------------------------------------------------
    // array_mean(arr) — arithmetic mean of integer elements
    //   Returns sum / len (integer division), or 0 for an empty array.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_MEAN) {
        validateArgCount(expr, "array_mean", 1);
        // Constant-fold array_mean([c0, c1, ...]) when the array is a compile-time literal.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                bool allInt = true;
                for (const auto& elem : cv->arrVal) {
                    if (elem.kind != ConstValue::Kind::Integer) { allInt = false; break; }
                }
                if (allInt) {
                    optStats_.constFolded++;
                    if (cv->arrVal.empty())
                        return llvm::ConstantInt::get(getDefaultType(), 0);
                    int64_t total = 0;
                    for (const auto& elem : cv->arrVal)
                        total += elem.intVal;
                    return llvm::ConstantInt::get(getDefaultType(),
                        total / static_cast<int64_t>(cv->arrVal.size()));
                }
            }
        }
        // Constant-fold array_mean(array_fill(n, v)) → v when n > 0 (mean of n identical values)
        if (auto* call = dynamic_cast<CallExpr*>(expr->arguments[0].get())) {
            if (call->callee == "array_fill" && call->arguments.size() == 2) {
                if (auto n = tryFoldInt(call->arguments[0].get())) {
                    if (auto v = tryFoldInt(call->arguments[1].get())) {
                        if (*n > 0) {
                            optStats_.constFolded++;
                            return llvm::ConstantInt::get(getDefaultType(), *v);
                        }
                        if (*n == 0) {
                            optStats_.constFolded++;
                            return llvm::ConstantInt::get(getDefaultType(), 0);
                        }
                    }
                }
            }
        }
        llvm::Value* amArr = generateExpression(expr->arguments[0].get());
                llvm::Value* amArrPtr = emitToArrayPtr(amArr, "amean.arrptr");
                llvm::Value* amLenLoad = emitLoadArrayLen(amArrPtr, "amean.len");
        llvm::Value* amZero  = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* amOne   = llvm::ConstantInt::get(getDefaultType(), 1);
        // If empty, return 0
        llvm::Function* amParentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* amPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* amLoopBB = llvm::BasicBlock::Create(*context, "amean.loop", amParentFn);
        llvm::BasicBlock* amBodyBB = llvm::BasicBlock::Create(*context, "amean.body", amParentFn);
        llvm::BasicBlock* amDoneBB = llvm::BasicBlock::Create(*context, "amean.done", amParentFn);
        llvm::AllocaInst* amSumA   = createEntryBlockAlloca(amParentFn, "amean.sum", getDefaultType());
        builder->CreateStore(amZero, amSumA);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(amLoopBB)));

        builder->SetInsertPoint(amLoopBB);
        llvm::PHINode* amI = builder->CreatePHI(getDefaultType(), 2, "amean.i");
        amI->addIncoming(amZero, amPreBB);
        builder->CreateCondBr(builder->CreateICmpULT(amI, amLenLoad, "amean.cond"), amBodyBB, amDoneBB);

        builder->SetInsertPoint(amBodyBB);
        llvm::Value* amElemIdx = builder->CreateAdd(amI, amOne, "amean.ei", true, true);
        llvm::Value* amElemPtr = builder->CreateInBoundsGEP(getDefaultType(), amArrPtr, amElemIdx, "amean.ep");
                llvm::Value* amElemLoad = emitLoadArrayElem(amElemPtr, "amean.elem");
        llvm::Value* amOldSum = builder->CreateAlignedLoad(getDefaultType(), amSumA, llvm::MaybeAlign(8), "amean.os");
        builder->CreateStore(builder->CreateAdd(amOldSum, amElemLoad, "amean.ns"), amSumA);
        llvm::Value* amI1 = builder->CreateAdd(amI, amOne, "amean.i1", true, true);
        amI->addIncoming(amI1, amBodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(amLoopBB)));

        builder->SetInsertPoint(amDoneBB);
        llvm::Value* amFinalSum = builder->CreateAlignedLoad(getDefaultType(), amSumA, llvm::MaybeAlign(8), "amean.fs");
        // Return sum / len (sdiv), or 0 when len == 0
        llvm::Value* amIsEmpty  = builder->CreateICmpEQ(amLenLoad, amZero, "amean.isempty");
        llvm::Value* amDiv      = builder->CreateSDiv(amFinalSum, amLenLoad, "amean.div");
        return builder->CreateSelect(amIsEmpty, amZero, amDiv, "amean.result");
    }

    // -----------------------------------------------------------------------
    // map_merge(a, b) — create a new map with all entries from a and b.
    //   When a key exists in both, b's value wins.
    //   Neither a nor b is mutated.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::MAP_MERGE) {
        validateArgCount(expr, "map_merge", 2);
        llvm::Value* mmA = generateExpression(expr->arguments[0].get());
        llvm::Value* mmB = generateExpression(expr->arguments[1].get());
        mmA = toDefaultType(mmA); mmB = toDefaultType(mmB);
        auto* mmPtrTy = llvm::PointerType::getUnqual(*context);
        // Create new result map (copy of a)
        llvm::Value* mmAPtr  = builder->CreateIntToPtr(mmA, mmPtrTy, "mmerge.aptr");
        llvm::Value* mmBPtr  = builder->CreateIntToPtr(mmB, mmPtrTy, "mmerge.bptr");
        // result = map_new(); then insert all of a, then all of b (b wins)
        llvm::Value* mmRes = builder->CreateCall(getOrEmitHashMapNew(), {}, "mmerge.res");
        llvm::AllocaInst* mmResA = createEntryBlockAlloca(
            builder->GetInsertBlock()->getParent(), "mmerge.resmap", mmPtrTy);
        builder->CreateStore(mmRes, mmResA);

        // Helper lambda: iterate a map's buckets and insert into result
        auto mmInsertAll = [&](llvm::Value* srcMapPtr, const char* pfx) {
            // Read cap from offset 0
            llvm::Value* mmCap = builder->CreateAlignedLoad(getDefaultType(), srcMapPtr,
                llvm::MaybeAlign(8), (std::string(pfx)+".cap").c_str());
            llvm::Value* mmZero = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* mmOne  = llvm::ConstantInt::get(getDefaultType(), 1);
            llvm::Function* mmFn = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock* mmPreBB  = builder->GetInsertBlock();
            llvm::BasicBlock* mmLoopBB = llvm::BasicBlock::Create(*context,
                (std::string(pfx)+".loop").c_str(), mmFn);
            llvm::BasicBlock* mmTestBB = llvm::BasicBlock::Create(*context,
                (std::string(pfx)+".test").c_str(), mmFn);
            llvm::BasicBlock* mmInsB   = llvm::BasicBlock::Create(*context,
                (std::string(pfx)+".ins").c_str(),  mmFn);
            llvm::BasicBlock* mmIncBB  = llvm::BasicBlock::Create(*context,
                (std::string(pfx)+".inc").c_str(),  mmFn);
            llvm::BasicBlock* mmDoneBB = llvm::BasicBlock::Create(*context,
                (std::string(pfx)+".done").c_str(), mmFn);
            attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(mmLoopBB)));
            builder->SetInsertPoint(mmLoopBB);
            llvm::PHINode* mmBi = builder->CreatePHI(getDefaultType(), 2, (std::string(pfx)+".bi").c_str());
            mmBi->addIncoming(mmZero, mmPreBB);
            builder->CreateCondBr(builder->CreateICmpULT(mmBi, mmCap), mmTestBB, mmDoneBB);
            builder->SetInsertPoint(mmTestBB);
            // bucket offset = 2 + bi*3
            llvm::Value* mmBoff = builder->CreateAdd(
                builder->CreateMul(mmBi, llvm::ConstantInt::get(getDefaultType(), 3)),
                llvm::ConstantInt::get(getDefaultType(), 2));
            auto* mmHashV = builder->CreateAlignedLoad(getDefaultType(),
                builder->CreateInBoundsGEP(getDefaultType(), srcMapPtr, mmBoff),
                llvm::MaybeAlign(8));
            mmHashV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
            llvm::Value* mmOcc = builder->CreateICmpNE(mmHashV, mmZero);
            builder->CreateCondBr(mmOcc, mmInsB, mmIncBB);
            builder->SetInsertPoint(mmInsB);
            auto* mmKeyV = builder->CreateAlignedLoad(getDefaultType(),
                builder->CreateInBoundsGEP(getDefaultType(), srcMapPtr,
                    builder->CreateAdd(mmBoff, mmOne)),
                llvm::MaybeAlign(8));
            mmKeyV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
            auto* mmValV = builder->CreateAlignedLoad(getDefaultType(),
                builder->CreateInBoundsGEP(getDefaultType(), srcMapPtr,
                    builder->CreateAdd(mmBoff, llvm::ConstantInt::get(getDefaultType(), 2))),
                llvm::MaybeAlign(8));
            mmValV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
            llvm::Value* mmCurRes = builder->CreateAlignedLoad(mmPtrTy, mmResA, llvm::MaybeAlign(8));
            llvm::Value* mmNewRes = builder->CreateCall(getOrEmitHashMapSet(),
                {mmCurRes, mmKeyV, mmValV});
            builder->CreateStore(mmNewRes, mmResA);
            builder->CreateBr(mmIncBB);
            builder->SetInsertPoint(mmIncBB);
            llvm::Value* mmBi1 = builder->CreateAdd(mmBi, mmOne, "", true, true);
            mmBi->addIncoming(mmBi1, mmIncBB);
            attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(mmLoopBB)));
            builder->SetInsertPoint(mmDoneBB);
        };

        mmInsertAll(mmAPtr, "mmerge.a");
        mmInsertAll(mmBPtr, "mmerge.b");

        llvm::Value* mmFinalRes = builder->CreateAlignedLoad(mmPtrTy, mmResA, llvm::MaybeAlign(8), "mmerge.final");
        return builder->CreatePtrToInt(mmFinalRes, getDefaultType(), "mmerge.result");
    }

    // -----------------------------------------------------------------------
    // map_invert(m) — create a new map with keys and values swapped.
    //   If multiple keys map to the same value, the last one (bucket order)
    //   wins for the corresponding key in the inverted map.
    //   Values must be strings (i64 string pointers) to be used as keys.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::MAP_INVERT) {
        validateArgCount(expr, "map_invert", 1);
        llvm::Value* miM = generateExpression(expr->arguments[0].get());
                llvm::Value* miMPtr = emitToArrayPtr(miM, "minv.mptr");
        auto* miPtrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* miCap  = builder->CreateAlignedLoad(getDefaultType(), miMPtr,
            llvm::MaybeAlign(8), "minv.cap");
        llvm::Value* miZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* miOne  = llvm::ConstantInt::get(getDefaultType(), 1);
        // Create result map
        llvm::Value* miRes = builder->CreateCall(getOrEmitHashMapNew(), {}, "minv.res");
        llvm::AllocaInst* miResA = createEntryBlockAlloca(
            builder->GetInsertBlock()->getParent(), "minv.resmap", miPtrTy);
        builder->CreateStore(miRes, miResA);

        llvm::Function* miFn   = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* miPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* miLoopBB = llvm::BasicBlock::Create(*context, "minv.loop", miFn);
        llvm::BasicBlock* miTestBB = llvm::BasicBlock::Create(*context, "minv.test", miFn);
        llvm::BasicBlock* miInsB   = llvm::BasicBlock::Create(*context, "minv.ins",  miFn);
        llvm::BasicBlock* miIncBB  = llvm::BasicBlock::Create(*context, "minv.inc",  miFn);
        llvm::BasicBlock* miDoneBB = llvm::BasicBlock::Create(*context, "minv.done", miFn);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(miLoopBB)));

        builder->SetInsertPoint(miLoopBB);
        llvm::PHINode* miBi = builder->CreatePHI(getDefaultType(), 2, "minv.bi");
        miBi->addIncoming(miZero, miPreBB);
        builder->CreateCondBr(builder->CreateICmpULT(miBi, miCap), miTestBB, miDoneBB);

        builder->SetInsertPoint(miTestBB);
        llvm::Value* miBoff = builder->CreateAdd(
            builder->CreateMul(miBi, llvm::ConstantInt::get(getDefaultType(), 3)),
            llvm::ConstantInt::get(getDefaultType(), 2));
        auto* miHashV = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), miMPtr, miBoff),
            llvm::MaybeAlign(8));
        miHashV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
        builder->CreateCondBr(builder->CreateICmpNE(miHashV, miZero), miInsB, miIncBB);

        builder->SetInsertPoint(miInsB);
        auto* miKeyV = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), miMPtr, builder->CreateAdd(miBoff, miOne)),
            llvm::MaybeAlign(8));
        miKeyV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
        auto* miValV = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), miMPtr,
                builder->CreateAdd(miBoff, llvm::ConstantInt::get(getDefaultType(), 2))),
            llvm::MaybeAlign(8));
        miValV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
        // Invert: new key = old value, new value = old key
        llvm::Value* miCurRes = builder->CreateAlignedLoad(miPtrTy, miResA, llvm::MaybeAlign(8));
        llvm::Value* miNewRes = builder->CreateCall(getOrEmitHashMapSet(),
            {miCurRes, miValV, miKeyV});
        builder->CreateStore(miNewRes, miResA);
        builder->CreateBr(miIncBB);

        builder->SetInsertPoint(miIncBB);
        llvm::Value* miBi1 = builder->CreateAdd(miBi, miOne, "minv.bi1", true, true);
        miBi->addIncoming(miBi1, miIncBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(miLoopBB)));

        builder->SetInsertPoint(miDoneBB);
        llvm::Value* miFinalRes = builder->CreateAlignedLoad(miPtrTy, miResA, llvm::MaybeAlign(8), "minv.final");
        return builder->CreatePtrToInt(miFinalRes, getDefaultType(), "minv.result");
    }

    // -----------------------------------------------------------------------
    // sudo_command(cmd, password) — run shell command via sudo, providing
    //   the password on stdin using `sudo -S`.
    //
    //   The command is run as:
    //     printf '%s\n' 'ESCAPED_PASS' | sudo -S -- sh -c 'CMD' 2>&1
    //
    //   Single-quote characters in the password are escaped as `'\''` so that
    //   the printf argument is always a valid single-quoted shell word.
    //   The command string is passed as-is to `sh -c` (same as `command`).
    //   Returns the combined stdout+stderr of the subprocess as a string,
    //   or an empty string if popen() fails.
    //
    //   Security note: the password is never passed as a process argument —
    //   it travels only through the pipe from printf to sudo's stdin.
    //   However the full pipeline string is visible in /proc on some systems;
    //   prefer passwordless sudo rules (NOPASSWD) for production use.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::SUDO_COMMAND) {
        validateArgCount(expr, "sudo_command", 2);
        llvm::Value* scCmdArg  = generateExpression(expr->arguments[0].get());
        llvm::Value* scPassArg = generateExpression(expr->arguments[1].get());
        auto* scPtrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* scCmdPtr  = scCmdArg->getType()->isPointerTy()
            ? scCmdArg
            : builder->CreateIntToPtr(scCmdArg, scPtrTy, "sudo.cmdptr");
        llvm::Value* scPassPtr = scPassArg->getType()->isPointerTy()
            ? scPassArg
            : builder->CreateIntToPtr(scPassArg, scPtrTy, "sudo.passptr");

        llvm::Function* scParentFn = builder->GetInsertBlock()->getParent();

        // ---- Step 1: escape single-quotes in the password ----
        // Escaped form: each `'` → `'\''`.  Worst case: 4x expansion.
        llvm::Value* scPassLen = builder->CreateCall(getOrDeclareStrlen(), {scPassPtr}, "sudo.passlen");
        nonNegValues_.insert(scPassLen);
        llvm::Value* scEscMax  = builder->CreateAdd(
            builder->CreateMul(scPassLen, llvm::ConstantInt::get(getDefaultType(), 4), "sudo.escmax4"),
            llvm::ConstantInt::get(getDefaultType(), 1), "sudo.escmax", true, true);
        llvm::Value* scEscBuf  = builder->CreateCall(getOrDeclareMalloc(), {scEscMax}, "sudo.escbuf");
        llvm::AllocaInst* scEscWrA = createEntryBlockAlloca(scParentFn, "sudo.escwr", getDefaultType());
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), scEscWrA);

        llvm::BasicBlock* scEscPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* scEscLoopBB = llvm::BasicBlock::Create(*context, "sudo.escloop", scParentFn);
        llvm::BasicBlock* scEscBodyBB = llvm::BasicBlock::Create(*context, "sudo.escbody", scParentFn);
        llvm::BasicBlock* scEscSqBB   = llvm::BasicBlock::Create(*context, "sudo.escsq",   scParentFn);
        llvm::BasicBlock* scEscNormBB = llvm::BasicBlock::Create(*context, "sudo.escnorm",  scParentFn);
        llvm::BasicBlock* scEscIncBB  = llvm::BasicBlock::Create(*context, "sudo.escinc",   scParentFn);
        llvm::BasicBlock* scEscDoneBB = llvm::BasicBlock::Create(*context, "sudo.escdone",  scParentFn);
        llvm::Value* scEscZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* scEscOne  = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(scEscLoopBB)));

        builder->SetInsertPoint(scEscLoopBB);
        llvm::PHINode* scEscI = builder->CreatePHI(getDefaultType(), 2, "sudo.esci");
        scEscI->addIncoming(scEscZero, scEscPreBB);
        builder->CreateCondBr(builder->CreateICmpULT(scEscI, scPassLen, "sudo.esccond"),
            scEscBodyBB, scEscDoneBB);

        builder->SetInsertPoint(scEscBodyBB);
        llvm::Value* scCharPtr  = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), scPassPtr, scEscI, "sudo.charptr");
        auto* scCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), scCharPtr, "sudo.char");
        scCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* scIsSq = builder->CreateICmpEQ(
            scCharLoad, llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), '\''), "sudo.issq");
        builder->CreateCondBr(scIsSq, scEscSqBB, scEscNormBB);

        // Single-quote path: emit  ' \ ' '  (4 bytes)
        builder->SetInsertPoint(scEscSqBB);
        llvm::Value* scWrSq  = builder->CreateAlignedLoad(getDefaultType(), scEscWrA, llvm::MaybeAlign(8), "sudo.wrsq");
        auto scEmitByte = [&](llvm::Value* wrIdx, uint8_t byte) -> llvm::Value* {
            llvm::Value* p = builder->CreateInBoundsGEP(
                llvm::Type::getInt8Ty(*context), scEscBuf, wrIdx, "sudo.ep");
            builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), byte), p);
            return builder->CreateAdd(wrIdx, scEscOne, "sudo.wr1", true, true);
        };
        llvm::Value* scWrSq1 = scEmitByte(scWrSq,  '\'');
        llvm::Value* scWrSq2 = scEmitByte(scWrSq1, '\\');
        llvm::Value* scWrSq3 = scEmitByte(scWrSq2, '\'');
        llvm::Value* scWrSq4 = scEmitByte(scWrSq3, '\'');
        builder->CreateStore(scWrSq4, scEscWrA);
        builder->CreateBr(scEscIncBB);

        // Normal path: emit byte as-is
        builder->SetInsertPoint(scEscNormBB);
        llvm::Value* scWrNorm  = builder->CreateAlignedLoad(getDefaultType(), scEscWrA, llvm::MaybeAlign(8), "sudo.wrnorm");
        {
            llvm::Value* scNP = builder->CreateInBoundsGEP(
                llvm::Type::getInt8Ty(*context), scEscBuf, scWrNorm, "sudo.np");
            builder->CreateStore(scCharLoad, scNP);
            llvm::Value* scWrNorm2 = builder->CreateAdd(scWrNorm, scEscOne, "sudo.wrnorm2", true, true);
            builder->CreateStore(scWrNorm2, scEscWrA);
        }
        builder->CreateBr(scEscIncBB);

        builder->SetInsertPoint(scEscIncBB);
        llvm::Value* scEscI1 = builder->CreateAdd(scEscI, scEscOne, "sudo.esci1", true, true);
        scEscI->addIncoming(scEscI1, scEscIncBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(scEscLoopBB)));

        builder->SetInsertPoint(scEscDoneBB);
        // NUL-terminate escaped password
        llvm::Value* scEscWrFinal = builder->CreateAlignedLoad(getDefaultType(), scEscWrA, llvm::MaybeAlign(8), "sudo.escfin");
        llvm::Value* scEscNulPtr  = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), scEscBuf, scEscWrFinal, "sudo.escnul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), scEscNulPtr);

        // ---- Step 2: build full pipeline command string ----
        // Format: printf '%%s\n' 'ESCAPED_PASS' | sudo -S -- sh -c 'CMD' 2>&1
        // Length = len("printf '%s\n' '' | sudo -S -- sh -c '' 2>&1") + len(escaped_pass) + len(cmd)
        //        = 45 + scEscWrFinal + len(cmd) + 1
        llvm::Value* scCmdLen   = builder->CreateCall(getOrDeclareStrlen(), {scCmdPtr}, "sudo.cmdlen");
        nonNegValues_.insert(scCmdLen);
        // prefix: "printf '%s\n' '" = 16 chars, suffix: "' | sudo -S -- sh -c '" = 22 chars,
        // cmd_suffix: "' 2>&1" = 6 chars, NUL = 1
        llvm::Value* scFmtFixed = llvm::ConstantInt::get(getDefaultType(), 16 + 22 + 6 + 1);
        llvm::Value* scFmtLen   = builder->CreateAdd(
            builder->CreateAdd(scFmtFixed, scEscWrFinal, "sudo.flen1", true, true),
            scCmdLen, "sudo.flen", true, true);
        llvm::Value* scFmtBuf   = builder->CreateCall(getOrDeclareMalloc(), {scFmtLen}, "sudo.fmtbuf");

        // Use snprintf to build the command string
        // format string: "printf '%%s\n' '%s' | sudo -S -- sh -c '%s' 2>&1"
        // Note: %% becomes % in the output, so printf gets '%s\n'
        llvm::GlobalVariable* scFmtStr = module->getGlobalVariable("__sudo_fmt", true);
        if (!scFmtStr)
            scFmtStr = builder->CreateGlobalString(
                "printf '%%s\\n' '%s' | sudo -S -- sh -c '%s' 2>&1",
                "__sudo_fmt");
        builder->CreateCall(getOrDeclareSnprintf(), {scFmtBuf, scFmtLen, scFmtStr, scEscBuf, scCmdPtr});

        // ---- Step 3: popen + read loop (identical to COMMAND) ----
        auto* scPopenFn = llvm::dyn_cast_or_null<llvm::Function>(
            module->getOrInsertFunction("popen",
                llvm::FunctionType::get(scPtrTy, {scPtrTy, scPtrTy}, false))
            .getCallee());
        if (scPopenFn) {
            scPopenFn->addFnAttr(llvm::Attribute::NoUnwind);
            OMSC_ADD_NOCAPTURE(scPopenFn, 0);
            OMSC_ADD_NOCAPTURE(scPopenFn, 1);
        }
        auto* scPcloseFn = llvm::dyn_cast_or_null<llvm::Function>(
            module->getOrInsertFunction("pclose",
                llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {scPtrTy}, false))
            .getCallee());
        if (scPcloseFn) scPcloseFn->addFnAttr(llvm::Attribute::NoUnwind);

        llvm::GlobalVariable* scModeR = module->getGlobalVariable("__popen_mode_r", true);
        if (!scModeR) scModeR = builder->CreateGlobalString("r", "__popen_mode_r");

        llvm::Value* scFp = builder->CreateCall(scPopenFn, {scFmtBuf, scModeR}, "sudo.fp");
        llvm::Value* scNullPtr = llvm::ConstantPointerNull::get(scPtrTy);
        llvm::Value* scIsNull  = builder->CreateICmpEQ(scFp, scNullPtr, "sudo.isnull");

        llvm::BasicBlock* scNullBB  = llvm::BasicBlock::Create(*context, "sudo.null",  scParentFn);
        llvm::BasicBlock* scReadBB  = llvm::BasicBlock::Create(*context, "sudo.read",  scParentFn);
        llvm::BasicBlock* scMergeBB = llvm::BasicBlock::Create(*context, "sudo.merge", scParentFn);

        builder->CreateCondBr(scIsNull, scNullBB, scReadBB,
            llvm::MDBuilder(*context).createBranchWeights(1, 100));

        // Null path → empty string
        builder->SetInsertPoint(scNullBB);
        llvm::Value* scEmptyBuf = builder->CreateCall(getOrDeclareMalloc(),
            {llvm::ConstantInt::get(getDefaultType(), 1)}, "sudo.empty");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), scEmptyBuf);
        llvm::Value* scEmptyI64 = builder->CreatePtrToInt(scEmptyBuf, getDefaultType(), "sudo.emptyi64");
        builder->CreateBr(scMergeBB);
        llvm::BasicBlock* scNullEndBB = builder->GetInsertBlock();

        // Read path: growing buffer with fgets
        builder->SetInsertPoint(scReadBB);
        llvm::Value* scInitCap  = llvm::ConstantInt::get(getDefaultType(), 4096);
        llvm::AllocaInst* scCapPtr  = createEntryBlockAlloca(scParentFn, "sudo.cap",  getDefaultType());
        llvm::AllocaInst* scSizePtr = createEntryBlockAlloca(scParentFn, "sudo.size", getDefaultType());
        llvm::AllocaInst* scBufPtr  = createEntryBlockAlloca(scParentFn, "sudo.bufp", scPtrTy);
        builder->CreateStore(scInitCap, scCapPtr);
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), scSizePtr);
        llvm::Value* scInitBuf = builder->CreateCall(getOrDeclareMalloc(), {scInitCap}, "sudo.buf");
        builder->CreateStore(scInitBuf, scBufPtr);

        llvm::Value* scChunkBuf  = builder->CreateCall(getOrDeclareMalloc(),
            {llvm::ConstantInt::get(getDefaultType(), 256)}, "sudo.chunk");
        llvm::Value* scChunkSize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 256);

        llvm::BasicBlock* scRLoopBB  = llvm::BasicBlock::Create(*context, "sudo.rloop",  scParentFn);
        llvm::BasicBlock* scAppendBB = llvm::BasicBlock::Create(*context, "sudo.append", scParentFn);
        llvm::BasicBlock* scRDoneBB  = llvm::BasicBlock::Create(*context, "sudo.rdone",  scParentFn);
        llvm::BasicBlock* scGrowBB   = llvm::BasicBlock::Create(*context, "sudo.grow",   scParentFn);
        llvm::BasicBlock* scCopyBB   = llvm::BasicBlock::Create(*context, "sudo.copy",   scParentFn);

        builder->CreateBr(scRLoopBB);
        builder->SetInsertPoint(scRLoopBB);

        llvm::Value* scGot     = builder->CreateCall(getOrDeclareFgets(),
            {scChunkBuf, scChunkSize, scFp}, "sudo.got");
        llvm::Value* scGotNull = builder->CreateICmpEQ(scGot, scNullPtr, "sudo.gotnull");
        builder->CreateCondBr(scGotNull, scRDoneBB, scAppendBB,
            llvm::MDBuilder(*context).createBranchWeights(1, 1000));

        builder->SetInsertPoint(scAppendBB);
        llvm::Value* scChunkLen = builder->CreateCall(getOrDeclareStrlen(), {scChunkBuf}, "sudo.clen");
        llvm::Value* scCurSize  = builder->CreateAlignedLoad(getDefaultType(), scSizePtr, llvm::MaybeAlign(8), "sudo.csz");
        llvm::Value* scCurCap   = builder->CreateAlignedLoad(getDefaultType(), scCapPtr,  llvm::MaybeAlign(8), "sudo.ccap");
        llvm::Value* scNewSize  = builder->CreateAdd(scCurSize, scChunkLen, "sudo.nsz", true, true);
        llvm::Value* scNeedOne  = builder->CreateAdd(scNewSize,
            llvm::ConstantInt::get(getDefaultType(), 1), "sudo.ns1", true, true);
        llvm::Value* scNeedGrow = builder->CreateICmpUGT(scNeedOne, scCurCap, "sudo.needgrow");
        builder->CreateCondBr(scNeedGrow, scGrowBB, scCopyBB);

        builder->SetInsertPoint(scGrowBB);
        llvm::Value* scNewCap  = builder->CreateMul(scCurCap,
            llvm::ConstantInt::get(getDefaultType(), 2), "sudo.ncap", true, true);
        llvm::Value* scCurBufG = builder->CreateAlignedLoad(scPtrTy, scBufPtr, llvm::MaybeAlign(8), "sudo.cbufg");
        llvm::Value* scNewBuf  = builder->CreateCall(getOrDeclareRealloc(), {scCurBufG, scNewCap}, "sudo.nbuf");
        builder->CreateStore(scNewBuf, scBufPtr);
        builder->CreateStore(scNewCap, scCapPtr);
        builder->CreateBr(scCopyBB);

        builder->SetInsertPoint(scCopyBB);
        llvm::Value* scCurBufC = builder->CreateAlignedLoad(scPtrTy, scBufPtr, llvm::MaybeAlign(8), "sudo.cbufc");
        llvm::Value* scDst     = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), scCurBufC, scCurSize, "sudo.dst");
        builder->CreateCall(getOrDeclareMemcpy(), {scDst, scChunkBuf, scChunkLen});
        builder->CreateStore(scNewSize, scSizePtr);
        builder->CreateBr(scRLoopBB);

        builder->SetInsertPoint(scRDoneBB);
        builder->CreateCall(scPcloseFn, {scFp});
        // Free the temporary chunk buffer now that reading is complete.
        builder->CreateCall(getOrDeclareFree(), {scChunkBuf});
        llvm::Value* scFinalSz  = builder->CreateAlignedLoad(getDefaultType(), scSizePtr, llvm::MaybeAlign(8), "sudo.fsz");
        llvm::Value* scFinalBuf = builder->CreateAlignedLoad(scPtrTy, scBufPtr, llvm::MaybeAlign(8), "sudo.fbuf");
        llvm::Value* scNtPtr    = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(*context), scFinalBuf, scFinalSz, "sudo.nt");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), scNtPtr);
        llvm::Value* scReadResult = builder->CreatePtrToInt(scFinalBuf, getDefaultType(), "sudo.res");
        builder->CreateBr(scMergeBB);
        llvm::BasicBlock* scReadEndBB = builder->GetInsertBlock();

        builder->SetInsertPoint(scMergeBB);
        llvm::PHINode* scPhi = builder->CreatePHI(getDefaultType(), 2, "sudo.phi");
        scPhi->addIncoming(scEmptyI64, scNullEndBB);
        scPhi->addIncoming(scReadResult, scReadEndBB);
        stringReturningFunctions_.insert("sudo_command");
        return scPhi;
    }

    // -----------------------------------------------------------------------
    // env_get(name) — get an environment variable's value as a string.
    //   Returns the value string if the variable exists, or an empty string
    //   if it is not set (getenv returns NULL).
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ENV_GET) {
        validateArgCount(expr, "env_get", 1);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nameArg = generateExpression(expr->arguments[0].get());
        // nameArg may be an i64 pointer-as-int or already a ptr
        llvm::Value* namePtr = nameArg->getType()->isPointerTy()
            ? nameArg
            : builder->CreateIntToPtr(nameArg, ptrTy, "env_get.name");
        // Call getenv(name) — returns char* or NULL
        llvm::Value* envPtr = builder->CreateCall(getOrDeclareGetenv(), {namePtr}, "env_get.res");
        // If NULL, return empty string constant; otherwise return the pointer as i64
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* nullBB  = llvm::BasicBlock::Create(*context, "env_get.null",    parentFn);
        llvm::BasicBlock* okBB    = llvm::BasicBlock::Create(*context, "env_get.ok",      parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "env_get.merge",   parentFn);
        llvm::Value* isNull = builder->CreateICmpEQ(
            envPtr, llvm::ConstantPointerNull::get(ptrTy), "env_get.isnull");
        // Env variable not set is somewhat uncommon — favour the non-null path.
        llvm::MDNode* egW = llvm::MDBuilder(*context).createBranchWeights(1, 99);
        builder->CreateCondBr(isNull, nullBB, okBB, egW);

        builder->SetInsertPoint(nullBB);
        // Return a heap-allocated empty string when the variable is not set.
        llvm::Value* emptyBuf = builder->CreateCall(getOrDeclareMalloc(),
            {llvm::ConstantInt::get(getDefaultType(), 1)}, "env_get.empty");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), emptyBuf);
        llvm::Value* emptyI64 = builder->CreatePtrToInt(emptyBuf, getDefaultType(), "env_get.emptyi64");
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(okBB);
        llvm::Value* okI64 = builder->CreatePtrToInt(envPtr, getDefaultType(), "env_get.i64");
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "env_get.phi");
        phi->addIncoming(emptyI64, nullBB);
        phi->addIncoming(okI64, okBB);
        stringReturningFunctions_.insert("env_get");
        return phi;
    }

    // -----------------------------------------------------------------------
    // env_set(name, value) — set an environment variable.
    //   Returns 1 on success, 0 on failure (same sign convention as the rest
    //   of the standard library boolean returns).
    //   Calls setenv(name, value, 1) — overwrite always.
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ENV_SET) {
        validateArgCount(expr, "env_set", 2);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nameArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg  = generateExpression(expr->arguments[1].get());
        llvm::Value* namePtr = nameArg->getType()->isPointerTy()
            ? nameArg : builder->CreateIntToPtr(nameArg, ptrTy, "env_set.name");
        llvm::Value* valPtr = valArg->getType()->isPointerTy()
            ? valArg : builder->CreateIntToPtr(valArg, ptrTy, "env_set.val");
        // setenv(name, value, 1) — overwrite = 1
        llvm::Value* overwrite = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
        llvm::Value* rc = builder->CreateCall(getOrDeclareSetenv(), {namePtr, valPtr, overwrite}, "env_set.rc");
        // setenv returns 0 on success, -1 on failure; convert to 1/0
        llvm::Value* success = builder->CreateICmpEQ(rc,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "env_set.ok");
        llvm::Value* result = builder->CreateZExt(success, getDefaultType(), "env_set.res");
        return result;
    }

    // ── str_format(fmt, val1[, val2[, val3[, val4]]]) ──────────────────────
    // Format one to four values into a string using a printf-style format template.
    // Implemented via a two-pass snprintf: first probe for the required buffer
    // size (snprintf(NULL, 0, fmt, ...)), then allocate and fill.
    //
    // Supported argument types: integers (i64 → %lld), floats (double → %f/
    // %g/etc.), and strings (ptr → %s).  The caller's format string must use
    // matching format specifiers.
    //
    // Compile-time fold: when the format string and ALL value arguments are
    // compile-time constants, the formatted string is produced at compile time
    // and interned as a global string constant, eliminating all runtime overhead.
    if (bid == BuiltinId::STR_FORMAT) {
        const size_t nArgs = expr->arguments.size();
        if (nArgs < 2 || nArgs > 5)
            codegenError("str_format requires 2 to 5 arguments (fmt + 1 to 4 values)", expr);

        // ── Compile-time fold ───────────────────────────────────────
        // Try to constant-fold when fmt and ALL value args are compile-time
        // constants.  Only Integer and String ConstValues are supported (Float
        // values are not tracked as ConstValues in OmScript's constant system;
        // they fall through to the runtime snprintf path).
        if (auto fmtConst = tryFoldStr(expr->arguments[0].get())) {
            bool allConst = true;
            std::vector<ConstValue> constArgs;
            for (size_t i = 1; i < nArgs; ++i) {
                if (auto cv = tryFoldExprToConst(expr->arguments[i].get())) {
                    if (cv->kind != ConstValue::Kind::Integer &&
                        cv->kind != ConstValue::Kind::String) {
                        allConst = false;
                        break;
                    }
                    constArgs.push_back(*cv);
                } else {
                    allConst = false;
                    break;
                }
            }
            if (allConst) {
                const std::string& fmt = *fmtConst;
                char buf[4096];
                int written = -1;
                auto asStr = [](const ConstValue& cv) -> const char* {
                    return cv.strVal.c_str();
                };
                if (constArgs.size() == 1) {
                    const auto& a0 = constArgs[0];
                    if (a0.kind == ConstValue::Kind::Integer)
                        written = std::snprintf(buf, sizeof(buf), fmt.c_str(), a0.intVal);
                    else
                        written = std::snprintf(buf, sizeof(buf), fmt.c_str(), asStr(a0));
                } else if (constArgs.size() == 2) {
                    const auto& a0 = constArgs[0];
                    const auto& a1 = constArgs[1];
                    bool i0 = (a0.kind == ConstValue::Kind::Integer);
                    bool i1 = (a1.kind == ConstValue::Kind::Integer);
                    if      (i0 && i1)  written = std::snprintf(buf, sizeof(buf), fmt.c_str(), a0.intVal,   a1.intVal);
                    else if (i0 && !i1) written = std::snprintf(buf, sizeof(buf), fmt.c_str(), a0.intVal,   asStr(a1));
                    else if (!i0 && i1) written = std::snprintf(buf, sizeof(buf), fmt.c_str(), asStr(a0),   a1.intVal);
                    else                written = std::snprintf(buf, sizeof(buf), fmt.c_str(), asStr(a0),   asStr(a1));
                }
                if (written > 0 && written < static_cast<int>(sizeof(buf))) {
                    optStats_.constFolded++;
                    llvm::GlobalVariable* gv = internString(std::string(buf, static_cast<size_t>(written)));
                    stringReturningFunctions_.insert("str_format");
                    return llvm::ConstantExpr::getInBoundsGetElementPtr(
                        gv->getValueType(), gv,
                        llvm::ArrayRef<llvm::Constant*>{
                            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
                }
            }
        }

        // ── Runtime path: two-pass snprintf ────────────────────────
        // Pass 1: probe required buffer size via snprintf(NULL, 0, fmt, ...).
        // Pass 2: allocate (size + 1) bytes and fill.
        auto* ptrTy = llvm::PointerType::getUnqual(*context);

        // Codegen format string argument (must be a pointer/string).
        llvm::Value* fmtArg = generateExpression(expr->arguments[0].get());
        llvm::Value* fmtPtr = fmtArg->getType()->isPointerTy()
            ? fmtArg
            : builder->CreateIntToPtr(fmtArg, ptrTy, "strfmt.fmtptr");

        // Codegen value arguments, keeping track of their LLVM types for snprintf.
        std::vector<llvm::Value*> valArgs;
        for (size_t i = 1; i < nArgs; ++i) {
            llvm::Value* v = generateExpression(expr->arguments[i].get());
            // Strings are passed as pointers; integers as i64; floats as double.
            if (!v->getType()->isPointerTy() && !v->getType()->isDoubleTy())
                v = toDefaultType(v); // ensure i64
            valArgs.push_back(v);
        }

        // Probe call: snprintf(NULL, 0, fmt, val...) → required length.
        llvm::Value* nullBuf = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* zero32  = llvm::ConstantInt::get(getDefaultType(), 0);
        std::vector<llvm::Value*> probeCallArgs = {nullBuf, zero32, fmtPtr};
        probeCallArgs.insert(probeCallArgs.end(), valArgs.begin(), valArgs.end());
        llvm::Value* probeResult = builder->CreateCall(
            getOrDeclareSnprintf(), probeCallArgs, "strfmt.probe");
        // snprintf returns the number of characters that would have been written
        // (not counting the null terminator).  Extend to i64 for arithmetic.
        llvm::Value* neededLen = builder->CreateZExt(probeResult, getDefaultType(), "strfmt.needed");
        nonNegValues_.insert(neededLen);

        // Allocate neededLen + 1 bytes (room for null terminator).
        llvm::Value* allocSize = builder->CreateAdd(
            neededLen, llvm::ConstantInt::get(getDefaultType(), 1),
            "strfmt.allocsz", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "strfmt.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 1));

        // Fill call: snprintf(buf, neededLen + 1, fmt, val...).
        std::vector<llvm::Value*> fillCallArgs = {buf, allocSize, fmtPtr};
        fillCallArgs.insert(fillCallArgs.end(), valArgs.begin(), valArgs.end());
        builder->CreateCall(getOrDeclareSnprintf(), fillCallArgs);

        stringReturningFunctions_.insert("str_format");
        return buf;
    }

    // -----------------------------------------------------------------------
    // array_zip(a, b) — interleave two arrays.
    //   Result: [a[0], b[0], a[1], b[1], ..., a[m-1], b[m-1]]
    //   where m = min(len(a), len(b)).
    //   Result length = 2 * m.
    //
    // This lets callers access the i-th pair as result[2*i] and result[2*i+1].
    // The output is a heap-allocated OmScript array (length header + elements).
    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_ZIP) {
        validateArgCount(expr, "array_zip", 2);

        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* aPtr = builder->CreateIntToPtr(a, ptrTy, "azip.aptr");
        llvm::Value* bPtr = builder->CreateIntToPtr(b, ptrTy, "azip.bptr");

                llvm::Value* aLenLoad = emitLoadArrayLen(aPtr, "azip.alen");

                llvm::Value* bLenLoad = emitLoadArrayLen(bPtr, "azip.blen");

        // m = min(len(a), len(b))
        llvm::Value* aLtB = builder->CreateICmpSLT(aLenLoad, bLenLoad, "azip.altb");
        llvm::Value* m    = builder->CreateSelect(aLtB, aLenLoad, bLenLoad, "azip.m");
        nonNegValues_.insert(m);

        // outLen = 2 * m  (nuw+nsw: m ≤ INT64_MAX/2)
        llvm::Value* outLen = builder->CreateAdd(m, m, "azip.outlen", /*HasNUW=*/true, /*HasNSW=*/true);
        nonNegValues_.insert(outLen);

        // Allocate (outLen + 1) * 8 bytes
        llvm::Value* one   = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(outLen, one, "azip.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "azip.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf   = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "azip.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));

        // Store length header
        emitStoreArrayLen(outLen, buf);

        // Emit loop: for i in 0...m: buf[2*i+1] = a[i+1]; buf[2*i+2] = b[i+1]
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preBB  = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "azip.loop", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "azip.body", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "azip.done", parentFn);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        // Skip the loop entirely when m == 0 (branch weight: m>0 is likely)
        auto* skipW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(builder->CreateICmpSGT(m, zero, "azip.mgt0"), loopBB, doneBB, skipW);

        // loop header: phi i
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* i = builder->CreatePHI(getDefaultType(), 2, "azip.i");
        i->addIncoming(zero, preBB);
        builder->CreateCondBr(
            builder->CreateICmpSLT(i, m, "azip.cond"),
            bodyBB, doneBB,
            llvm::MDBuilder(*context).createBranchWeights(1000, 1));

        // loop body
        builder->SetInsertPoint(bodyBB);
        // srcIdx = i + 1  (nuw+nsw: i < m ≤ INT64_MAX-1)
        llvm::Value* srcIdx = builder->CreateAdd(i, one, "azip.srcidx", /*HasNUW=*/true, /*HasNSW=*/true);
        // dstIdx_a = 2*i + 1  (nuw+nsw)
        llvm::Value* two_i    = builder->CreateAdd(i, i, "azip.2i", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* dstIdxA  = builder->CreateAdd(two_i, one, "azip.dstidxa", /*HasNUW=*/true, /*HasNSW=*/true);
        // dstIdx_b = 2*i + 2  (nuw+nsw)
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);
        llvm::Value* dstIdxB  = builder->CreateAdd(two_i, two, "azip.dstidxb", /*HasNUW=*/true, /*HasNSW=*/true);

        // Load a[i] and b[i] (element at srcIdx in the header+data layout)
        llvm::Value* aElemPtr = builder->CreateInBoundsGEP(getDefaultType(), aPtr, srcIdx, "azip.aelemptr");
                llvm::Value* aElemLoad = emitLoadArrayElem(aElemPtr, "azip.aelem");

        llvm::Value* bElemPtr = builder->CreateInBoundsGEP(getDefaultType(), bPtr, srcIdx, "azip.belemptr");
                llvm::Value* bElemLoad = emitLoadArrayElem(bElemPtr, "azip.belem");

        // Store into output buf at interleaved positions
        llvm::Value* dstPtrA = builder->CreateInBoundsGEP(getDefaultType(), buf, dstIdxA, "azip.dstptra");
        auto* stA = builder->CreateStore(aElemLoad, dstPtrA);
        stA->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);

        llvm::Value* dstPtrB = builder->CreateInBoundsGEP(getDefaultType(), buf, dstIdxB, "azip.dstptrb");
        auto* stB = builder->CreateStore(bElemLoad, dstPtrB);
        stB->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);

        // Increment: next = i + 1  (nuw+nsw)
        llvm::Value* next = builder->CreateAdd(i, one, "azip.next", /*HasNUW=*/true, /*HasNSW=*/true);
        i->addIncoming(next, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        arrayReturningFunctions_.insert("array_zip");
        return builder->CreatePtrToInt(buf, getDefaultType(), "azip.result");
    }

    // ── BigInt builtins ──────────────────────────────────────────────────────
    // All bigint values are opaque pointers to heap-allocated OmBigInt objects.
    // The C runtime API is declared in include/bigint_runtime.h.

    // bigint(x) — construct from integer or string
    if (bid == BuiltinId::BIGINT_NEW) {
        if (expr->arguments.size() == 1) {
            llvm::Value* arg = generateExpression(expr->arguments[0].get());
            if (arg->getType()->isPointerTy()) {
                // Assume string argument
                return builder->CreateCall(getOrDeclareBigintNewStr(), {arg}, "bigint.new.str");
            } else {
                // Integer argument — widen to i64 if needed
                if (arg->getType() != getDefaultType())
                    arg = builder->CreateSExt(arg, getDefaultType(), "bigint.arg.sext");
                return builder->CreateCall(getOrDeclareBigintNewI64(), {arg}, "bigint.new.i64");
            }
        }
        codegenError("bigint() requires exactly 1 argument", expr);
    }

    // Binary bigint ops: both args must be bigint pointers
    auto getBigintBinaryArgs = [&](const char* name) -> std::pair<llvm::Value*, llvm::Value*> {
        validateArgCount(expr, name, 2);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        if (!a->getType()->isPointerTy() || !b->getType()->isPointerTy())
            codegenError(std::string(name) + ": arguments must be bigint values", expr);
        return {a, b};
    };
    auto getBigintUnaryArg = [&](const char* name) -> llvm::Value* {
        validateArgCount(expr, name, 1);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        if (!a->getType()->isPointerTy())
            codegenError(std::string(name) + ": argument must be a bigint value", expr);
        return a;
    };

    if (bid == BuiltinId::BIGINT_ADD) {
        auto [a, b] = getBigintBinaryArgs("bigint_add");
        return builder->CreateCall(getOrDeclareBigintAdd(), {a, b}, "bigint.add");
    }
    if (bid == BuiltinId::BIGINT_SUB) {
        auto [a, b] = getBigintBinaryArgs("bigint_sub");
        return builder->CreateCall(getOrDeclareBigintSub(), {a, b}, "bigint.sub");
    }
    if (bid == BuiltinId::BIGINT_MUL) {
        auto [a, b] = getBigintBinaryArgs("bigint_mul");
        return builder->CreateCall(getOrDeclareBigintMul(), {a, b}, "bigint.mul");
    }
    if (bid == BuiltinId::BIGINT_DIV) {
        auto [a, b] = getBigintBinaryArgs("bigint_div");
        return builder->CreateCall(getOrDeclareBigintDiv(), {a, b}, "bigint.div");
    }
    if (bid == BuiltinId::BIGINT_MOD) {
        auto [a, b] = getBigintBinaryArgs("bigint_mod");
        return builder->CreateCall(getOrDeclareBigintMod(), {a, b}, "bigint.mod");
    }
    if (bid == BuiltinId::BIGINT_NEG) {
        llvm::Value* a = getBigintUnaryArg("bigint_neg");
        return builder->CreateCall(getOrDeclareBigintNeg(), {a}, "bigint.neg");
    }
    if (bid == BuiltinId::BIGINT_ABS) {
        llvm::Value* a = getBigintUnaryArg("bigint_abs");
        return builder->CreateCall(getOrDeclareBigintAbs(), {a}, "bigint.abs");
    }
    if (bid == BuiltinId::BIGINT_POW) {
        auto [a, b] = getBigintBinaryArgs("bigint_pow");
        return builder->CreateCall(getOrDeclareBigintPow(), {a, b}, "bigint.pow");
    }
    if (bid == BuiltinId::BIGINT_GCD) {
        auto [a, b] = getBigintBinaryArgs("bigint_gcd");
        return builder->CreateCall(getOrDeclareBigintGcd(), {a, b}, "bigint.gcd");
    }
    // Comparison builtins: return i32 (0 or 1), widened to i64 for OmScript
    if (bid == BuiltinId::BIGINT_EQ) {
        auto [a, b] = getBigintBinaryArgs("bigint_eq");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintEq(), {a, b}, "bigint.eq");
        return builder->CreateZExt(r, getDefaultType(), "bigint.eq.zext");
    }
    if (bid == BuiltinId::BIGINT_LT) {
        auto [a, b] = getBigintBinaryArgs("bigint_lt");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintLt(), {a, b}, "bigint.lt");
        return builder->CreateZExt(r, getDefaultType(), "bigint.lt.zext");
    }
    if (bid == BuiltinId::BIGINT_LE) {
        auto [a, b] = getBigintBinaryArgs("bigint_le");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintLe(), {a, b}, "bigint.le");
        return builder->CreateZExt(r, getDefaultType(), "bigint.le.zext");
    }
    if (bid == BuiltinId::BIGINT_GT) {
        auto [a, b] = getBigintBinaryArgs("bigint_gt");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintGt(), {a, b}, "bigint.gt");
        return builder->CreateZExt(r, getDefaultType(), "bigint.gt.zext");
    }
    if (bid == BuiltinId::BIGINT_GE) {
        auto [a, b] = getBigintBinaryArgs("bigint_ge");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintGe(), {a, b}, "bigint.ge");
        return builder->CreateZExt(r, getDefaultType(), "bigint.ge.zext");
    }
    if (bid == BuiltinId::BIGINT_CMP) {
        auto [a, b] = getBigintBinaryArgs("bigint_cmp");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintCmp(), {a, b}, "bigint.cmp");
        return builder->CreateSExt(r, getDefaultType(), "bigint.cmp.sext");
    }
    if (bid == BuiltinId::BIGINT_TOSTRING) {
        llvm::Value* a = getBigintUnaryArg("bigint_tostring");
        stringReturningFunctions_.insert("bigint_tostring");
        return builder->CreateCall(getOrDeclareBigintTostring(), {a}, "bigint.tostring");
    }
    if (bid == BuiltinId::BIGINT_TO_I64) {
        llvm::Value* a = getBigintUnaryArg("bigint_to_i64");
        return builder->CreateCall(getOrDeclareBigintToI64(), {a}, "bigint.to_i64");
    }
    if (bid == BuiltinId::BIGINT_BIT_LENGTH) {
        llvm::Value* a = getBigintUnaryArg("bigint_bit_length");
        return builder->CreateCall(getOrDeclareBigintBitLength(), {a}, "bigint.bitlen");
    }
    if (bid == BuiltinId::BIGINT_IS_ZERO) {
        llvm::Value* a = getBigintUnaryArg("bigint_is_zero");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintIsZero(), {a}, "bigint.iszero");
        return builder->CreateZExt(r, getDefaultType(), "bigint.iszero.zext");
    }
    if (bid == BuiltinId::BIGINT_IS_NEGATIVE) {
        llvm::Value* a = getBigintUnaryArg("bigint_is_negative");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintIsNegative(), {a}, "bigint.isneg");
        return builder->CreateZExt(r, getDefaultType(), "bigint.isneg.zext");
    }
    if (bid == BuiltinId::BIGINT_SHL) {
        validateArgCount(expr, "bigint_shl", 2);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* n = generateExpression(expr->arguments[1].get());
        n = toDefaultType(n);
        return builder->CreateCall(getOrDeclareBigintShl(), {a, n}, "bigint.shl");
    }
    if (bid == BuiltinId::BIGINT_SHR) {
        validateArgCount(expr, "bigint_shr", 2);
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* n = generateExpression(expr->arguments[1].get());
        n = toDefaultType(n);
        return builder->CreateCall(getOrDeclareBigintShr(), {a, n}, "bigint.shr");
    }

    // ── mulhi(a, b) — signed high 64 bits of 128-bit product ─────────────────
    // Returns the upper 64 bits of the full 128-bit product of two i64 values.
    // Useful for: fixed-point arithmetic, fast modular reduction, Knuth
    // multiplicative hashing.  More efficient than the user writing __int128
    // and right-shifting manually, since LLVM lowers this to a single IMUL
    // high-half instruction on x86-64 (mulq / imulq).
    if (bid == BuiltinId::INT_MULHI) {
        validateArgCount(expr, "mulhi", 2);
        // Constant-fold if both args are compile-time known.
        if (auto a = tryFoldInt(expr->arguments[0].get()))
            if (auto b = tryFoldInt(expr->arguments[1].get())) {
                using I128 = __int128;
                I128 product = static_cast<I128>(*a) * static_cast<I128>(*b);
                int64_t hi = static_cast<int64_t>(product >> 64);
                return llvm::ConstantInt::get(getDefaultType(), hi);
            }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        // Widen to i128, multiply, then shift right 64.
        llvm::Type* i128Ty = llvm::Type::getIntNTy(*context, 128);
        llvm::Value* aWide = builder->CreateSExt(a, i128Ty, "mulhi.a");
        llvm::Value* bWide = builder->CreateSExt(b, i128Ty, "mulhi.b");
        llvm::Value* prod  = builder->CreateMul(aWide, bWide, "mulhi.prod");
        llvm::Value* hi128 = builder->CreateAShr(prod,
            llvm::ConstantInt::get(i128Ty, 64), "mulhi.hi128");
        return builder->CreateTrunc(hi128, getDefaultType(), "mulhi.result");
    }

    // ── mulhi_u(a, b) — unsigned high 64 bits of 128-bit product ─────────────
    if (bid == BuiltinId::UINT_MULHI) {
        validateArgCount(expr, "mulhi_u", 2);
        if (auto a = tryFoldInt(expr->arguments[0].get()))
            if (auto b = tryFoldInt(expr->arguments[1].get())) {
                using U128 = unsigned __int128;
                U128 product = static_cast<U128>(static_cast<uint64_t>(*a))
                             * static_cast<U128>(static_cast<uint64_t>(*b));
                int64_t hi = static_cast<int64_t>(static_cast<uint64_t>(product >> 64));
                return llvm::ConstantInt::get(getDefaultType(), hi);
            }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        llvm::Type* i128Ty = llvm::Type::getIntNTy(*context, 128);
        llvm::Value* aWide = builder->CreateZExt(a, i128Ty, "umulhi.a");
        llvm::Value* bWide = builder->CreateZExt(b, i128Ty, "umulhi.b");
        llvm::Value* prod  = builder->CreateMul(aWide, bWide, "umulhi.prod");
        llvm::Value* hi128 = builder->CreateLShr(prod,
            llvm::ConstantInt::get(i128Ty, 64), "umulhi.hi128");
        return builder->CreateTrunc(hi128, getDefaultType(), "umulhi.result");
    }

    // ── absdiff(a, b) — |a - b| without signed overflow ─────────────────────
    // Computes the absolute difference of two integers safely, widening to i128
    // to avoid overflow before taking abs.
    if (bid == BuiltinId::INT_ABSDIFF) {
        validateArgCount(expr, "absdiff", 2);
        if (auto a = tryFoldInt(expr->arguments[0].get()))
            if (auto b = tryFoldInt(expr->arguments[1].get())) {
                using I128 = __int128;
                I128 diff = static_cast<I128>(*a) - static_cast<I128>(*b);
                if (diff < 0) diff = -diff;
                return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(diff));
            }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        // Use select: diff = a>=b ? a-b : b-a.  Both subtractions are safe
        // because we only execute the non-overflowing branch.
        llvm::Value* cmp  = builder->CreateICmpSGE(a, b, "absdiff.ge");
        llvm::Value* sub1 = builder->CreateSub(a, b, "absdiff.sub1");
        llvm::Value* sub2 = builder->CreateSub(b, a, "absdiff.sub2");
        llvm::Value* result = builder->CreateSelect(cmp, sub1, sub2, "absdiff.result");
        nonNegValues_.insert(result);
        return result;
    }

    // ── fast_sqrt(x) — sqrt with fast-math flags (RSqrt-eligible on x86) ────
    // Emits llvm.sqrt.f64 with the reassoc + nnan + ninf fast-math flags, which
    // allows the backend to use the hardware RSQRT instruction followed by one
    // Newton-Raphson refinement step (much faster than full IEEE sqrt on modern
    // CPUs when precision ~14 bits is acceptable).
    if (bid == BuiltinId::FAST_SQRT) {
        validateArgCount(expr, "fast_sqrt", 1);
        if (auto v = tryFoldInt(expr->arguments[0].get())) {
            double d = std::sqrt(static_cast<double>(*v));
            return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(d));
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        llvm::Function* sqrtFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sqrt, {getFloatType()});
        llvm::CallInst* result = builder->CreateCall(sqrtFn, {fval}, "fast.sqrt");
        // Apply fast-math: reassoc + nnan + ninf allows RSQRT approximation.
        llvm::FastMathFlags fmf;
        fmf.setAllowReassoc(true);
        fmf.setNoNaNs(true);
        fmf.setNoInfs(true);
        result->setFastMathFlags(fmf);
        return builder->CreateFPToSI(result, getDefaultType(), "fast.sqrt.result");
    }

    // ── is_nan(x) — 1 if x (reinterpreted as f64) is a NaN, else 0 ──────────
    if (bid == BuiltinId::FLOAT_IS_NAN) {
        validateArgCount(expr, "is_nan", 1);
        if (auto v = tryFoldInt(expr->arguments[0].get())) {
            // Reinterpret i64 bits as f64 and check NaN.
            double d;
            uint64_t bits = static_cast<uint64_t>(*v);
            std::memcpy(&d, &bits, 8);
            return llvm::ConstantInt::get(getDefaultType(), std::isnan(d) ? 1 : 0);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        // IEEE NaN: unordered comparison with itself is true only for NaN.
        llvm::Value* cmp = builder->CreateFCmpUNO(fval, fval, "is_nan.cmp");
        return builder->CreateZExt(cmp, getDefaultType(), "is_nan.result");
    }

    // ── is_inf(x) — 1 if x (as f64) is ±Infinity, else 0 ───────────────────
    if (bid == BuiltinId::FLOAT_IS_INF) {
        validateArgCount(expr, "is_inf", 1);
        if (auto v = tryFoldInt(expr->arguments[0].get())) {
            double d;
            uint64_t bits = static_cast<uint64_t>(*v);
            std::memcpy(&d, &bits, 8);
            return llvm::ConstantInt::get(getDefaultType(), std::isinf(d) ? 1 : 0);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        // |x| == inf  ⟺  x == +inf  ||  x == -inf
        llvm::Value* pos = llvm::ConstantFP::getInfinity(getFloatType(), false);
        llvm::Value* neg = llvm::ConstantFP::getInfinity(getFloatType(), true);
        llvm::Value* isPos = builder->CreateFCmpOEQ(fval, pos, "is_inf.pos");
        llvm::Value* isNeg = builder->CreateFCmpOEQ(fval, neg, "is_inf.neg");
        llvm::Value* either = builder->CreateOr(isPos, isNeg, "is_inf.either");
        return builder->CreateZExt(either, getDefaultType(), "is_inf.result");
    }

    // ── 2D Column-Major Matrix Builtins ──────────────────────────────────────
    //
    // Memory layout (column-major, like Fortran/BLAS/LAPACK):
    //
    //   slot 0  : rows   (i64 header)
    //   slot 1  : cols   (i64 header)
    //   slot 2  : A[0,0]            ← start of column 0
    //   slot 3  : A[1,0]
    //   ...
    //   slot 2+rows-1  : A[rows-1, 0]  ← end of column 0
    //   slot 2+rows    : A[0,1]         ← start of column 1
    //   ...
    //   slot 2+cols*rows-1 : A[rows-1, cols-1]
    //
    // Element (i,j) is at byte offset: (2 + j*rows + i) * 8
    //
    // Column-major means the *row* index (i) is the fast-varying dimension.
    // A full column of any matrix is a contiguous i64 array of length `rows`
    // with stride 1 — ideal for auto-vectorization by LLVM's SLP and
    // loop vectorizer without gather/scatter.
    //
    // Helper lambda: given a column-major matrix pointer and the pre-loaded
    // `rows` value, compute the GEP to element (i, j).
    auto matElemPtr = [&](llvm::Value* mPtr, llvm::Value* rowsV,
                          llvm::Value* iV, llvm::Value* jV,
                          const char* name) -> llvm::Value* {
        // slot = 2 + j * rows + i  (all i64 arithmetic, NSW to aid SCEV)
        llvm::Value* jRows  = builder->CreateMul(jV, rowsV,
                                  (std::string(name) + ".jrows").c_str(),
                                  /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slot   = builder->CreateAdd(jRows, iV,
                                  (std::string(name) + ".slot0").c_str(),
                                  /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slot2  = builder->CreateAdd(slot,
                                  llvm::ConstantInt::get(getDefaultType(), 2),
                                  (std::string(name) + ".slot2").c_str(),
                                  /*HasNUW=*/true, /*HasNSW=*/true);
        return builder->CreateInBoundsGEP(getDefaultType(), mPtr, slot2,
                                          (std::string(name) + ".ptr").c_str());
    };

    // ── mat_new(rows, cols) ─────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_NEW) {
        validateArgCount(expr, "mat_new", 2);
        llvm::Value* rowsV = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* colsV = toDefaultType(generateExpression(expr->arguments[1].get()));
        // Clamp negative dimensions to 0 to prevent overflow in size calculation.
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        rowsV = builder->CreateSelect(
            builder->CreateICmpSLT(rowsV, zero, "mat.rows.neg"), zero, rowsV, "mat.rows");
        colsV = builder->CreateSelect(
            builder->CreateICmpSLT(colsV, zero, "mat.cols.neg"), zero, colsV, "mat.cols");
        // slots = rows * cols + 2  (two i64 header slots)
        llvm::Value* elems = builder->CreateMul(rowsV, colsV, "mat.elems",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slots = builder->CreateAdd(elems,
                                llvm::ConstantInt::get(getDefaultType(), 2),
                                "mat.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        // calloc(slots, 8) → zero-initialised; header must still be written.
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* buf   = builder->CreateCall(getOrDeclareCalloc(), {slots, eight}, "mat.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
        // Write header: buf[0] = rows, buf[1] = cols
        llvm::Value* hdr0 = builder->CreateInBoundsGEP(getDefaultType(), buf,
                                llvm::ConstantInt::get(getDefaultType(), 0), "mat.hdr0");
        builder->CreateAlignedStore(rowsV, hdr0, llvm::MaybeAlign(8));
        llvm::Value* hdr1 = builder->CreateInBoundsGEP(getDefaultType(), buf,
                                llvm::ConstantInt::get(getDefaultType(), 1), "mat.hdr1");
        builder->CreateAlignedStore(colsV, hdr1, llvm::MaybeAlign(8));
        // mat_new returns a matrix handle — treat it like an array-returning function
        // so the codegen knows its result is a heap pointer.
        arrayReturningFunctions_.insert("mat_new");
        return builder->CreatePtrToInt(buf, getDefaultType(), "mat.new.result");
    }

    // ── mat_fill(rows, cols, val) ───────────────────────────────────────────
    if (bid == BuiltinId::MAT_FILL) {
        validateArgCount(expr, "mat_fill", 3);
        llvm::Value* rowsV = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* colsV = toDefaultType(generateExpression(expr->arguments[1].get()));
        llvm::Value* valV  = toDefaultType(generateExpression(expr->arguments[2].get()));
        llvm::Value* zero  = llvm::ConstantInt::get(getDefaultType(), 0);
        rowsV = builder->CreateSelect(
            builder->CreateICmpSLT(rowsV, zero, "matf.rows.neg"), zero, rowsV, "matf.rows");
        colsV = builder->CreateSelect(
            builder->CreateICmpSLT(colsV, zero, "matf.cols.neg"), zero, colsV, "matf.cols");
        llvm::Value* elems = builder->CreateMul(rowsV, colsV, "matf.elems",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slots = builder->CreateAdd(elems,
                                llvm::ConstantInt::get(getDefaultType(), 2),
                                "matf.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "matf.bytes",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf   = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "matf.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
        // Write header
        builder->CreateAlignedStore(rowsV,
            builder->CreateInBoundsGEP(getDefaultType(), buf,
                llvm::ConstantInt::get(getDefaultType(), 0), "matf.hdr0"),
            llvm::MaybeAlign(8));
        builder->CreateAlignedStore(colsV,
            builder->CreateInBoundsGEP(getDefaultType(), buf,
                llvm::ConstantInt::get(getDefaultType(), 1), "matf.hdr1"),
            llvm::MaybeAlign(8));
        // Fill loop over all elements (column-major order, stride-1 access).
        // Use emitCountingLoop to get vectorized fill.
        llvm::Value* fillBuf = buf;
        llvm::Value* fillVal = valV;
        emitCountingLoop("matf.fill", elems, zero, 4,
            [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
                // slot = idx + 2  (skip the two header slots)
                llvm::Value* slot = builder->CreateAdd(idx,
                    llvm::ConstantInt::get(getDefaultType(), 2),
                    "matf.slot", /*HasNUW=*/true, /*HasNSW=*/true);
                llvm::Value* ep = builder->CreateInBoundsGEP(getDefaultType(),
                    fillBuf, slot, "matf.ep");
                emitStoreArrayElem(fillVal, ep);
                llvm::Value* next = builder->CreateAdd(idx,
                    llvm::ConstantInt::get(getDefaultType(), 1),
                    "matf.next", /*HasNUW=*/true, /*HasNSW=*/true);
                idx->addIncoming(next, builder->GetInsertBlock());
                attachLoopMetadataVec(
                    llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
            });
        arrayReturningFunctions_.insert("mat_fill");
        return builder->CreatePtrToInt(buf, getDefaultType(), "matf.result");
    }

    // ── mat_rows(m) ─────────────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_ROWS) {
        validateArgCount(expr, "mat_rows", 1);
        llvm::Value* mVal = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* mPtr = builder->CreateIntToPtr(mVal,
            llvm::PointerType::getUnqual(*context), "matr.ptr");
        llvm::Value* hdr0 = builder->CreateInBoundsGEP(getDefaultType(), mPtr,
            llvm::ConstantInt::get(getDefaultType(), 0), "matr.hdr0");
        return builder->CreateAlignedLoad(getDefaultType(), hdr0,
            llvm::MaybeAlign(8), "mat.rows.val");
    }

    // ── mat_cols(m) ─────────────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_COLS) {
        validateArgCount(expr, "mat_cols", 1);
        llvm::Value* mVal = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* mPtr = builder->CreateIntToPtr(mVal,
            llvm::PointerType::getUnqual(*context), "matc.ptr");
        llvm::Value* hdr1 = builder->CreateInBoundsGEP(getDefaultType(), mPtr,
            llvm::ConstantInt::get(getDefaultType(), 1), "matc.hdr1");
        return builder->CreateAlignedLoad(getDefaultType(), hdr1,
            llvm::MaybeAlign(8), "mat.cols.val");
    }

    // ── mat_get(m, i, j) ────────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_GET) {
        validateArgCount(expr, "mat_get", 3);
        llvm::Value* mVal  = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* iVal  = toDefaultType(generateExpression(expr->arguments[1].get()));
        llvm::Value* jVal  = toDefaultType(generateExpression(expr->arguments[2].get()));
        llvm::Value* mPtr  = builder->CreateIntToPtr(mVal,
            llvm::PointerType::getUnqual(*context), "matg.ptr");
        llvm::Value* rowsV = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mPtr,
                llvm::ConstantInt::get(getDefaultType(), 0), "matg.hdr0"),
            llvm::MaybeAlign(8), "matg.rows");
        llvm::Value* ep    = matElemPtr(mPtr, rowsV, iVal, jVal, "matg");
        auto* ld = builder->CreateAlignedLoad(getDefaultType(), ep,
            llvm::MaybeAlign(8), "mat.get.val");
        if (optimizationLevel >= OptimizationLevel::O1)
            ld->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
        if (currentLoopAccessGroup_)
            ld->setMetadata(llvm::LLVMContext::MD_access_group, currentLoopAccessGroup_);
        return ld;
    }

    // ── mat_set(m, i, j, val) ───────────────────────────────────────────────
    if (bid == BuiltinId::MAT_SET) {
        validateArgCount(expr, "mat_set", 4);
        llvm::Value* mVal  = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* iVal  = toDefaultType(generateExpression(expr->arguments[1].get()));
        llvm::Value* jVal  = toDefaultType(generateExpression(expr->arguments[2].get()));
        llvm::Value* valV  = toDefaultType(generateExpression(expr->arguments[3].get()));
        llvm::Value* mPtr  = builder->CreateIntToPtr(mVal,
            llvm::PointerType::getUnqual(*context), "mats.ptr");
        llvm::Value* rowsV = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mPtr,
                llvm::ConstantInt::get(getDefaultType(), 0), "mats.hdr0"),
            llvm::MaybeAlign(8), "mats.rows");
        llvm::Value* ep    = matElemPtr(mPtr, rowsV, iVal, jVal, "mats");
        auto* st = builder->CreateAlignedStore(valV, ep, llvm::MaybeAlign(8));
        if (currentLoopAccessGroup_)
            st->setMetadata(llvm::LLVMContext::MD_access_group, currentLoopAccessGroup_);
        return mVal; // return the matrix pointer for chaining
    }

    // ── mat_transp(m) ────────────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_TRANSP) {
        validateArgCount(expr, "mat_transp", 1);
        llvm::Value* mVal   = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* mPtr   = builder->CreateIntToPtr(mVal,
            llvm::PointerType::getUnqual(*context), "matt.ptr");
        llvm::Value* rowsV  = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mPtr,
                llvm::ConstantInt::get(getDefaultType(), 0), "matt.hdr0"),
            llvm::MaybeAlign(8), "matt.rows");
        llvm::Value* colsV  = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mPtr,
                llvm::ConstantInt::get(getDefaultType(), 1), "matt.hdr1"),
            llvm::MaybeAlign(8), "matt.cols");
        // Allocate transposed matrix: mat_new(cols, rows)
        llvm::Value* elems  = builder->CreateMul(rowsV, colsV, "matt.elems",
                                                  /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slots  = builder->CreateAdd(elems,
                                  llvm::ConstantInt::get(getDefaultType(), 2),
                                  "matt.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* eight  = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* tBuf   = builder->CreateCall(getOrDeclareCalloc(),
                                  {slots, eight}, "matt.buf");
        llvm::cast<llvm::CallInst>(tBuf)->addRetAttr(llvm::Attribute::NonNull);
        // Header of transpose: rows_T = cols, cols_T = rows
        builder->CreateAlignedStore(colsV,
            builder->CreateInBoundsGEP(getDefaultType(), tBuf,
                llvm::ConstantInt::get(getDefaultType(), 0), "matt.thdr0"),
            llvm::MaybeAlign(8));
        builder->CreateAlignedStore(rowsV,
            builder->CreateInBoundsGEP(getDefaultType(), tBuf,
                llvm::ConstantInt::get(getDefaultType(), 1), "matt.thdr1"),
            llvm::MaybeAlign(8));
        // Copy loop: T[j, i] = A[i, j]  for all i in 0..rows, j in 0..cols
        // Outer loop over columns j, inner over rows i (both contiguous in their
        // respective matrices, so both the read and write are stride-1 by column).
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        emitCountingLoop("matt.j", colsV, zero, 1,
            [&](llvm::PHINode* jIdx, llvm::BasicBlock* jLoopBB) {
                llvm::Value* jIdxV = jIdx; // j column index
                emitCountingLoop("matt.i", rowsV, zero, 4,
                    [&](llvm::PHINode* iIdx, llvm::BasicBlock* iLoopBB) {
                        // Load A[i, j] (column-major: slot = j*rows + i + 2)
                        llvm::Value* epA = matElemPtr(mPtr, rowsV, iIdx, jIdxV, "matt.a");
                        auto* aElem = builder->CreateAlignedLoad(getDefaultType(),
                            epA, llvm::MaybeAlign(8), "matt.aval");
                        // Store T[j, i]: T has rowsT=cols, so slot = i*cols + j + 2
                        llvm::Value* epT = matElemPtr(tBuf, colsV, jIdxV, iIdx, "matt.t");
                        builder->CreateAlignedStore(aElem, epT, llvm::MaybeAlign(8));
                        llvm::Value* ni = builder->CreateAdd(iIdx,
                            llvm::ConstantInt::get(getDefaultType(), 1), "matt.ni",
                            /*HasNUW=*/true, /*HasNSW=*/true);
                        iIdx->addIncoming(ni, builder->GetInsertBlock());
                        attachLoopMetadataVec(
                            llvm::cast<llvm::BranchInst>(builder->CreateBr(iLoopBB)));
                    });
                llvm::Value* nj = builder->CreateAdd(jIdx,
                    llvm::ConstantInt::get(getDefaultType(), 1), "matt.nj",
                    /*HasNUW=*/true, /*HasNSW=*/true);
                jIdx->addIncoming(nj, builder->GetInsertBlock());
                builder->CreateBr(jLoopBB);
            });
        arrayReturningFunctions_.insert("mat_transp");
        return builder->CreatePtrToInt(tBuf, getDefaultType(), "matt.result");
    }

    // ── mat_mul(a, b) ────────────────────────────────────────────────────────
    // Column-major matrix multiply: C = A × B
    // A is (m × k), B is (k × n), C is (m × n)  — all column-major.
    //
    // Loop order: j (outer) → p (middle) → i (inner, SIMD-vectorizable)
    //
    //   for j in 0..n:          ← iterate over columns of B and C
    //     for p in 0..k:        ← iterate over rows of B / cols of A
    //       b_pj = B[p, j]      ← scalar load of B's element (contiguous in B's col j)
    //       for i in 0..m:      ← vectorized: column j of C += b_pj * column p of A
    //         C[i, j] += b_pj * A[i, p]
    //
    // The inner loop reads column p of A (contiguous, stride-1) and writes
    // column j of C (contiguous, stride-1) — both are ideal for SIMD.
    // This is the BLAS dgemm "column-saxpy" algorithm.
    if (bid == BuiltinId::MAT_MUL) {
        validateArgCount(expr, "mat_mul", 2);
        llvm::Value* aVal  = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* bVal  = toDefaultType(generateExpression(expr->arguments[1].get()));
        llvm::Value* aPtr  = builder->CreateIntToPtr(aVal,
            llvm::PointerType::getUnqual(*context), "matm.aptr");
        llvm::Value* bPtr  = builder->CreateIntToPtr(bVal,
            llvm::PointerType::getUnqual(*context), "matm.bptr");
        // Dimension loads
        llvm::Value* mDim  = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), aPtr,
                llvm::ConstantInt::get(getDefaultType(), 0), "matm.a.hdr0"),
            llvm::MaybeAlign(8), "matm.m");
        llvm::Value* kDim  = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), aPtr,
                llvm::ConstantInt::get(getDefaultType(), 1), "matm.a.hdr1"),
            llvm::MaybeAlign(8), "matm.k");
        llvm::Value* nDim  = builder->CreateAlignedLoad(getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), bPtr,
                llvm::ConstantInt::get(getDefaultType(), 1), "matm.b.hdr1"),
            llvm::MaybeAlign(8), "matm.n");
        // Allocate result matrix C(m, n) — zero-initialised via calloc
        llvm::Value* cElems = builder->CreateMul(mDim, nDim, "matm.celems",
                                                  /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* cSlots = builder->CreateAdd(cElems,
                                  llvm::ConstantInt::get(getDefaultType(), 2),
                                  "matm.cslots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* eight  = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* cBuf   = builder->CreateCall(getOrDeclareCalloc(),
                                  {cSlots, eight}, "matm.cbuf");
        llvm::cast<llvm::CallInst>(cBuf)->addRetAttr(llvm::Attribute::NonNull);
        // Write C header
        builder->CreateAlignedStore(mDim,
            builder->CreateInBoundsGEP(getDefaultType(), cBuf,
                llvm::ConstantInt::get(getDefaultType(), 0), "matm.chdr0"),
            llvm::MaybeAlign(8));
        builder->CreateAlignedStore(nDim,
            builder->CreateInBoundsGEP(getDefaultType(), cBuf,
                llvm::ConstantInt::get(getDefaultType(), 1), "matm.chdr1"),
            llvm::MaybeAlign(8));
        // Triple loop: j (outer) → p (middle) → i (inner, vectorized)
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        emitCountingLoop("matm.j", nDim, zero, 1,
            [&](llvm::PHINode* jIdx, llvm::BasicBlock* jLoopBB) {
                emitCountingLoop("matm.p", kDim, zero, 1,
                    [&](llvm::PHINode* pIdx, llvm::BasicBlock* pLoopBB) {
                        // b_pj = B[p, j]  (scalar)
                        llvm::Value* bEp   = matElemPtr(bPtr, kDim, pIdx, jIdx, "matm.b");
                        llvm::Value* b_pj  = builder->CreateAlignedLoad(getDefaultType(),
                            bEp, llvm::MaybeAlign(8), "matm.bpj");
                        // Inner loop: C[i, j] += b_pj * A[i, p]  (vectorized)
                        emitCountingLoop("matm.i", mDim, zero, 4,
                            [&](llvm::PHINode* iIdx, llvm::BasicBlock* iLoopBB) {
                                // A[i, p]: slot = p*m + i + 2
                                llvm::Value* aEp  = matElemPtr(aPtr, mDim, iIdx, pIdx, "matm.a");
                                llvm::Value* a_ip = builder->CreateAlignedLoad(getDefaultType(),
                                    aEp, llvm::MaybeAlign(8), "matm.aip");
                                // C[i, j]: slot = j*m + i + 2
                                llvm::Value* cEp  = matElemPtr(cBuf, mDim, iIdx, jIdx, "matm.c");
                                llvm::Value* c_ij = builder->CreateAlignedLoad(getDefaultType(),
                                    cEp, llvm::MaybeAlign(8), "matm.cij");
                                // c_ij += b_pj * a_ip
                                llvm::Value* prod = builder->CreateMul(b_pj, a_ip, "matm.prod",
                                    /*HasNUW=*/false, /*HasNSW=*/true);
                                llvm::Value* sum  = builder->CreateAdd(c_ij, prod, "matm.sum",
                                    /*HasNUW=*/false, /*HasNSW=*/true);
                                builder->CreateAlignedStore(sum, cEp, llvm::MaybeAlign(8));
                                llvm::Value* ni = builder->CreateAdd(iIdx,
                                    llvm::ConstantInt::get(getDefaultType(), 1), "matm.ni",
                                    /*HasNUW=*/true, /*HasNSW=*/true);
                                iIdx->addIncoming(ni, builder->GetInsertBlock());
                                attachLoopMetadataVec(
                                    llvm::cast<llvm::BranchInst>(builder->CreateBr(iLoopBB)));
                            });
                        llvm::Value* np = builder->CreateAdd(pIdx,
                            llvm::ConstantInt::get(getDefaultType(), 1), "matm.np",
                            /*HasNUW=*/true, /*HasNSW=*/true);
                        pIdx->addIncoming(np, builder->GetInsertBlock());
                        builder->CreateBr(pLoopBB);
                    });
                llvm::Value* nj = builder->CreateAdd(jIdx,
                    llvm::ConstantInt::get(getDefaultType(), 1), "matm.nj",
                    /*HasNUW=*/true, /*HasNSW=*/true);
                jIdx->addIncoming(nj, builder->GetInsertBlock());
                builder->CreateBr(jLoopBB);
            });
        arrayReturningFunctions_.insert("mat_mul");
        return builder->CreatePtrToInt(cBuf, getDefaultType(), "matm.result");
    }

    if (inOptMaxFunction) {
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

    // Short-circuit calls to zero-parameter pure functions whose return value
    // is a compile-time constant (classified by analyzeConstantReturnValues).
    if (expr->arguments.empty() && !inOptMaxFunction && optCtx_) {
        if (auto iv = optCtx_->constIntReturn(expr->callee)) {
            auto* ci = llvm::ConstantInt::get(getDefaultType(), *iv);
            if (*iv >= 0) nonNegValues_.insert(ci);
            return ci;
        }
        if (auto sv = optCtx_->constStringReturn(expr->callee)) {
            llvm::GlobalVariable* gv = internString(*sv);
            stringReturningFunctions_.insert(expr->callee);
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                gv->getValueType(), gv,
                llvm::ArrayRef<llvm::Constant*>{
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
    }

    // Multi-param constant folding: if all arguments are compile-time constants
    // and the function body is pure-foldable, evaluate entirely at compile time.
    if (!inOptMaxFunction && !expr->arguments.empty() &&
        optimizationLevel >= OptimizationLevel::O1) {
        auto declIt2 = functionDecls_.find(expr->callee);
        if (declIt2 != functionDecls_.end() && declIt2->second->body &&
            expr->arguments.size() == declIt2->second->parameters.size()) {

            // Try CF-CTRE engine first (richer cross-function evaluation).
            if (ctEngine_ && ctEngine_->isPure(expr->callee)) {
                std::vector<CTValue> ctArgs;
                ctArgs.reserve(expr->arguments.size());
                bool allConst = true;
                bool anyConst = false;
                for (auto& arg : expr->arguments) {
                    auto cv = tryFoldExprToConst(arg.get());
                    if (!cv) {
                        allConst = false;
                        ctArgs.push_back(CTValue::symbolic()); // use symbolic for unknown args
                    } else {
                        ctArgs.push_back(constValueToCTValue(*cv));
                        anyConst = true;
                    }
                }
                // Full evaluation (all args known): normal path.
                // Partial evaluation (some args symbolic): attempt path-sensitive folding.
                // A symbolic arg means "we don't know the value"; executeFunction will
                // propagate SYMBOLIC through the body and return a CONCRETE result if
                // an early-exit guard based on the known args fires before the symbolic
                // value is needed (e.g. `if n == 0 { return 0 }` with n=0).
                if (allConst || anyConst) {
                    auto ctResult = ctEngine_->executeFunction(expr->callee, ctArgs);
                    if (ctResult) {
                        if (ctResult->isInt()) {
                            auto* ci = llvm::ConstantInt::get(getDefaultType(), ctResult->asI64());
                            if (ctResult->asI64() >= 0) nonNegValues_.insert(ci);
                            return ci;
                        }
                        if (ctResult->isString()) {
                            llvm::GlobalVariable* gv = internString(ctResult->asStr());
                            stringReturningFunctions_.insert(expr->callee);
                            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                                gv->getValueType(), gv,
                                llvm::ArrayRef<llvm::Constant*>{
                                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
                        }
                        if (ctResult->isArray()) {
                            arrayReturningFunctions_.insert(expr->callee);
                            auto cv = ctValueToConstValue(*ctResult);
                            return emitComptimeArray(cv.arrVal);
                        }
                    }
                }
            }

            std::unordered_map<std::string, ConstValue> argEnv;
            bool allConst = true;
            for (size_t i = 0; i < expr->arguments.size(); ++i) {
                auto cv = tryFoldExprToConst(expr->arguments[i].get());
                if (!cv) { allConst = false; break; }
                argEnv[declIt2->second->parameters[i].name] = *cv;
            }
            if (allConst) {
                auto result = tryConstEvalFull(declIt2->second, argEnv);
                if (result) {
                    if (result->kind == ConstValue::Kind::Integer) {
                        auto* ci = llvm::ConstantInt::get(getDefaultType(), result->intVal);
                        if (result->intVal >= 0) nonNegValues_.insert(ci);
                        return ci;
                    }
                    // String result: intern and return a GEP pointer
                    if (result->kind == ConstValue::Kind::String) {
                        llvm::GlobalVariable* gv = internString(result->strVal);
                        stringReturningFunctions_.insert(expr->callee);
                        return llvm::ConstantExpr::getInBoundsGetElementPtr(
                            gv->getValueType(), gv,
                            llvm::ArrayRef<llvm::Constant*>{
                                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
                    }
                    // Array result: emit as a private global constant and
                    // return the base pointer as i64.  Marks the callee as
                    // array-returning so downstream analysis tracks it correctly.
                    if (result->kind == ConstValue::Kind::Array) {
                        arrayReturningFunctions_.insert(expr->callee);
                        return emitComptimeArray(result->arrVal);
                    }
                }
            }
        }
    }

    // ── Integer / wide-integer type-cast syntax ────────────────────────────
    // iN(x) and uN(x) for N in [1..256], plus "int", "uint", "bool".
    // Replaces the old fixed kIntTypeCasts set.
    {
        const std::string& cn = expr->callee;
        unsigned castBits = 0;
        bool castUnsigned = false;
        // Parse "iN" / "uN" pattern
        if (cn.size() >= 2 && (cn[0] == 'i' || cn[0] == 'u')) {
            bool allDigits = true;
            int n = 0;
            for (size_t j = 1; j < cn.size(); ++j) {
                if (!std::isdigit(static_cast<unsigned char>(cn[j]))) { allDigits = false; break; }
                n = n * 10 + (cn[j] - '0');
                if (n > 256) { allDigits = false; break; }
            }
            if (allDigits && n >= 1 && n <= 256) {
                castBits = static_cast<unsigned>(n);
                castUnsigned = (cn[0] == 'u');
            }
        }
        // Accept "int" / "uint" as aliases for i64/u64
        if (cn == "int")  { castBits = 64; castUnsigned = false; }
        if (cn == "uint") { castBits = 64; castUnsigned = true;  }
        if (cn == "bool") { castBits = 1;  castUnsigned = true;  }

        if (castBits >= 1 && expr->arguments.size() == 1) {
            llvm::Value* arg = generateExpression(expr->arguments[0].get());

            // Normalise input to a scalar integer (pointer/float → i64 first).
            if (arg->getType()->isPointerTy())
                arg = builder->CreatePtrToInt(arg, getDefaultType(), "cast.ptoi");
            else if (arg->getType()->isDoubleTy())
                arg = builder->CreateFPToSI(arg, getDefaultType(), "cast.ftoi");

            const unsigned srcBits = arg->getType()->isIntegerTy()
                ? arg->getType()->getIntegerBitWidth() : 64u;
            auto* destTy = llvm::IntegerType::get(*context, castBits);

            if (castBits == 1) {
                // bool(x): normalise to 0 or 1
                auto* zero = llvm::ConstantInt::get(arg->getType(), 0);
                auto* cmp  = builder->CreateICmpNE(arg, zero, "bool.cmp");
                // Return as i64 (widen back for default-type context)
                return builder->CreateZExt(cmp, getDefaultType(), "bool.zext");
            }

            if (castBits == srcBits) {
                // Same width: identity (includes i64(x), u64(x), int(x), uint(x))
                return arg;
            }

            if (castBits > srcBits) {
                // Widen: ZExt for unsigned, SExt for signed
                if (castUnsigned)
                    return builder->CreateZExt(arg, destTy, cn + ".zext");
                else
                    return builder->CreateSExt(arg, destTy, cn + ".sext");
            }

            // Narrow: castBits < srcBits
            if (castBits <= 64 && srcBits <= 64) {
                // Both fit in i64 — keep result as i64 for backward compat
                if (castUnsigned) {
                    // uN(x): AND off the high bits, return as i64
                    const uint64_t mask = castBits == 64 ? UINT64_MAX
                                        : (UINT64_C(1) << castBits) - 1u;
                    auto* maskVal = llvm::ConstantInt::get(getDefaultType(), mask);
                    // Ensure arg is i64 for the AND
                    if (arg->getType() != getDefaultType())
                        arg = builder->CreateZExt(arg, getDefaultType(), "cast.zext64");
                    return builder->CreateAnd(arg, maskVal, cn + ".and");
                } else {
                    // iN(x): trunc to iN, sext back to i64
                    if (arg->getType() != getDefaultType())
                        arg = builder->CreateSExt(arg, getDefaultType(), "cast.sext64");
                    auto* trunc = builder->CreateTrunc(arg, destTy, cn + ".trunc");
                    return builder->CreateSExt(trunc, getDefaultType(), cn + ".sext");
                }
            }

            // Wide → narrow OR wide → wide: produce value at destTy width
            return builder->CreateTrunc(arg, destTy, cn + ".trunc");
        }
    }

    auto calleeIt = functions.find(expr->callee);
    if (calleeIt == functions.end() || !calleeIt->second) {
        // Build "did you mean?" suggestion from both user-defined functions
        // AND all built-in names so that typos like "abss" → "abs" work.
        std::string msg = "Unknown function: " + expr->callee;
        std::vector<std::string> candidates;
        candidates.reserve(functions.size() + builtinLookup.size());
        for (const auto& kv : functions) {
            if (kv.second)
                candidates.push_back(kv.getKey().str());
        }
        for (const auto& kv : builtinLookup)
            candidates.emplace_back(kv.first);
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
    // Emit !range metadata when a narrowed value range is known for this callee.
    // This allows LLVM's CVP/LVI passes to propagate tighter bounds into callers
    // without requiring inlining.  Only applicable to i64-returning functions.
    if (optCtx_ && callee->getReturnType()->isIntegerTy(64)) {
        if (auto rng = optCtx_->returnRange(expr->callee)) {
            if (rng->isNarrowed() && !rng->isEmpty() &&
                rng->hi < std::numeric_limits<int64_t>::max()) {
                llvm::Type* i64 = getDefaultType();
                auto* rangeMD = llvm::MDNode::get(*context, {
                    llvm::ConstantAsMetadata::get(
                        llvm::ConstantInt::get(i64, rng->lo, /*isSigned=*/true)),
                    llvm::ConstantAsMetadata::get(
                        llvm::ConstantInt::get(i64, rng->hi + 1, /*isSigned=*/true))
                });
                callResult->setMetadata(llvm::LLVMContext::MD_range, rangeMD);
            }
        }
    }
    // Propagate non-negativity from the callee: if the callee's return value
    // has !range [0, INT64_MAX) metadata, mark the result as non-negative.
    if (callee->hasRetAttribute(llvm::Attribute::ZExt))
        nonNegValues_.insert(callResult);
    return callResult;
}

} // namespace omscript
