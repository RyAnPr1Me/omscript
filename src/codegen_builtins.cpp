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
#ifdef __GNUC__
#pragma GCC optimize("O3,unroll-loops,tree-vectorize")
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

enum class BuiltinId : uint8_t {
    NONE = 0, // Not a builtin — fall through to user-defined function lookup
    PRINT,
    ABS,
    LEN,
    MIN,
    MAX,
    SIGN,
    CLAMP,
    POW,
    PRINT_CHAR,
    INPUT,
    INPUT_LINE,
    SQRT,
    FAST_ADD,
    FAST_SUB,
    FAST_MUL,
    FAST_DIV,
    PRECISE_ADD,
    PRECISE_SUB,
    PRECISE_MUL,
    PRECISE_DIV,
    IS_EVEN,
    IS_ODD,
    SUM,
    SWAP,
    REVERSE,
    TO_CHAR,
    IS_ALPHA,
    IS_DIGIT,
    TYPEOF,
    ASSERT,
    STR_LEN,
    CHAR_AT,
    STR_EQ,
    STR_CONCAT,
    LOG2,
    GCD,
    TO_STRING,
    STR_FIND,
    FLOOR,
    CEIL,
    ROUND,
    TO_INT,
    TO_FLOAT,
    STR_SUBSTR,
    STR_UPPER,
    STR_LOWER,
    STR_CONTAINS,
    STR_INDEX_OF,
    STR_REPLACE,
    STR_TRIM,
    STR_STARTS_WITH,
    STR_ENDS_WITH,
    STR_REPEAT,
    STR_REVERSE,
    PUSH,
    POP,
    INDEX_OF,
    ARRAY_CONTAINS,
    SORT,
    ARRAY_FILL,
    ARRAY_CONCAT,
    ARRAY_SLICE,
    ARRAY_COPY,
    ARRAY_REMOVE,
    ARRAY_MAP,
    ARRAY_FILTER,
    ARRAY_REDUCE,
    PRINTLN,
    WRITE,
    EXIT_PROGRAM,
    // Array shift / unshift: head removal and head insertion (O(n) in length).
    SHIFT,
    UNSHIFT,
    RANDOM,
    TIME,
    SLEEP,
    STR_TO_INT,
    STR_TO_FLOAT,
    STR_SPLIT,
    STR_CHARS,
    FILE_READ,
    FILE_WRITE,
    FILE_APPEND,
    FILE_EXISTS,
    MAP_NEW,
    MAP_SET,
    MAP_GET,
    MAP_HAS,
    MAP_REMOVE,
    MAP_KEYS,
    MAP_VALUES,
    MAP_SIZE,
    RANGE,
    RANGE_STEP,
    CHAR_CODE,
    NUMBER_TO_STRING,
    STRING_TO_NUMBER,
    THREAD_CREATE,
    THREAD_JOIN,
    MUTEX_NEW,
    MUTEX_LOCK,
    MUTEX_UNLOCK,
    MUTEX_DESTROY,
    SIN,
    COS,
    TAN,
    ASIN,
    ACOS,
    ATAN,
    ATAN2,
    EXP,
    LOG,
    LOG10,
    CBRT,
    HYPOT,
    ARRAY_MIN,
    ARRAY_MAX,
    ARRAY_ANY,
    ARRAY_EVERY,
    ARRAY_FIND,
    ARRAY_COUNT,
    STR_JOIN,
    STR_COUNT,
    POPCOUNT,
    CLZ,
    CTZ,
    BITREVERSE,
    EXP2,
    IS_POWER_OF_2,
    LCM,
    // New intrinsic builtins
    ROTATE_LEFT,
    ROTATE_RIGHT,
    BSWAP,
    SATURATING_ADD,
    SATURATING_SUB,
    FMA_BUILTIN,
    COPYSIGN,
    MIN_FLOAT,
    MAX_FLOAT,
    // Optimizer hint builtins
    ASSUME,
    UNREACHABLE,
    EXPECT,
    // Array utility builtins
    ARRAY_PRODUCT,
    ARRAY_LAST,
    ARRAY_INSERT,
    // String padding builtins
    STR_PAD_LEFT,
    STR_PAD_RIGHT,
    // Character classification predicates
    IS_UPPER,
    IS_LOWER,
    IS_SPACE,
    IS_ALNUM,
    // Generic filter (dispatches based on argument type)
    FILTER,
    // String character filter
    STR_FILTER,
    // Map entry filter
    MAP_FILTER,
    // Shell command execution
    COMMAND,
    // String: strip leading / trailing whitespace independently
    STR_LSTRIP,
    STR_RSTRIP,
    // String: remove all occurrences of a substring
    STR_REMOVE,
    // Array: take first n / drop first n elements
    ARRAY_TAKE,
    ARRAY_DROP,
    // Array: deduplicate consecutive equal elements
    ARRAY_UNIQUE,
    // Array: rotate left by n positions
    ARRAY_ROTATE,
    // Array: arithmetic mean (integer division)
    ARRAY_MEAN,
    // Map: merge two maps (b wins on conflict), invert keys↔values
    MAP_MERGE,
    MAP_INVERT,
    // Shell command execution with sudo (password provided as second arg)
    SUDO_COMMAND,
    // Environment variable access
    ENV_GET,
    ENV_SET,
    // String formatting: str_format(fmt, val1[, val2[, ...]]) via snprintf
    STR_FORMAT,
    // Array: interleave two arrays into [a[0],b[0], a[1],b[1], ...].
    // Result length = 2 * min(len(a), len(b)).
    ARRAY_ZIP,
    // Arbitrary-precision integer (bigint) builtins
    BIGINT_NEW,
    BIGINT_ADD,
    BIGINT_SUB,
    BIGINT_MUL,
    BIGINT_DIV,
    BIGINT_MOD,
    BIGINT_NEG,
    BIGINT_ABS,
    BIGINT_POW,
    BIGINT_GCD,
    BIGINT_EQ,
    BIGINT_LT,
    BIGINT_LE,
    BIGINT_GT,
    BIGINT_GE,
    BIGINT_CMP,
    BIGINT_TOSTRING,
    BIGINT_TO_I64,
    BIGINT_BIT_LENGTH,
    BIGINT_IS_ZERO,
    BIGINT_IS_NEGATIVE,
    BIGINT_SHL,
    BIGINT_SHR,
    // Type-specific fast arithmetic: upper half of 128-bit multiply,
    // overflow-safe absolute difference, reciprocal sqrt with fast-math.
    INT_MULHI,    ///< Signed   mulhi(a,b) → high 64 bits of i128 product
    UINT_MULHI,   ///< Unsigned mulhi_u(a,b) → high 64 bits of u128 product
    INT_ABSDIFF,  ///< absdiff(a,b) → |a-b| without overflow (widens to i128)
    FAST_SQRT,    ///< fast_sqrt(x) → sqrt with reassociate/nnan fast-math flags
    FLOAT_IS_NAN, ///< is_nan(x)  → 1 if x is NaN when reinterpreted as f64
    FLOAT_IS_INF, ///< is_inf(x)  → 1 if x is ±Infinity as f64
    // ── 2D column-major matrix builtins ─────────────────────────────────────
    MAT_NEW,    ///< mat_new(rows,cols)       → zero-filled column-major matrix
    MAT_FILL,   ///< mat_fill(rows,cols,val)  → column-major matrix filled with val
    MAT_GET,    ///< mat_get(m,i,j)           → element (i,j) of matrix m
    MAT_SET,    ///< mat_set(m,i,j,val)       → set element (i,j); returns m
    MAT_ROWS,   ///< mat_rows(m)              → number of rows
    MAT_COLS,   ///< mat_cols(m)              → number of columns
    MAT_MUL,    ///< mat_mul(a,b)             → C = A × B (column-major result)
    MAT_TRANSP, ///< mat_transp(m)            → transpose of m (new allocation)
    // ── Region management builtins (RLC pass support) ────────────────────────
    NEW_REGION, ///< newRegion()        → opaque region handle (malloc arena head)
    ALLOC,      ///< alloc(r, size)     → allocate `size` bytes in region r
    // ── Raw memory builtins ──────────────────────────────────────────────────
    MALLOC, ///< malloc(size)       → allocate `size` bytes, return ptr
    FREE,   ///< free(ptr)          → deallocate ptr (void return)
    SIZEOF, ///< sizeof(type_name)  → byte size of type as i64 constant
    // ── HTTP client builtins ─────────────────────────────────────────────────
    HTTP_GET,     ///< http_get(url)                   → response body string
    HTTP_POST,    ///< http_post(url, body[, ct])       → response body string
    HTTP_REQUEST, ///< http_request(method,url,body,hdr)→ response body string
    HTTP_STATUS,  ///< http_status(url)                → HTTP status code (int)
    // ── Function pointer builtins ────────────────────────────────────────────
    FUNCPTR_FROM, ///< funcptr_from(fn_name_str) → address of named fn as i64
    FUNCPTR_NEW,  ///< funcptr_new(byte_arr, n)  → executable-memory funcptr from raw bytes
    // ── Pointer slice builtins ───────────────────────────────────────────────
    PSLICE_LEN, ///< pslice_len(s)            → length of the slice
    PSLICE_PTR, ///< pslice_ptr(s)            → raw pointer from the slice
    // ── Generic boxing builtin ───────────────────────────────────────────────
    STORE_PTR, ///< store_ptr(value)          → box value onto stack, return ptr<typeof(value)>
    // ── Type introspection ───────────────────────────────────────────────────
    TYPE_NAME,  ///< type_name(expr)  → compile-time string describing the LLVM type of expr
    // ── Round-73 additions ───────────────────────────────────────────────────
    STR_IS_EMPTY,   ///< str_is_empty(s)       → 1 if s has length 0, else 0
    STR_CAPITALIZE, ///< str_capitalize(s)     → first char uppercased, rest lowercased
    ARRAY_FIRST,    ///< array_first(arr)      → first element (aborts on empty array)
    MAP_COPY,       ///< map_copy(d)           → shallow copy of a hashmap
    MAP_CLEAR,      ///< map_clear(d)          → return a fresh empty map
    // ── Round-74 additions ───────────────────────────────────────────────────
    ARRAY_SUM,      ///< array_sum(arr)        → sum of all integer elements
    ARRAY_SORTED,   ///< array_sorted(arr)     → sorted copy (non-mutating)
    ARRAY_REVERSE,  ///< array_reverse(arr)    → reversed copy
    STR_WORDS,      ///< str_words(s)          → split on whitespace → string[]
    STR_TITLE,      ///< str_title(s)          → title-case each word
    STR_SWAPCASE,   ///< str_swapcase(s)       → swap upper↔lower for each ASCII char
    // ── Round-75 additions ───────────────────────────────────────────────────
    ARRAY_FLATTEN,  ///< array_flatten(arr)    → flatten one level: int[][]→int[]
    ARRAY_MIN_BY,   ///< array_min_by(arr,fn)  → element with minimum key fn(elem)
    ARRAY_MAX_BY,   ///< array_max_by(arr,fn)  → element with maximum key fn(elem)
    STR_TO_LINES,   ///< str_to_lines(s)       → split on \n / \r\n → string[]
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
    {"shift", BuiltinId::SHIFT},
    {"unshift", BuiltinId::UNSHIFT},
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
    {"shell", BuiltinId::COMMAND}, // alias
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
    {"map_merge", BuiltinId::MAP_MERGE},
    {"map_invert", BuiltinId::MAP_INVERT},
    // Shell command execution with sudo (password provided as second arg)
    {"sudo_command", BuiltinId::SUDO_COMMAND},
    {"env_get", BuiltinId::ENV_GET},
    {"env_set", BuiltinId::ENV_SET},
    // String formatting
    {"str_format", BuiltinId::STR_FORMAT},
    {"array_zip", BuiltinId::ARRAY_ZIP},
    // bigint builtins
    {"bigint", BuiltinId::BIGINT_NEW},
    {"bigint_add", BuiltinId::BIGINT_ADD},
    {"bigint_sub", BuiltinId::BIGINT_SUB},
    {"bigint_mul", BuiltinId::BIGINT_MUL},
    {"bigint_div", BuiltinId::BIGINT_DIV},
    {"bigint_mod", BuiltinId::BIGINT_MOD},
    {"bigint_neg", BuiltinId::BIGINT_NEG},
    {"bigint_abs", BuiltinId::BIGINT_ABS},
    {"bigint_pow", BuiltinId::BIGINT_POW},
    {"bigint_gcd", BuiltinId::BIGINT_GCD},
    {"bigint_eq", BuiltinId::BIGINT_EQ},
    {"bigint_lt", BuiltinId::BIGINT_LT},
    {"bigint_le", BuiltinId::BIGINT_LE},
    {"bigint_gt", BuiltinId::BIGINT_GT},
    {"bigint_ge", BuiltinId::BIGINT_GE},
    {"bigint_cmp", BuiltinId::BIGINT_CMP},
    {"bigint_tostring", BuiltinId::BIGINT_TOSTRING},
    {"bigint_to_i64", BuiltinId::BIGINT_TO_I64},
    {"bigint_bit_length", BuiltinId::BIGINT_BIT_LENGTH},
    {"bigint_is_zero", BuiltinId::BIGINT_IS_ZERO},
    {"bigint_is_negative", BuiltinId::BIGINT_IS_NEGATIVE},
    {"bigint_shl", BuiltinId::BIGINT_SHL},
    {"bigint_shr", BuiltinId::BIGINT_SHR},
    // Type-specific fast builtins
    {"mulhi", BuiltinId::INT_MULHI},
    {"mulhi_u", BuiltinId::UINT_MULHI},
    {"absdiff", BuiltinId::INT_ABSDIFF},
    {"fast_sqrt", BuiltinId::FAST_SQRT},
    {"is_nan", BuiltinId::FLOAT_IS_NAN},
    {"is_inf", BuiltinId::FLOAT_IS_INF},
    // 2D column-major matrix builtins
    {"mat_new", BuiltinId::MAT_NEW},
    {"mat_fill", BuiltinId::MAT_FILL},
    {"mat_get", BuiltinId::MAT_GET},
    {"mat_set", BuiltinId::MAT_SET},
    {"mat_rows", BuiltinId::MAT_ROWS},
    {"mat_cols", BuiltinId::MAT_COLS},
    {"mat_mul", BuiltinId::MAT_MUL},
    {"mat_transp", BuiltinId::MAT_TRANSP},
    // Region management
    {"newRegion", BuiltinId::NEW_REGION},
    {"alloc", BuiltinId::ALLOC},
    // Raw memory
    {"malloc", BuiltinId::MALLOC},
    {"free", BuiltinId::FREE},
    {"sizeof", BuiltinId::SIZEOF},
    // HTTP client
    {"http_get", BuiltinId::HTTP_GET},
    {"http_post", BuiltinId::HTTP_POST},
    {"http_request", BuiltinId::HTTP_REQUEST},
    {"http_status", BuiltinId::HTTP_STATUS},
    {"funcptr_from", BuiltinId::FUNCPTR_FROM},
    {"funcptr_new", BuiltinId::FUNCPTR_NEW},
    {"pslice_len", BuiltinId::PSLICE_LEN},
    {"pslice_ptr", BuiltinId::PSLICE_PTR},
    {"store_ptr", BuiltinId::STORE_PTR},
    {"type_name", BuiltinId::TYPE_NAME},
    // Round-73
    {"str_is_empty",   BuiltinId::STR_IS_EMPTY},
    {"str_capitalize", BuiltinId::STR_CAPITALIZE},
    {"array_first",    BuiltinId::ARRAY_FIRST},
    {"map_copy",       BuiltinId::MAP_COPY},
    {"map_clear",      BuiltinId::MAP_CLEAR},
    // Round-74
    {"array_sum",      BuiltinId::ARRAY_SUM},
    {"array_sorted",   BuiltinId::ARRAY_SORTED},
    {"array_reverse",  BuiltinId::ARRAY_REVERSE},
    {"str_words",      BuiltinId::STR_WORDS},
    {"str_title",      BuiltinId::STR_TITLE},
    {"str_swapcase",   BuiltinId::STR_SWAPCASE},
    // Round-75
    {"array_flatten",  BuiltinId::ARRAY_FLATTEN},
    {"array_min_by",   BuiltinId::ARRAY_MIN_BY},
    {"array_max_by",   BuiltinId::ARRAY_MAX_BY},
    {"str_to_lines",   BuiltinId::STR_TO_LINES},
};

static BuiltinId lookupBuiltin(const std::string& name) {
    auto it = builtinLookup.find(std::string_view(name));
    return it != builtinLookup.end() ? it->second : BuiltinId::NONE;
}

// ---------------------------------------------------------------------------
// Compile-time constant folding helper
void CodeGenerator::validateArgCount(const CallExpr* expr, const std::string& funcName, size_t expected) {
    if (expr->arguments.size() != expected) {
        codegenError("Built-in function '" + funcName + "' expects " + std::to_string(expected) + " argument(s), but " +
                         std::to_string(expr->arguments.size()) + " provided",
                     expr);
    }
}

// ── extractFnName ──────────────────────────────────────────────────────────
// Extract a function name from an expression that is either:
//   - LiteralExpr(STRING)  — the traditional lambda-as-string form
//   - IdentifierExpr       — the modern lambda-as-identifier form (IdentifierExpr)
// Returns an empty string if the expression does not name a function.
static std::string extractFnName(const Expression* arg) {
    if (!arg)
        return "";
    if (arg->type == ASTNodeType::LITERAL_EXPR) {
        const auto* lit = static_cast<const LiteralExpr*>(arg);
        if (lit->literalType == LiteralExpr::LiteralType::STRING)
            return lit->stringValue;
    } else if (arg->type == ASTNodeType::IDENTIFIER_EXPR) {
        return static_cast<const IdentifierExpr*>(arg)->name;
    }
    return "";
}

llvm::Value* CodeGenerator::generateCall(CallExpr* expr) {
    // O(1) hash map lookup replaces the previous linear chain of ~80
    // If a user-defined function has the same name as a builtin, the
    // user-defined function takes priority (allows test-time overrides).
    const bool hasUserDef = functions.count(expr->callee) &&
                            functions.find(expr->callee)->second != nullptr;
    const BuiltinId bid = hasUserDef ? BuiltinId::NONE : lookupBuiltin(expr->callee);

    // ── Mandatory namespace enforcement ──────────────────────────────────────
    // Unless the source file has `import std;`, stdlib functions must be called
    // as `std::funcname(...)`.  Bare calls like `println(x)` are rejected.
    // This only applies when:
    //  - the call is to a known stdlib function name (isStdlibFunction)
    //  - the call was NOT written as std::foo() (fromStdNamespace == false)
    //  - the source file has NOT done `import std;` (stdImported_ == false)
    //  - there is NO user-defined function with the same bare name
    if (!expr->fromStdNamespace && !stdImported_ && bid != BuiltinId::NONE) {
        // Only block if there is no user-defined override with the same name.
        bool hasUserDef = functions.count(expr->callee) && functions.find(expr->callee)->second != nullptr;
        if (!hasUserDef && isStdlibFunction(expr->callee)) {
            codegenError("Function '" + expr->callee +
                             "' requires namespace qualification.\n"
                             "  Use 'std::" +
                             expr->callee +
                             "(...)' or add 'import std;' "
                             "at the top of the file.",
                         expr);
        }
    }
    // ─────────────────────────────────────────────────────────────────────────

    // ── pslice_new<T>(ptr, len) — create a fat pointer slice ─────────────────
    // Bundles a raw pointer with a length for compile-time bounds checking.
    // Usage:  var s: pslice<i64> = pslice_new<i64>(raw_ptr, count)
    //
    // Returns the raw pointer as a native LLVM pointer (no ptrtoint) so that
    // LLVM alias analysis can track provenance through the stored pointer.
    // The length is stashed in lastPsliceNewLen_ so VarDecl can populate
    // the companion len alloca.
    if (expr->callee.size() > 11 && expr->callee.rfind("pslice_new<", 0) == 0 && expr->callee.back() == '>') {
        validateArgCount(expr, "pslice_new<T>", 2);
        llvm::Value* ptrVal = generateExpression(expr->arguments[0].get());
        llvm::Value* lenVal = generateExpression(expr->arguments[1].get());
        lenVal = toDefaultType(lenVal);
        // Keep the value as a native pointer — avoids ptrtoint which would
        // break alias analysis.  Integer addresses are wrapped via inttoptr.
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        if (!ptrVal->getType()->isPointerTy())
            ptrVal = builder->CreateIntToPtr(toDefaultType(ptrVal), ptrTy, "pslice.new.itoptr");
        // Stash the length so VarDecl can store it into the companion alloca.
        lastPsliceNewLen_ = lenVal;
        return ptrVal;
    }

    // ── alloc<T>(x) — compile-time smart allocator ───────────────────────────
    // Three compile-time tiers; no runtime branches are emitted:
    //
    //  T1 (stack alloca):  count is a ConstantInt AND
    //                      count*sizeof(T) <= kStackAllocThreshold (8 KiB).
    //                      Alloca is placed in the function entry block so
    //                      mem2reg / SROA can reason about it universally.
    //
    //  T2 (static arena):  count is a ConstantInt AND
    //                      count*sizeof(T) > kStackAllocThreshold AND
    //                      remaining arena capacity >= count*sizeof(T).
    //                      All T2 allocations in a function share a single
    //                      static stack alloca slab (emitted once in entry
    //                      block).  Sub-allocation is a compile-time-constant
    //                      GEP.  lifetime.end is emitted at function exit.
    //
    //  T3 (heap malloc):   dynamic count OR compile-time count whose size
    //                      exceeds the remaining arena capacity.
    //                      Falls back to malloc / aligned_alloc.
    if (expr->callee.size() > 6 && expr->callee.rfind("alloc<", 0) == 0 && expr->callee.back() == '>') {
        const std::string elemTypeName = expr->callee.substr(6, expr->callee.size() - 7);
        llvm::Type* elemTy = resolveAnnotatedType(elemTypeName);
        // resolveAnnotatedType returns an opaque ptr for struct types (used for variable
        // storage), but alloc<T> needs the real struct LLVM type so that sizeof(T) and the
        // stack/arena/heap allocations are sized correctly.  Without this, alloc<Point>(1)
        // allocates 8 bytes (ptr size) when Point = {i64, i64} requires 16 bytes, causing
        // the second field store to write past the allocation → undefined behaviour.
        if (llvm::StructType* sty = getOrCreateStructLLVMType(elemTypeName))
            elemTy = sty;
        const llvm::DataLayout& DL = module->getDataLayout();
        const uint64_t elemSize = DL.getTypeAllocSize(elemTy);
        const llvm::Align elemAlign = DL.getABITypeAlign(elemTy);

        if (expr->arguments.size() > 1)
            codegenError("alloc<T> expects zero or one argument (element count); "
                         "alloc<T>() allocates one element, alloc<T>(n) allocates n elements",
                         expr);

        // Reset side-channels for the upcoming VarDecl registration.
        lastStackAllocBacking_ = nullptr;
        lastAllocWasArena_ = false;

        // Zero arguments: allocate exactly one element (Ω spec §4.1: alloc<T>()).
        llvm::Value* countVal;
        if (expr->arguments.empty()) {
            countVal = llvm::ConstantInt::get(getDefaultType(), 1);
        } else {
            countVal = generateExpression(expr->arguments[0].get());
            countVal = builder->CreateIntCast(countVal, getDefaultType(),
                                              /*isSigned=*/true, "alloc.cnt");
        }

        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(countVal)) {
            const uint64_t count = ci->getZExtValue();
            if (count == 0) {
                // Zero-element allocation: return a null pointer without
                // emitting any heap or stack allocation.
                return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
            }
            const uint64_t totalBytes = count * elemSize;
            // Align up to the element's ABI alignment so arena sub-allocations
            // satisfy alignment requirements.
            const uint64_t alignVal = elemAlign.value();
            const uint64_t alignedBytes = (totalBytes + alignVal - 1u) & ~(alignVal - 1u);

            // ── Tier 1: stack alloca ──────────────────────────────────────────
            if (totalBytes <= kStackAllocThreshold) {
                // Place the alloca in the function entry block so mem2reg /
                // SROA can reason about it across all basic blocks.
                llvm::Function* fn = builder->GetInsertBlock()->getParent();
                llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
                auto* arrayTy = llvm::ArrayType::get(elemTy, count);
                auto* stackAlloc = entryB.CreateAlloca(arrayTy, nullptr, "stkalloc." + elemTypeName);
                stackAlloc->setAlignment(elemAlign);

                // Emit lifetime.start to scope the allocation tightly and
                // allow DSE/LICM to treat it as a bounded object.
                const uint64_t lifeSz = DL.getTypeAllocSize(arrayTy);
                auto* lifeSzVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), lifeSz);
                auto* lifetimeStart = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::lifetime_start,
                                                         {llvm::PointerType::getUnqual(*context)});
                builder->CreateCall(lifetimeStart, {lifeSzVal, stackAlloc});

                // Return pointer to first element (GEP through [N x T]* → T*).
                auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0);
                auto* ptr = builder->CreateInBoundsGEP(arrayTy, stackAlloc, {zero, zero}, "stkalloc.ptr");
                // Record the backing alloca so invalidate can end its lifetime.
                lastStackAllocBacking_ = stackAlloc;
                return ptr;
            }

            // ── Tier 2: per-function arena sub-allocation (static stack) ─────
            // Use when the compile-time allocation fits in the remaining arena
            // capacity.  The arena slab is a single entry-block alloca shared
            // by all T2 allocations; no heap involvement whatsoever.
            if (alignedBytes <= kFuncArenaSlabSize && funcArenaUsedBytes_ + alignedBytes <= kFuncArenaSlabSize) {
                llvm::Function* fn = builder->GetInsertBlock()->getParent();
                llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
                auto* ptrTy = llvm::PointerType::getUnqual(*context);
                auto* i8Ty = llvm::Type::getInt8Ty(*context);
                auto* i64Ty = llvm::Type::getInt64Ty(*context);

                // Lazily emit the arena slab as a static stack alloca.
                if (!funcArenaBaseAlloca_) {
                    auto* slabArrayTy = llvm::ArrayType::get(i8Ty, kFuncArenaSlabSize);
                    funcArenaBaseAlloca_ = entryB.CreateAlloca(slabArrayTy, nullptr, "arena.slab");
                    // 16-byte alignment satisfies all scalar and SIMD types.
                    funcArenaBaseAlloca_->setAlignment(llvm::Align(16));
                    // lifetime.start scopes the slab for DSE/LICM and stack
                    // coloring — the optimizer may overlap non-live slabs.
                    auto* lifeSzVal = llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(kFuncArenaSlabSize));
                    auto* lifetimeStart = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::lifetime_start, {ptrTy});
                    entryB.CreateCall(lifetimeStart, {lifeSzVal, funcArenaBaseAlloca_});
                }

                // Sub-allocate at a compile-time-constant byte offset.
                const uint64_t offset = funcArenaUsedBytes_;
                funcArenaUsedBytes_ += alignedBytes;

                // GEP directly into the slab alloca — no load needed.
                // Recompute the array type from the constant rather than casting
                // funcArenaBaseAlloca_->getAllocatedType() to avoid a redundant cast.
                auto* slabArrayTy = llvm::ArrayType::get(i8Ty, kFuncArenaSlabSize);
                auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0);
                llvm::Value* arenaBase =
                    builder->CreateInBoundsGEP(slabArrayTy, funcArenaBaseAlloca_, {zero32, zero32}, "arena.base");
                llvm::Value* slotPtr = builder->CreateInBoundsGEP(
                    i8Ty, arenaBase, llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(offset)),
                    "arena.slot." + elemTypeName);
                // Cast to the element pointer type for type safety.
                llvm::Value* typedPtr =
                    (elemTy != i8Ty) ? builder->CreatePointerCast(slotPtr, ptrTy, "arena.ptr") : slotPtr;
                lastAllocWasArena_ = true;
                return typedPtr;
            }
        }

        // ── Tier 3: heap malloc ───────────────────────────────────────────────
        // Dynamic count or size exceeded arena capacity.
        llvm::Value* totalSize =
            builder->CreateMul(countVal, llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(elemSize)),
                               "halloc.sz", /*HasNUW=*/true);

        llvm::Value* heapPtr;
        if (elemAlign.value() > 16) {
            // Round totalSize up to the next multiple of alignment so that
            // aligned_alloc's precondition (size % align == 0) is satisfied.
            const uint64_t alignBytes = elemAlign.value();
            llvm::Value* alignV = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(alignBytes));
            // roundUp(sz, a) = (sz + a - 1) & ~(a - 1)  — valid since a is pow2.
            llvm::Value* alignMinus1 = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(alignBytes - 1));
            llvm::Value* mask = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(~(alignBytes - 1)));
            llvm::Value* roundedSize = builder->CreateAnd(
                builder->CreateAdd(totalSize, alignMinus1, "halloc.roundup", /*HasNUW=*/false, /*HasNSW=*/false), mask,
                "halloc.rounded");
            heapPtr = builder->CreateCall(getOrDeclareAlignedAlloc(), {alignV, roundedSize}, "halloc.ptr");
        } else {
            heapPtr = builder->CreateCall(getOrDeclareMalloc(), {totalSize}, "halloc.ptr");
        }
        // Annotate the heap pointer with nonnull + dereferenceable (when size is
        // known at compile time).  This allows LLVM's alias analysis, load-store
        // forwarding, and inliner to reason about the returned memory without
        // emitting any runtime overhead.
        if (auto* ci = llvm::dyn_cast<llvm::CallInst>(heapPtr)) {
            ci->addRetAttr(llvm::Attribute::NonNull);
            // When count is a compile-time constant, the dereferenceable byte
            // count is also known at compile time.
            if (auto* cntCi = llvm::dyn_cast<llvm::ConstantInt>(countVal)) {
                const uint64_t derefBytes = cntCi->getZExtValue() * elemSize;
                if (derefBytes > 0)
                    ci->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, derefBytes));
            }
            // Emit llvm.assume(ptr != null) so that GVN / IndVars / LICM can
            // treat any load/store through heapPtr as non-speculative.
            auto* notNull = builder->CreateIsNotNull(heapPtr, "halloc.nonnull");
            auto* assumeFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::assume, {});
            builder->CreateCall(assumeFn, {notNull});
            // Emit llvm.assume(alignmentok) to let the vectorizer and load/store
            // legalization use the strongest safe alignment.
            const uint64_t alignBytes = elemAlign.value();
            if (alignBytes >= 2) {
                // alignmentok: (ptr & (alignBytes-1)) == 0
                auto* ptrAsInt = builder->CreatePtrToInt(heapPtr, getDefaultType(), "halloc.ptrint");
                auto* alignMask = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(alignBytes - 1));
                auto* andVal = builder->CreateAnd(ptrAsInt, alignMask, "halloc.alignchk");
                auto* zero64 = llvm::ConstantInt::get(getDefaultType(), 0);
                auto* alignOk = builder->CreateICmpEQ(andVal, zero64, "halloc.aligned");
                builder->CreateCall(assumeFn, {alignOk});
            }
        }
        return heapPtr;
    }

    // ── new_zero<T>(n) — zero-initialised allocation (new T(n) lowers here) ──
    // Identical to alloc<T>(n) except that the returned memory is guaranteed
    // to be zero-filled:
    //   T1 / T2 (stack / arena): same alloc path + CreateMemSet(ptr,0,size,align)
    //   T3 (heap):                calloc(count, sizeof(T)) — OS-zeroed, no runtime overhead
    if (expr->callee.size() > 9 && expr->callee.rfind("new_zero<", 0) == 0 && expr->callee.back() == '>') {
        const std::string elemTypeName = expr->callee.substr(9, expr->callee.size() - 10);
        llvm::Type* elemTy = resolveAnnotatedType(elemTypeName);
        // Same struct-size fix as alloc<T>: use the actual struct LLVM type so that
        // sizeof(T) is computed correctly for struct element types.
        llvm::StructType* structTy = getOrCreateStructLLVMType(elemTypeName);
        if (structTy)
            elemTy = structTy;
        const llvm::DataLayout& DL = module->getDataLayout();
        const uint64_t elemSize = DL.getTypeAllocSize(elemTy);
        const llvm::Align elemAlign = DL.getABITypeAlign(elemTy);

        if (expr->arguments.size() > 1)
            codegenError("new T expects zero or one argument (element count); "
                         "new T allocates one zero-initialised element, new T(n) allocates n",
                         expr);

        lastStackAllocBacking_ = nullptr;
        lastAllocWasArena_ = false;

        llvm::Value* countVal;
        if (expr->arguments.empty()) {
            countVal = llvm::ConstantInt::get(getDefaultType(), 1);
        } else {
            countVal = generateExpression(expr->arguments[0].get());
            countVal = builder->CreateIntCast(countVal, getDefaultType(),
                                              /*isSigned=*/true, "newz.cnt");
        }

        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(countVal)) {
            const uint64_t count = ci->getZExtValue();
            if (count == 0)
                return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));

            const uint64_t totalBytes = count * elemSize;
            const uint64_t alignVal = elemAlign.value();
            const uint64_t alignedBytes = (totalBytes + alignVal - 1u) & ~(alignVal - 1u);

            // T1: stack alloca + zero-init
            if (totalBytes <= kStackAllocThreshold) {
                llvm::Function* fn = builder->GetInsertBlock()->getParent();
                llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
                auto* arrayTy = llvm::ArrayType::get(elemTy, count);
                auto* stackAlloc = entryB.CreateAlloca(arrayTy, nullptr, "newz.stkalloc." + elemTypeName);
                stackAlloc->setAlignment(elemAlign);
                const uint64_t lifeSz = DL.getTypeAllocSize(arrayTy);
                auto* lifeSzVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), lifeSz);
                auto* lifetimeStart = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::lifetime_start,
                                                         {llvm::PointerType::getUnqual(*context)});
                builder->CreateCall(lifetimeStart, {lifeSzVal, stackAlloc});
                auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0);
                auto* ptr = builder->CreateInBoundsGEP(arrayTy, stackAlloc, {zero32, zero32}, "newz.stkptr");
                // For struct types: auto-construct each element with typed per-field
                // zero stores (with TBAA metadata) instead of a flat memset.  This
                // lets the optimizer see explicit field writes and respects field types.
                if (structTy) {
                    const unsigned numFields = structTy->getNumElements();
                    for (uint64_t elem = 0; elem < count; ++elem) {
                        llvm::Value* elemBase =
                            (count == 1) ? ptr
                                         : builder->CreateInBoundsGEP(
                                               structTy, ptr,
                                               llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(elem)),
                                               "newz.elem." + std::to_string(elem));
                        for (unsigned fi = 0; fi < numFields; ++fi) {
                            llvm::Type* fty = structTy->getElementType(fi);
                            llvm::Value* fptr =
                                builder->CreateStructGEP(structTy, elemBase, fi, "newz.field." + std::to_string(fi));
                            llvm::StoreInst* st = builder->CreateAlignedStore(llvm::Constant::getNullValue(fty), fptr,
                                                                              DL.getABITypeAlign(fty));
                            st->setMetadata(llvm::LLVMContext::MD_tbaa, getOrCreateFieldTBAA(elemTypeName, fi));
                        }
                    }
                } else {
                    // Scalar / non-struct type: flat memset is optimal.
                    builder->CreateMemSet(ptr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0),
                                          llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(totalBytes)),
                                          elemAlign);
                }
                lastStackAllocBacking_ = stackAlloc;
                return ptr;
            }

            // T2: arena sub-allocation + memset zero
            if (alignedBytes <= kFuncArenaSlabSize && funcArenaUsedBytes_ + alignedBytes <= kFuncArenaSlabSize) {
                llvm::Function* fn = builder->GetInsertBlock()->getParent();
                llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
                auto* ptrTy = llvm::PointerType::getUnqual(*context);
                auto* i8Ty = llvm::Type::getInt8Ty(*context);
                auto* i64Ty = llvm::Type::getInt64Ty(*context);
                if (!funcArenaBaseAlloca_) {
                    auto* slabArrayTy = llvm::ArrayType::get(i8Ty, kFuncArenaSlabSize);
                    funcArenaBaseAlloca_ = entryB.CreateAlloca(slabArrayTy, nullptr, "arena.slab");
                    funcArenaBaseAlloca_->setAlignment(llvm::Align(16));
                    auto* lifeSzVal = llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(kFuncArenaSlabSize));
                    auto* lifetimeStart = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::lifetime_start, {ptrTy});
                    entryB.CreateCall(lifetimeStart, {lifeSzVal, funcArenaBaseAlloca_});
                }
                const uint64_t offset = funcArenaUsedBytes_;
                funcArenaUsedBytes_ += alignedBytes;
                auto* slabArrayTy = llvm::ArrayType::get(i8Ty, kFuncArenaSlabSize);
                auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0);
                llvm::Value* arenaBase =
                    builder->CreateInBoundsGEP(slabArrayTy, funcArenaBaseAlloca_, {zero32, zero32}, "arena.base");
                llvm::Value* slotPtr = builder->CreateInBoundsGEP(
                    i8Ty, arenaBase, llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(offset)),
                    "newz.arena.slot." + elemTypeName);
                llvm::Value* typedPtr =
                    (elemTy != i8Ty) ? builder->CreatePointerCast(slotPtr, ptrTy, "newz.arena.ptr") : slotPtr;
                // For struct types: auto-construct each element with typed per-field
                // zero stores (with TBAA metadata) instead of a flat memset.
                if (structTy) {
                    const unsigned numFields = structTy->getNumElements();
                    for (uint64_t elem = 0; elem < count; ++elem) {
                        llvm::Value* elemBase =
                            (count == 1) ? typedPtr
                                         : builder->CreateInBoundsGEP(
                                               structTy, typedPtr,
                                               llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(elem)),
                                               "newz.arena.elem." + std::to_string(elem));
                        for (unsigned fi = 0; fi < numFields; ++fi) {
                            llvm::Type* fty = structTy->getElementType(fi);
                            llvm::Value* fptr = builder->CreateStructGEP(structTy, elemBase, fi,
                                                                         "newz.arena.field." + std::to_string(fi));
                            llvm::StoreInst* st = builder->CreateAlignedStore(llvm::Constant::getNullValue(fty), fptr,
                                                                              DL.getABITypeAlign(fty));
                            st->setMetadata(llvm::LLVMContext::MD_tbaa, getOrCreateFieldTBAA(elemTypeName, fi));
                        }
                    }
                } else {
                    builder->CreateMemSet(typedPtr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0),
                                          llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(totalBytes)), elemAlign);
                }
                lastAllocWasArena_ = true;
                return typedPtr;
            }
        }

        // T3: heap — use calloc(count, sizeof(T)) for OS-level zero-init (no extra memset).
        auto* callocFn = getOrDeclareCalloc();
        auto* elemSzVal = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(elemSize));
        llvm::Value* heapPtr = builder->CreateCall(callocFn, {countVal, elemSzVal}, "newz.halloc.ptr");
        if (auto* ci = llvm::dyn_cast<llvm::CallInst>(heapPtr)) {
            ci->addRetAttr(llvm::Attribute::NonNull);
            if (auto* cntCi = llvm::dyn_cast<llvm::ConstantInt>(countVal)) {
                const uint64_t derefBytes = cntCi->getZExtValue() * elemSize;
                if (derefBytes > 0)
                    ci->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, derefBytes));
            }
            auto* notNull = builder->CreateIsNotNull(heapPtr, "newz.halloc.nonnull");
            auto* assumeFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::assume, {});
            builder->CreateCall(assumeFn, {notNull});
            const uint64_t alignBytes = elemAlign.value();
            if (alignBytes >= 2) {
                auto* ptrAsInt = builder->CreatePtrToInt(heapPtr, getDefaultType(), "newz.halloc.ptrint");
                auto* alignMask = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(alignBytes - 1));
                auto* andVal = builder->CreateAnd(ptrAsInt, alignMask, "newz.halloc.alignchk");
                auto* zero64 = llvm::ConstantInt::get(getDefaultType(), 0);
                auto* alignOk = builder->CreateICmpEQ(andVal, zero64, "newz.halloc.aligned");
                builder->CreateCall(assumeFn, {alignOk});
            }
        }
        return heapPtr;
    }

    // ── Width-typed integer intrinsics (__tw_<op>_<N>) ───────────────────────
    if (expr->callee.size() > 5 && expr->callee.substr(0, 5) == "__tw_") {
        // Parse: __tw_<opname>_<width>
        const std::string suffix = expr->callee.substr(5); // e.g. "popcount_32"
        const auto uscore = suffix.rfind('_');
        if (uscore != std::string::npos) {
            const std::string opname = suffix.substr(0, uscore);
            const std::string widthStr = suffix.substr(uscore + 1);
            int bw = 0;
            bool widthOk = !widthStr.empty();
            for (char c : widthStr) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    widthOk = false;
                    break;
                }
                bw = bw * 10 + (c - '0');
            }
            if (widthOk && bw >= 1 && bw <= 64) {
                llvm::Type* narrowTy = llvm::Type::getIntNTy(*context, static_cast<unsigned>(bw));
                // Helper: truncate argument to the narrow type.
                auto toNarrow = [&](llvm::Value* v) -> llvm::Value* {
                    v = toDefaultType(v);
                    return builder->CreateTrunc(v, narrowTy, "tw.narrow");
                };
                // Helper: zero-extend narrow result back to i64.
                // The result is always non-negative (narrow unsigned value).
                auto toWide = [&](llvm::Value* v) -> llvm::Value* {
                    auto* w = builder->CreateZExt(v, getDefaultType(), "tw.wide",
                                                  /*IsNonNeg=*/false);
                    nonNegValues_.insert(w);
                    return w;
                };

                if (opname == "popcount" && expr->arguments.size() == 1) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get()))
                        return llvm::ConstantInt::get(
                            getDefaultType(),
                            __builtin_popcountll(static_cast<uint64_t>(*cv) & ((bw < 64) ? (1ULL << bw) - 1 : ~0ULL)));
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctpop, {narrowTy});
                    llvm::Value* r =
                        builder->CreateCall(fn, {toNarrow(generateExpression(expr->arguments[0].get()))}, "tw.pop");
                    nonNegValues_.insert(r);
                    if (optimizationLevel >= OptimizationLevel::O1) {
                        // Range [0, bw+1) for tighter CVP.
                        auto* rangeMD = llvm::MDNode::get(
                            *context, {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, 0)),
                                       llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, bw + 1))});
                        llvm::cast<llvm::Instruction>(r)->setMetadata(llvm::LLVMContext::MD_range, rangeMD);
                    }
                    return toWide(r);
                }
                if (opname == "clz" && expr->arguments.size() == 1) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get())) {
                        uint64_t bits = static_cast<uint64_t>(*cv) & ((bw < 64) ? (1ULL << bw) - 1 : ~0ULL);
                        int64_t res = bits == 0 ? bw : static_cast<int64_t>(__builtin_clzll(bits) - (64 - bw));
                        return llvm::ConstantInt::get(getDefaultType(), res);
                    }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctlz, {narrowTy});
                    llvm::Value* r = builder->CreateCall(
                        fn, {toNarrow(generateExpression(expr->arguments[0].get())), builder->getFalse()}, "tw.clz");
                    nonNegValues_.insert(r);
                    if (optimizationLevel >= OptimizationLevel::O1) {
                        auto* rangeMD = llvm::MDNode::get(
                            *context, {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, 0)),
                                       llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, bw + 1))});
                        llvm::cast<llvm::Instruction>(r)->setMetadata(llvm::LLVMContext::MD_range, rangeMD);
                    }
                    return toWide(r);
                }
                if (opname == "ctz" && expr->arguments.size() == 1) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get())) {
                        uint64_t bits = static_cast<uint64_t>(*cv) & ((bw < 64) ? (1ULL << bw) - 1 : ~0ULL);
                        int64_t res = bits == 0 ? bw : static_cast<int64_t>(__builtin_ctzll(bits));
                        return llvm::ConstantInt::get(getDefaultType(), res);
                    }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::cttz, {narrowTy});
                    llvm::Value* r = builder->CreateCall(
                        fn, {toNarrow(generateExpression(expr->arguments[0].get())), builder->getFalse()}, "tw.ctz");
                    nonNegValues_.insert(r);
                    if (optimizationLevel >= OptimizationLevel::O1) {
                        auto* rangeMD = llvm::MDNode::get(
                            *context, {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, 0)),
                                       llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(narrowTy, bw + 1))});
                        llvm::cast<llvm::Instruction>(r)->setMetadata(llvm::LLVMContext::MD_range, rangeMD);
                    }
                    return toWide(r);
                }
                if (opname == "bitreverse" && expr->arguments.size() == 1) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get())) {
                        uint64_t bits = static_cast<uint64_t>(*cv) & ((bw < 64) ? (1ULL << bw) - 1 : ~0ULL);
                        uint64_t rev = 0;
                        for (int i = 0; i < bw; ++i)
                            rev |= ((bits >> i) & 1ULL) << (bw - 1 - i);
                        return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(rev));
                    }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::bitreverse, {narrowTy});
                    return toWide(
                        builder->CreateCall(fn, {toNarrow(generateExpression(expr->arguments[0].get()))}, "tw.brev"));
                }
                if (opname == "bswap" && expr->arguments.size() == 1 && bw >= 16 && (bw % 8) == 0) {
                    if (auto cv = tryFoldInt(expr->arguments[0].get())) {
                        uint64_t bits = static_cast<uint64_t>(*cv) & ((bw < 64) ? (1ULL << bw) - 1 : ~0ULL);
                        uint64_t swapped = 0;
                        int bytes = bw / 8;
                        for (int i = 0; i < bytes; ++i)
                            swapped |= ((bits >> (i * 8)) & 0xFF) << ((bytes - 1 - i) * 8);
                        return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(swapped));
                    }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::bswap, {narrowTy});
                    return toWide(
                        builder->CreateCall(fn, {toNarrow(generateExpression(expr->arguments[0].get()))}, "tw.bswap"));
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
                            int64_t lo = -(int64_t(1) << (bw - 1)), hi = (int64_t(1) << (bw - 1)) - 1;
                            if (res < lo)
                                res = lo;
                            if (res > hi)
                                res = hi;
                            return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(res));
                        }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sadd_sat, {narrowTy});
                    llvm::Value* a = toNarrow(generateExpression(expr->arguments[0].get()));
                    llvm::Value* b = toNarrow(generateExpression(expr->arguments[1].get()));
                    return builder->CreateSExt(builder->CreateCall(fn, {a, b}, "tw.sadd_sat"), getDefaultType(),
                                               "tw.sadd_sat.wide");
                }
                if (opname == "saturating_sub" && expr->arguments.size() == 2) {
                    if (auto a = tryFoldInt(expr->arguments[0].get()))
                        if (auto b = tryFoldInt(expr->arguments[1].get())) {
                            using I128 = __int128;
                            I128 res = static_cast<I128>(*a) - static_cast<I128>(*b);
                            int64_t lo = -(int64_t(1) << (bw - 1)), hi = (int64_t(1) << (bw - 1)) - 1;
                            if (res < lo)
                                res = lo;
                            if (res > hi)
                                res = hi;
                            return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(res));
                        }
                    llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ssub_sat, {narrowTy});
                    llvm::Value* a = toNarrow(generateExpression(expr->arguments[0].get()));
                    llvm::Value* b = toNarrow(generateExpression(expr->arguments[1].get()));
                    return builder->CreateSExt(builder->CreateCall(fn, {a, b}, "tw.ssub_sat"), getDefaultType(),
                                               "tw.ssub_sat.wide");
                }
            }
        }
    }

    // ── f32-typed float intrinsics (__tf_<op>) ────────────────────────────────
    if (expr->callee.size() > 5 && expr->callee.substr(0, 5) == "__tf_") {
        const std::string opname = expr->callee.substr(5);
        llvm::Type* f32Ty = llvm::Type::getFloatTy(*context);

        // Convert an i64 OmScript value to f32 (interpret as double then narrow).
        auto toF32 = [&](llvm::Value* v) -> llvm::Value* {
            if (v->getType()->isFloatTy())
                return v;
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
            if (expr->arguments.size() != 1)
                codegenError("expected 1 argument", expr);
            llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), iid, {f32Ty});
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.unary"));
        };
        // Binary f32 LLVM intrinsic.
        auto binaryF32 = [&](llvm::Intrinsic::ID iid) -> llvm::Value* {
            if (expr->arguments.size() != 2)
                codegenError("expected 2 arguments", expr);
            llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), iid, {f32Ty});
            llvm::Value* a = toF32(generateExpression(expr->arguments[0].get()));
            llvm::Value* b = toF32(generateExpression(expr->arguments[1].get()));
            return f32ToI64(builder->CreateCall(fn, {a, b}, "tf.binary"));
        };

        if (opname == "sqrt")
            return unaryF32(llvm::Intrinsic::sqrt);
        else if (opname == "sin")
            return unaryF32(llvm::Intrinsic::sin);
        else if (opname == "cos")
            return unaryF32(llvm::Intrinsic::cos);
        else if (opname == "tan") {
            // LLVM has no tan intrinsic; call tanf() from libm.
            if (expr->arguments.size() != 1)
                codegenError("expected 1 argument", expr);
            llvm::FunctionCallee tanf =
                module->getOrInsertFunction("tanf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(tanf, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.tan"));
        } else if (opname == "asin") {
            llvm::FunctionCallee fn =
                module->getOrInsertFunction("asinf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.asin"));
        } else if (opname == "acos") {
            llvm::FunctionCallee fn =
                module->getOrInsertFunction("acosf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.acos"));
        } else if (opname == "atan") {
            llvm::FunctionCallee fn =
                module->getOrInsertFunction("atanf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.atan"));
        } else if (opname == "atan2") {
            if (expr->arguments.size() != 2)
                codegenError("expected 2 arguments", expr);
            llvm::FunctionCallee fn =
                module->getOrInsertFunction("atan2f", llvm::FunctionType::get(f32Ty, {f32Ty, f32Ty}, false));
            llvm::Value* a = toF32(generateExpression(expr->arguments[0].get()));
            llvm::Value* b = toF32(generateExpression(expr->arguments[1].get()));
            return f32ToI64(builder->CreateCall(fn, {a, b}, "tf.atan2"));
        } else if (opname == "log")
            return unaryF32(llvm::Intrinsic::log);
        else if (opname == "log2")
            return unaryF32(llvm::Intrinsic::log2);
        else if (opname == "log10")
            return unaryF32(llvm::Intrinsic::log10);
        else if (opname == "exp")
            return unaryF32(llvm::Intrinsic::exp);
        else if (opname == "exp2")
            return unaryF32(llvm::Intrinsic::exp2);
        else if (opname == "cbrt") {
            llvm::FunctionCallee fn =
                module->getOrInsertFunction("cbrtf", llvm::FunctionType::get(f32Ty, {f32Ty}, false));
            return f32ToI64(builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.cbrt"));
        } else if (opname == "hypot") {
            if (expr->arguments.size() != 2)
                codegenError("expected 2 arguments", expr);
            llvm::FunctionCallee fn =
                module->getOrInsertFunction("hypotf", llvm::FunctionType::get(f32Ty, {f32Ty, f32Ty}, false));
            llvm::Value* a = toF32(generateExpression(expr->arguments[0].get()));
            llvm::Value* b = toF32(generateExpression(expr->arguments[1].get()));
            return f32ToI64(builder->CreateCall(fn, {a, b}, "tf.hypot"));
        } else if (opname == "fma") {
            if (expr->arguments.size() != 3)
                codegenError("expected 3 arguments", expr);
            llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::fma, {f32Ty});
            llvm::Value* a = toF32(generateExpression(expr->arguments[0].get()));
            llvm::Value* b = toF32(generateExpression(expr->arguments[1].get()));
            llvm::Value* c = toF32(generateExpression(expr->arguments[2].get()));
            return f32ToI64(builder->CreateCall(fn, {a, b, c}, "tf.fma"));
        } else if (opname == "copysign")
            return binaryF32(llvm::Intrinsic::copysign);
        else if (opname == "fast_sqrt") {
            if (expr->arguments.size() != 1)
                codegenError("expected 1 argument", expr);
            llvm::Function* fn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sqrt, {f32Ty});
            llvm::CallInst* call =
                builder->CreateCall(fn, {toF32(generateExpression(expr->arguments[0].get()))}, "tf.fsqrt");
            llvm::FastMathFlags fmf;
            fmf.setAllowReassoc(true);
            fmf.setNoNaNs(true);
            fmf.setNoInfs(true);
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
            // Extract char data from fat-pointer header and use puts().
            llvm::Value* strData = emitStringData(arg, "print.data");
            builder->CreateCall(getOrDeclarePuts(), {strData});
            return llvm::ConstantInt::get(getDefaultType(), 0);
        } else {
            // Print integer — printf %lld requires a 64-bit argument.
            // Widen narrow integers; truncate integers wider than i64 (e.g. i128).
            if (arg->getType()->isIntegerTy() && !arg->getType()->isIntegerTy(64)) {
                const unsigned bits = arg->getType()->getIntegerBitWidth();
                if (bits > 64) {
                    arg = builder->CreateTrunc(arg, getDefaultType(), "print.trunc");
                } else {
                    const bool isUnsigned = unsignedExprs_.count(arg) || isUnsignedValue(arg);
                    arg = isUnsigned ? builder->CreateZExt(arg, getDefaultType(), "print.zext",
                                                           /*IsNonNeg=*/false)
                                     : builder->CreateSExt(arg, getDefaultType(), "print.sext");
                    if (isUnsigned)
                        nonNegValues_.insert(arg);
                }
            }
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
        // Identity fold: abs(x) = x when x is known non-negative — the
        // intrinsic call is unnecessary and the value is already non-negative.
        if (nonNegValues_.count(arg))
            return arg;
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
        // Emit !range [0, INT64_MAX) so CVP/LVI/InstCombine can use this
        if (optimizationLevel >= OptimizationLevel::O1)
            result->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        return result;
    }

    if (bid == BuiltinId::LEN) {
        validateArgCount(expr, "len", 1);

        // Constant-fold len() on any expression that can be reduced to a
        {
            std::string folded;
            if (tryFoldStringConcat(expr->arguments[0].get(), folded)) {
                auto* result = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(folded.size()));
                nonNegValues_.insert(result);
                return result;
            }
            // tryFoldExprToConst handles multi-param calls (e.g. greet("world"))
            // that tryFoldStringConcat doesn't traverse.
            auto cv = tryFoldExprToConst(expr->arguments[0].get());
            if (cv && cv->kind == ConstValue::Kind::String) {
                auto* result = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(cv->strVal.size()));
                nonNegValues_.insert(result);
                return result;
            }
            // Fold len() on const arrays: `const arr = [1,2,3]; len(arr)` → 3.
            if (cv && cv->kind == ConstValue::Kind::Array) {
                auto* result = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(cv->arrVal.size()));
                nonNegValues_.insert(result);
                return result;
            }
        }

        // Constant-fold len(array_fill(N, val)): when N is a compile-time
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
            if (call->callee == "str_chars" && call->arguments.size() == 1) {
                // Generate the string length directly from fat-pointer header.
                llvm::Value* strArg = generateExpression(call->arguments[0].get());
                llvm::Value* strPtr =
                    strArg->getType()->isPointerTy()
                        ? strArg
                        : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "len.chars.ptr");
                llvm::Value* result = emitStringLen(strPtr, "len.chars.strlen");
                return result;
            }
        }
        // Also fold len(array_const_expr) via the unified evaluator.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                auto* result = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(cv->arrVal.size()));
                nonNegValues_.insert(result);
                optStats_.constFolded++;
                return result;
            }
        }

        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        Expression* argExpr = expr->arguments[0].get();

        // String detection uses two complementary checks:
        if (isStringExpr(argExpr)) {
            // Load length field from fat-pointer header (offset 0).
            llvm::Value* strPtr =
                arg->getType()->isPointerTy()
                    ? arg
                    : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "len.sptr");
            llvm::Value* result = emitStringLen(strPtr, "len.strlen");
            return result;
        }
        // Array is stored as an i64 holding a pointer to [length, elem0, elem1, ...]
        // Convert to integer first if needed (e.g. if stored in a float variable)
        arg = toDefaultType(arg);
        llvm::Value* arrPtr = arg->getType()->isPointerTy()
                                  ? arg
                                  : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "arrptr");
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
                if (result >= 0)
                    nonNegValues_.insert(c);
                return c;
            }
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        // Identity fold: min(x, x) = x — no intrinsic needed.
        if (a == b) {
            if (nonNegValues_.count(a))
                nonNegValues_.insert(a);
            return a;
        }
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
        if (nonNegValues_.count(a) && nonNegValues_.count(b)) {
            nonNegValues_.insert(result);
            // Emit !range metadata so LLVM passes see the non-negativity.
            if (optimizationLevel >= OptimizationLevel::O1)
                result->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        }
        return result;
    }

    if (bid == BuiltinId::MAX) {
        validateArgCount(expr, "max", 2);
        // Constant-fold max(a, b) when both are compile-time constants.
        if (auto ca = tryFoldInt(expr->arguments[0].get())) {
            if (auto cb = tryFoldInt(expr->arguments[1].get())) {
                const int64_t result = std::max(*ca, *cb);
                auto* c = llvm::ConstantInt::get(getDefaultType(), result);
                if (result >= 0)
                    nonNegValues_.insert(c);
                return c;
            }
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        // Identity fold: max(x, x) = x — no intrinsic needed.
        if (a == b) {
            if (nonNegValues_.count(a))
                nonNegValues_.insert(a);
            return a;
        }
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
        if (nonNegValues_.count(a) || nonNegValues_.count(b)) {
            nonNegValues_.insert(result);
            // Emit !range metadata so LLVM passes see the non-negativity.
            if (optimizationLevel >= OptimizationLevel::O1)
                result->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        }
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
        // Note: !range is not valid on select instructions in LLVM 18.
        // Range information is conveyed to the optimizer via nonNegValues_ tracking
        // and downstream llvm.assume calls if needed.
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
                    if (result >= 0)
                        nonNegValues_.insert(c);
                    return c;
                }
            }
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        llvm::Value* lo = generateExpression(expr->arguments[1].get());
        llvm::Value* hi = generateExpression(expr->arguments[2].get());
        // Identity fold: clamp(val, lo, lo) = lo — both bounds are identical.
        if (lo == hi) {
            if (nonNegValues_.count(lo))
                nonNegValues_.insert(lo);
            return lo;
        }
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
        // Constant-fold pow(base, exp) when both are compile-time constants.
        // When either argument is a float (not an exact integer), use double arithmetic.
        {
            auto cvB = tryFoldExprToConst(expr->arguments[0].get());
            auto cvE = tryFoldExprToConst(expr->arguments[1].get());
            bool bIsFloat = cvB && cvB->kind == ConstValue::Kind::Float;
            bool eIsFloat = cvE && cvE->kind == ConstValue::Kind::Float;
            if (cvB && cvE && (bIsFloat || eIsFloat)) {
                double fb = bIsFloat ? cvB->floatVal : static_cast<double>(cvB->intVal);
                double fe = eIsFloat ? cvE->floatVal : static_cast<double>(cvE->intVal);
                double result = std::pow(fb, fe);
                return llvm::ConstantFP::get(getFloatType(), result);
            }
        }
        // Integer constant-fold: both args must be exact integers.
        if (auto cb = tryFoldInt(expr->arguments[0].get())) {
            if (auto ce = tryFoldInt(expr->arguments[1].get())) {
                int64_t b = *cb, e = *ce;
                if (e >= 0) {
                    int64_t result = 1;
                    int64_t cur = b;
                    int64_t rem = e;
                    while (rem > 0) {
                        if (rem & 1)
                            result *= cur;
                        cur *= cur;
                        rem >>= 1;
                    }
                    auto* c = llvm::ConstantInt::get(getDefaultType(), result, true);
                    if (result >= 0)
                        nonNegValues_.insert(c);
                    return c;
                } else {
                    // Negative exponent in integer pow → 0 (matches runtime)
                    return llvm::ConstantInt::get(getDefaultType(), 0);
                }
            }
        }
        llvm::Value* base = generateExpression(expr->arguments[0].get());
        llvm::Value* exp = generateExpression(expr->arguments[1].get());

        // If either argument is a float, delegate to the llvm.pow.f64 intrinsic.
        // This handles pow(2.0, 0.5) = sqrt(2), pow(x, n) for fractional n, etc.
        if (base->getType()->isDoubleTy() || exp->getType()->isDoubleTy()) {
            if (!base->getType()->isDoubleTy())
                base = ensureFloat(base);
            if (!exp->getType()->isDoubleTy())
                exp = ensureFloat(exp);
            llvm::Function* powFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::pow, {getFloatType()});
            return builder->CreateCall(powFn, {base, exp}, "pow.fresult");
        }

        // Integer path: convert to i64 and use binary exponentiation (O(log n)).
        base = toDefaultType(base);
        exp = toDefaultType(exp);

        // Constant-exponent fast path: for small constant non-negative exponents
        if (auto* expCI = llvm::dyn_cast<llvm::ConstantInt>(exp)) {
            int64_t ev = expCI->getSExtValue();
            bool baseNonNeg = nonNegValues_.count(base) > 0;
            auto mkMulP = [&](llvm::Value* a, llvm::Value* b, const char* nm) -> llvm::Value* {
                return baseNonNeg ? builder->CreateNSWMul(a, b, nm) : builder->CreateMul(a, b, nm);
            };
            if (ev == 0) {
                auto* r = llvm::ConstantInt::get(getDefaultType(), 1);
                nonNegValues_.insert(r);
                return r;
            }
            if (ev == 1)
                return base;
            if (ev == 2) {
                auto* r = mkMulP(base, base, "powb.2");
                if (baseNonNeg)
                    nonNegValues_.insert(r);
                return r;
            }
            if (ev == 3) {
                auto* sq = mkMulP(base, base, "powb.3.sq");
                auto* r = mkMulP(sq, base, "powb.3");
                if (baseNonNeg)
                    nonNegValues_.insert(r);
                return r;
            }
            if (ev == 4) {
                auto* sq = mkMulP(base, base, "powb.4.sq");
                auto* r = mkMulP(sq, sq, "powb.4");
                nonNegValues_.insert(r); // even exponent → always non-neg
                return r;
            }
            if (ev == 5) {
                auto* sq = mkMulP(base, base, "powb.5.sq");
                auto* q4 = mkMulP(sq, sq, "powb.5.q4");
                auto* r = mkMulP(q4, base, "powb.5");
                if (baseNonNeg)
                    nonNegValues_.insert(r);
                return r;
            }
            if (ev == 6) {
                auto* sq = mkMulP(base, base, "powb.6.sq");
                auto* cb = mkMulP(sq, base, "powb.6.cb");
                auto* r = mkMulP(cb, cb, "powb.6");
                nonNegValues_.insert(r); // even exponent → always non-neg
                return r;
            }
            if (ev == 7) {
                auto* sq = mkMulP(base, base, "powb.7.sq");
                auto* cb = mkMulP(sq, base, "powb.7.cb");
                auto* p6 = mkMulP(cb, cb, "powb.7.p6");
                auto* r = mkMulP(p6, base, "powb.7");
                if (baseNonNeg)
                    nonNegValues_.insert(r);
                return r;
            }
            if (ev == 8) {
                auto* sq = mkMulP(base, base, "powb.8.sq");
                auto* q4 = mkMulP(sq, sq, "powb.8.q4");
                auto* r = mkMulP(q4, q4, "powb.8");
                nonNegValues_.insert(r); // even exponent → always non-neg
                return r;
            }
        }

        // Constant-base fast path: pow(2, n) → 1 << n for integer n.
        // This avoids the O(log n) binary-exponentiation loop entirely.
        // Negative exponents yield 0 (integer truncation); exponents ≥ 64 also
        // yield 0 (2^64 mod 2^64 = 0 in modular arithmetic).
        if (auto* baseCI = llvm::dyn_cast<llvm::ConstantInt>(base)) {
            if (baseCI->getSExtValue() == 2) {
                llvm::Value* zero64 = llvm::ConstantInt::get(getDefaultType(), 0, true);
                llvm::Value* one64 = llvm::ConstantInt::get(getDefaultType(), 1, true);
                llvm::Value* c63 = llvm::ConstantInt::get(getDefaultType(), 63, true);
                // Clamp exp into [0, 63] — LLVM UB if shift amount ≥ bitwidth.
                llvm::Function* smaxI = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smax, {getDefaultType()});
                llvm::Function* sminI = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smin, {getDefaultType()});
                llvm::Value* expPos = builder->CreateCall(smaxI, {exp, zero64}, "pow2.expnn");
                llvm::Value* expClamped = builder->CreateCall(sminI, {expPos, c63}, "pow2.expclamp");
                llvm::Value* shifted = builder->CreateShl(one64, expClamped, "pow2.shl");
                // Return 0 for negative exponents (integer pow(2,-1) = 0).
                llvm::Value* isNegExp = builder->CreateICmpSLT(exp, zero64, "pow2.isneg");
                auto* r = builder->CreateSelect(isNegExp, zero64, shifted, "pow2.result");
                nonNegValues_.insert(r);
                // Note: !range is not valid on select instructions in LLVM 18;
                // non-negativity is tracked via nonNegValues_.
                ++optStats_.constFolded;
                return r;
            }
        }

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Binary exponentiation (exponentiation by squaring): O(log n) in the exponent
        llvm::BasicBlock* negExpBB = llvm::BasicBlock::Create(*context, "pow.negexp", function);
        llvm::BasicBlock* zeroExpBB = llvm::BasicBlock::Create(*context, "pow.zeroexp", function);
        llvm::BasicBlock* posExpBB = llvm::BasicBlock::Create(*context, "pow.posexp", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "pow.loop", function);
        llvm::BasicBlock* oddBB = llvm::BasicBlock::Create(*context, "pow.odd", function);
        llvm::BasicBlock* squareBB = llvm::BasicBlock::Create(*context, "pow.square", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "pow.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1, true);

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
        llvm::Value* newResult = powBaseNonNeg ? builder->CreateNSWMul(result, curBase, "pow.mul")
                                               : builder->CreateMul(result, curBase, "pow.mul");
        llvm::Value* resultSel = builder->CreateSelect(isOdd, newResult, result, "pow.rsel");
        builder->CreateBr(squareBB);

        // Square the base and halve the exponent
        builder->SetInsertPoint(squareBB);
        llvm::Value* newBase = powBaseNonNeg ? builder->CreateNSWMul(curBase, curBase, "pow.sq")
                                             : builder->CreateMul(curBase, curBase, "pow.sq");
        llvm::Value* newCounter = builder->CreateAShr(counter, one, "pow.halve");
        result->addIncoming(resultSel, squareBB);
        curBase->addIncoming(newBase, squareBB);
        counter->addIncoming(newCounter, squareBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* finalResult = builder->CreatePHI(getDefaultType(), 3, "pow.final");
        finalResult->addIncoming(one, zeroExpBB); // pow(x, 0) == 1
        finalResult->addIncoming(zero, negExpBB); // pow(x, neg) == 0
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
            llvm::cast<llvm::LoadInst>(byte)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            charCode = builder->CreateZExt(byte, llvm::Type::getInt32Ty(*context), "charval",
                                           /*IsNonNeg=*/false);
            nonNegValues_.insert(charCode);
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
        // Allocate a 1024-byte temporary raw buffer for fgets
        llvm::Value* rawBufSize = llvm::ConstantInt::get(getDefaultType(), 1024);
        llvm::Value* rawBuf = builder->CreateCall(getOrDeclareMalloc(), {rawBufSize}, "inputln.rawbuf");
        llvm::cast<llvm::CallInst>(rawBuf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 1024));
        // Declare stdin as external global
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::GlobalVariable* stdinVar = module->getGlobalVariable("stdin");
        if (!stdinVar) {
            stdinVar =
                new llvm::GlobalVariable(*module, ptrTy, false, llvm::GlobalValue::ExternalLinkage, nullptr, "stdin");
        }
        llvm::Value* stdinVal = builder->CreateLoad(ptrTy, stdinVar, "inputln.stdin");
        llvm::cast<llvm::LoadInst>(stdinVal)->setMetadata(llvm::LLVMContext::MD_nonnull,
                                                          llvm::MDNode::get(*context, {}));
        // Call fgets(rawBuf, 1024, stdin)
        llvm::Value* intSize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1024);
        llvm::Value* fgetsRet = builder->CreateCall(getOrDeclareFgets(), {rawBuf, intSize, stdinVal}, "inputln.fgets");
        // If fgets returns NULL (EOF/error), set rawBuf[0] = '\0' so it looks like an empty string
        llvm::Value* fgetsNull = builder->CreateICmpEQ(fgetsRet, llvm::ConstantPointerNull::get(ptrTy), "inputln.eof");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* eofBB = llvm::BasicBlock::Create(*context, "inputln.eof", function);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "inputln.ok", function);
        llvm::BasicBlock* stripBB = llvm::BasicBlock::Create(*context, "inputln.strip", function);
        llvm::BasicBlock* noStripBB = llvm::BasicBlock::Create(*context, "inputln.nostrip", function);
        // EOF/error on fgets is rare — favour the success path.
        auto* fgetsW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(fgetsNull, eofBB, okBB, fgetsW);
        // EOF path: store '\0' at rawBuf[0], go to noStripBB
        builder->SetInsertPoint(eofBB);
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), rawBuf)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        builder->CreateBr(noStripBB);
        // OK path: strip trailing newline if present
        builder->SetInsertPoint(okBB);
        llvm::Value* nlChar = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10);
        llvm::Value* nlPtr = builder->CreateCall(getOrDeclareStrchr(), {rawBuf, nlChar}, "inputln.nl");
        llvm::Value* nlIsNull = builder->CreateICmpEQ(nlPtr, llvm::ConstantPointerNull::get(ptrTy), "inputln.nlnull");
        auto* nlW = llvm::MDBuilder(*context).createBranchWeights(1, 100);
        builder->CreateCondBr(nlIsNull, noStripBB, stripBB, nlW);
        builder->SetInsertPoint(stripBB);
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nlPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        builder->CreateBr(noStripBB);
        // noStripBB: rawBuf is now properly null-terminated, build fat-ptr
        builder->SetInsertPoint(noStripBB);
        // Use strlen to get actual length of the (possibly stripped) C string
        llvm::Value* rawLen = builder->CreateCall(getOrDeclareStrlen(), {rawBuf}, "inputln.rawlen");
        llvm::Value* hdr = emitAllocString(rawLen, rawLen, "inputln");
        // memcpy(charData, rawBuf, rawLen+1) — copies content including NUL
        llvm::Value* charData = emitStringData(hdr, "inputln.data");
        llvm::Value* rawLenP1 =
            builder->CreateAdd(rawLen, llvm::ConstantInt::get(getDefaultType(), 1), "inputln.rawlenp1", true, true);
        builder->CreateCall(getOrDeclareMemcpy(), {charData, rawBuf, rawLenP1});
        stringReturningFunctions_.insert("input_line");
        return hdr;
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

    if (bid == BuiltinId::FAST_ADD || bid == BuiltinId::FAST_SUB || bid == BuiltinId::FAST_MUL ||
        bid == BuiltinId::FAST_DIV) {
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

    if (bid == BuiltinId::PRECISE_ADD || bid == BuiltinId::PRECISE_SUB || bid == BuiltinId::PRECISE_MUL ||
        bid == BuiltinId::PRECISE_DIV) {
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
        return emitBoolZExt(isEven, "evenval");
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
                    if (elem.kind != ConstValue::Kind::Integer) {
                        allInt = false;
                        break;
                    }
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
        llvm::Value* arrPtr = arg->getType()->isPointerTy()
                                  ? arg
                                  : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "sum.arrptr");
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
        llvm::Value* done = builder->CreateICmpUGE(idx, length, "sum.done");
        auto* sumCondBr = builder->CreateCondBr(done, doneBB, bodyBB);
        if (optimizationLevel >= OptimizationLevel::O2) {
            sumCondBr->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(*context).createBranchWeights(1, 2000));
        }

        builder->SetInsertPoint(bodyBB);
        // Element is at offset (idx + 1) from array base
        llvm::Value* offset = builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 1), "sum.offset",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "sum.elemptr");
        llvm::Value* elem = emitLoadArrayElem(elemPtr, "sum.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                             llvm::MDNode::get(*context, {}));
        // nsw+nuw: OmScript guarantees array elements are initialized i64 values;
        llvm::Value* newAcc = builder->CreateAdd(acc, elem, "sum.newacc",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        // Eliminates a redundant add and provides SCEV with tight nsw+nuw flags.
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(offset, bodyBB);
        attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        return acc;
    }

    if (bid == BuiltinId::SWAP) {
        validateArgCount(expr, "swap", 3);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* i = generateExpression(expr->arguments[1].get());
        llvm::Value* j = generateExpression(expr->arguments[2].get());

        llvm::Value* arrPtr = arg->getType()->isPointerTy()
                                  ? arg
                                  : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "swap.arrptr");
        llvm::Value* swapLenLoad = emitLoadArrayLen(arrPtr, "swap.len");
        llvm::Value* length = swapLenLoad;

        // Bounds check both indices: 0 <= i < length and 0 <= j < length.
        // ULT(i, length) is equivalent to (SLT(i, length) && SGE(i, 0)) when
        // length >= 0 (guaranteed: emitLoadArrayLen tracks into nonNegValues_).
        // This eliminates one icmp + one and per index check.
        llvm::Value* iValid = builder->CreateICmpULT(i, length, "swap.i.valid");
        llvm::Value* jValid = builder->CreateICmpULT(j, length, "swap.j.valid");
        llvm::Value* bothValid = builder->CreateAnd(iValid, jValid, "swap.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "swap.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "swap.fail", function);
        // Swap OOB is extremely unlikely.
        llvm::MDNode* swapW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(bothValid, okBB, failBB, swapW);

        builder->SetInsertPoint(failBB);
        {
            std::string msg = expr->line > 0 ? std::string("Runtime error: swap index out of bounds at line ") +
                                                   std::to_string(expr->line) + "\n"
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
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(valI)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                             llvm::MDNode::get(*context, {}));
        llvm::Value* valJ = emitLoadArrayElem(ptrJ, "swap.valj");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(valJ)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                             llvm::MDNode::get(*context, {}));
        emitStoreArrayElem(valJ, ptrI);
        emitStoreArrayElem(valI, ptrJ);
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (bid == BuiltinId::REVERSE) {
        validateArgCount(expr, "reverse", 1);
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = arg->getType()->isPointerTy()
                                  ? arg
                                  : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "rev.arrptr");
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
        attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        return arg;
    }

    if (bid == BuiltinId::TO_CHAR) {
        validateArgCount(expr, "to_char", 1);
        // Allocate a 2-byte buffer [char, '\0'] and return a pointer to it,
        // so the result behaves like a one-character string.
        llvm::Value* code = generateExpression(expr->arguments[0].get());
        code = toDefaultType(code); // ensure i64
        llvm::Value* one64 = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* hdr = emitAllocString(one64, one64, "tochar");
        llvm::Value* byteVal = builder->CreateTrunc(code, llvm::Type::getInt8Ty(*context), "tochar.byte");
        llvm::Value* dataPtr = emitStringData(hdr, "tochar.data");
        builder->CreateStore(byteVal, dataPtr);
        llvm::Value* nulPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), dataPtr, one64, "tochar.nul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nulPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        stringReturningFunctions_.insert("to_char");
        return hdr;
    }

    if (bid == BuiltinId::IS_ALPHA) {
        validateArgCount(expr, "is_alpha", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_alpha() is an integer operation
        x = toDefaultType(x);
        // Use the unsigned-subtract range-check trick:
        auto* cA = llvm::ConstantInt::get(getDefaultType(), 65);  // 'A'
        auto* ca = llvm::ConstantInt::get(getDefaultType(), 97);  // 'a'
        auto* c25 = llvm::ConstantInt::get(getDefaultType(), 25); // 'Z'-'A'
        llvm::Value* upper = builder->CreateICmpULE(builder->CreateSub(x, cA, "sub.A"), c25, "isupper");
        llvm::Value* lower = builder->CreateICmpULE(builder->CreateSub(x, ca, "sub.a"), c25, "islower");
        llvm::Value* isAlpha = builder->CreateOr(upper, lower, "isalpha");
        return emitBoolZExt(isAlpha, "alphaval");
    }

    if (bid == BuiltinId::IS_DIGIT) {
        validateArgCount(expr, "is_digit", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_digit() is an integer operation
        x = toDefaultType(x);
        // Use the unsigned-subtract range-check trick:
        auto* c0 = llvm::ConstantInt::get(getDefaultType(), 48); // '0'
        auto* c9 = llvm::ConstantInt::get(getDefaultType(), 9);  // '9'-'0'
        llvm::Value* isDigit = builder->CreateICmpULE(builder->CreateSub(x, c0, "sub.0"), c9, "isdigit");
        return emitBoolZExt(isDigit, "digitval");
    }

    if (bid == BuiltinId::IS_UPPER) {
        validateArgCount(expr, "is_upper", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        x = toDefaultType(x);
        // (x - 'A') <=u 25  covers exactly 'A'..'Z'
        auto* cA = llvm::ConstantInt::get(getDefaultType(), 65);  // 'A'
        auto* c25 = llvm::ConstantInt::get(getDefaultType(), 25); // 'Z'-'A'
        llvm::Value* isUpper = builder->CreateICmpULE(builder->CreateSub(x, cA, "sub.A"), c25, "isupper");
        return emitBoolZExt(isUpper, "upperval");
    }

    if (bid == BuiltinId::IS_LOWER) {
        validateArgCount(expr, "is_lower", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        x = toDefaultType(x);
        // (x - 'a') <=u 25  covers exactly 'a'..'z'
        auto* ca = llvm::ConstantInt::get(getDefaultType(), 97);  // 'a'
        auto* c25 = llvm::ConstantInt::get(getDefaultType(), 25); // 'z'-'a'
        llvm::Value* isLower = builder->CreateICmpULE(builder->CreateSub(x, ca, "sub.a"), c25, "islower");
        return emitBoolZExt(isLower, "lowerval");
    }

    if (bid == BuiltinId::IS_SPACE) {
        validateArgCount(expr, "is_space", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        x = toDefaultType(x);
        // C whitespace: ' '(32), '\t'(9), '\n'(10), '\v'(11), '\f'(12), '\r'(13)
        auto* c9 = llvm::ConstantInt::get(getDefaultType(), 9);   // '\t'
        auto* c4 = llvm::ConstantInt::get(getDefaultType(), 4);   // '\r'-'\t'
        auto* c32 = llvm::ConstantInt::get(getDefaultType(), 32); // ' '
        llvm::Value* isCtrl = builder->CreateICmpULE(builder->CreateSub(x, c9, "sub.9"), c4, "isctrl");
        llvm::Value* isSpc = builder->CreateICmpEQ(x, c32, "isspc");
        llvm::Value* isSpace = builder->CreateOr(isCtrl, isSpc, "isspace");
        return emitBoolZExt(isSpace, "spaceval");
    }

    if (bid == BuiltinId::IS_ALNUM) {
        validateArgCount(expr, "is_alnum", 1);
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        x = toDefaultType(x);
        // Alphanumeric: letter (A-Z or a-z) or digit (0-9).
        auto* cA = llvm::ConstantInt::get(getDefaultType(), 65);  // 'A'
        auto* ca = llvm::ConstantInt::get(getDefaultType(), 97);  // 'a'
        auto* c0 = llvm::ConstantInt::get(getDefaultType(), 48);  // '0'
        auto* c25 = llvm::ConstantInt::get(getDefaultType(), 25); // 'Z'-'A'
        auto* c9 = llvm::ConstantInt::get(getDefaultType(), 9);   // '9'-'0'
        llvm::Value* upper = builder->CreateICmpULE(builder->CreateSub(x, cA, "sub.A"), c25, "isupper2");
        llvm::Value* lower = builder->CreateICmpULE(builder->CreateSub(x, ca, "sub.a"), c25, "islower2");
        llvm::Value* digit = builder->CreateICmpULE(builder->CreateSub(x, c0, "sub.0"), c9, "isdigit2");
        llvm::Value* isAlnum = builder->CreateOr(builder->CreateOr(upper, lower, "isalpha2"), digit, "isalnum");
        return emitBoolZExt(isAlnum, "alnumval");
    }

    // typeof(x) returns a compile-time type tag: 1=integer, 2=float, 3=string.
    // DEPRECATED: typeof() resolves at compile time from static type information,
    // not at runtime. Use explicit type annotations instead.
    if (bid == BuiltinId::TYPEOF) {
        validateArgCount(expr, "typeof", 1);
        std::cerr << "[warning] 'typeof' is deprecated and will be removed in a future version."
                     " It resolves statically at compile time; use explicit type annotations instead.\n";
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

    // type_name(x) — returns a human-readable compile-time string describing the
    // LLVM type of the argument expression.  Unlike the deprecated typeof() which
    // returns an opaque integer tag, type_name() returns an OmScript string constant:
    //   "int"     — any integer type (i64, i32, i8, …, bool is "bool")
    //   "float"   — f64 (double)
    //   "f32"     — f32 (single-precision float)
    //   "bool"    — i1 boolean
    //   "string"  — string fat-pointer
    //   "array"   — array / slice pointer
    //   "dict"    — hash-map pointer
    //   "ptr"     — raw / struct pointer
    //   "simd"    — LLVM fixed vector type
    //   "void"    — no-value (unreachable expression)
    //   "unknown" — anything else
    // The result is a compile-time string constant; it resolves purely from static
    // LLVM IR type information and incurs no runtime overhead.
    if (bid == BuiltinId::TYPE_NAME) {
        validateArgCount(expr, "type_name", 1);
        // Evaluate the argument purely for side-effects and type information;
        // the returned LLVM Value* is not used at runtime.
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Type* t = arg->getType();
        const char* name;
        if (t->isIntegerTy(1)) {
            // Check whether the variable carries a bool annotation.
            // i1 loaded from a bool-annotated alloca → "bool".
            name = "bool";
        } else if (t->isIntegerTy(32)) {
            // i32 could be a char (u32/char annotation) or a plain i32.
            // Check var annotation; if it's "char" or "c_int"/"c_uint"/"u32"/"i32"
            // we still call it "int" unless it's specifically annotated as char.
            if (expr->arguments[0]->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto* id = static_cast<const IdentifierExpr*>(expr->arguments[0].get());
                auto it = varTypeAnnotations_.find(id->name);
                if (it != varTypeAnnotations_.end() && it->second == "char")
                    name = "char";
                else
                    name = "int";
            } else {
                name = "int";
            }
        } else if (t->isIntegerTy()) {
            name = "int";
        } else if (t->isDoubleTy()) {
            name = "float";
        } else if (t->isFloatTy()) {
            name = "f32";
        } else if (t->isStructTy()) {
            name = "tuple";
        } else if (t->isPointerTy()) {
            // Distinguish string, array, dict, tuple, and plain pointers using the
            // higher-level expression classifiers the codegen already has.
            if (isStringExpr(expr->arguments[0].get())) {
                name = "string";
            } else if (isDictExpr(expr->arguments[0].get())) {
                name = "dict";
            } else if (expr->arguments[0]->type == ASTNodeType::ARRAY_EXPR) {
                name = "array";
            } else if (expr->arguments[0]->type == ASTNodeType::TUPLE_EXPR) {
                name = "tuple";
            } else if (expr->arguments[0]->type == ASTNodeType::IDENTIFIER_EXPR) {
                // Check the arrayVars_ / tupleVarTypes_ / stringVars_ tracking sets.
                const auto* id = static_cast<const IdentifierExpr*>(expr->arguments[0].get());
                if (tupleVarTypes_.count(id->name)) {
                    name = "tuple";
                } else if (arrayVars_.count(id->name)) {
                    name = "array";
                } else {
                    name = "ptr";
                }
            } else {
                name = "ptr";
            }
        } else if (t->isVectorTy()) {
            name = "simd";
        } else if (t->isVoidTy()) {
            name = "void";
        } else {
            name = "unknown";
        }
        (void)arg;
        // Return a proper OmScript fat-pointer string (same format as string literals:
        // { len:i64, cap:i64, [N+1 x i8] } global with a GEP-0 pointer to offset 0).
        // This makes the result compatible with str_eq(), println(), etc.
        llvm::GlobalVariable* gv = internString(name);
        return llvm::ConstantExpr::getInBoundsGetElementPtr(
            gv->getValueType(), gv,
            llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
    }

    // ── Round-73 builtins ────────────────────────────────────────────────────

    // str_is_empty(s) → 1 if len(s)==0, else 0
    if (bid == BuiltinId::STR_IS_EMPTY) {
        validateArgCount(expr, "str_is_empty", 1);
        // Compile-time fold: str_is_empty("literal")
        if (auto s = tryFoldStr(expr->arguments[0].get()))
            return llvm::ConstantInt::get(getDefaultType(), s->empty() ? 1 : 0);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context),
                                                            "isempty.ptr");
        llvm::Value* len  = emitStringLen(strPtr, "isempty.len");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* cmp  = builder->CreateICmpEQ(len, zero, "isempty.cmp");
        return builder->CreateZExt(cmp, getDefaultType(), "isempty.result");
    }

    // str_capitalize(s) → first char toupper, rest tolower
    if (bid == BuiltinId::STR_CAPITALIZE) {
        validateArgCount(expr, "str_capitalize", 1);
        // Compile-time fold
        if (auto s = tryFoldStr(expr->arguments[0].get())) {
            std::string result = *s;
            if (!result.empty()) {
                result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
                for (size_t i = 1; i < result.size(); ++i)
                    result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
            }
            llvm::GlobalVariable* gv = internString(result.c_str());
            stringReturningFunctions_.insert("str_capitalize");
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                gv->getValueType(), gv,
                llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context),
                                                            "cap.ptr");
        llvm::Value* strLen  = emitStringLen(strPtr, "cap.len");
        llvm::Value* hdr     = emitAllocString(strLen, strLen, "cap");
        llvm::Value* dstData = emitStringData(hdr, "cap.data");
        builder->CreateCall(getOrDeclareStrcpy(), {dstData, emitStringData(strPtr, "cap.srcdata")});

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // If string is non-empty, toupper the first byte
        llvm::BasicBlock* nonEmptyBB = llvm::BasicBlock::Create(*context, "cap.nonempty", function);
        llvm::BasicBlock* restBB     = llvm::BasicBlock::Create(*context, "cap.rest",     function);
        llvm::Value* isEmpty = builder->CreateICmpEQ(strLen, zero, "cap.isempty");
        builder->CreateCondBr(isEmpty, restBB, nonEmptyBB);

        builder->SetInsertPoint(nonEmptyBB);
        {
            llvm::Value* firstPtr = dstData; // offset 0
            auto* firstLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), firstPtr, "cap.first");
            firstLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* first32 = builder->CreateZExt(firstLoad, llvm::Type::getInt32Ty(*context), "cap.first32");
            llvm::Value* upper   = builder->CreateCall(getOrDeclareToupper(), {first32}, "cap.upper");
            llvm::Value* upper8  = builder->CreateTrunc(upper, llvm::Type::getInt8Ty(*context), "cap.upper8");
            auto* st = builder->CreateStore(upper8, firstPtr);
            st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        }
        builder->CreateBr(restBB);

        // Loop over indices [1..strLen): tolower each byte
        builder->SetInsertPoint(restBB);
        emitCountingLoop("cap.lower", strLen, one, 4, [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
            llvm::Value* charPtr =
                builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), dstData, idx, "cap.charptr");
            auto* chLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "cap.ch");
            chLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* ch32  = builder->CreateZExt(chLoad, llvm::Type::getInt32Ty(*context), "cap.ch32");
            llvm::Value* lower = builder->CreateCall(getOrDeclareTolower(), {ch32}, "cap.tolower");
            llvm::Value* low8  = builder->CreateTrunc(lower, llvm::Type::getInt8Ty(*context), "cap.low8");
            auto* st = builder->CreateStore(low8, charPtr);
            st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* nextIdx = builder->CreateAdd(idx, one, "cap.next", /*NUW=*/true, /*NSW=*/true);
            idx->addIncoming(nextIdx, builder->GetInsertBlock());
            attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        });
        stringReturningFunctions_.insert("str_capitalize");
        return hdr;
    }

    // array_first(arr) → first element; aborts with a runtime error on empty array
    if (bid == BuiltinId::ARRAY_FIRST) {
        validateArgCount(expr, "array_first", 1);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* arrPtr = arrArg->getType()->isPointerTy()
                                  ? arrArg
                                  : builder->CreateIntToPtr(arrArg, ptrTy, "afirst.ptr");
        llvm::Value* arrLen = emitLoadArrayLen(arrPtr, "afirst.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB   = llvm::BasicBlock::Create(*context, "afirst.ok",   function);
        llvm::BasicBlock* oobBB  = llvm::BasicBlock::Create(*context, "afirst.oob",  function);

        llvm::Value* zero    = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isEmpty = builder->CreateICmpEQ(arrLen, zero, "afirst.isempty");
        llvm::MDNode* oobW   = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(isEmpty, oobBB, okBB, oobW);

        builder->SetInsertPoint(oobBB);
        {
            const char* msg = "Runtime error: array_first called on empty array\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "afirst.errmsg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        llvm::Value* one      = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* elemPtr  = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, one, "afirst.elemptr");
        return builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "afirst.val");
    }

    // map_copy(d) → shallow copy of the hashmap by iterating live buckets
    if (bid == BuiltinId::MAP_COPY) {
        validateArgCount(expr, "map_copy", 1);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        mapArg = toDefaultType(mapArg);
        auto* mcPtrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mcSrcPtr = builder->CreateIntToPtr(mapArg, mcPtrTy, "mcopy.srcptr");

        // Allocate fresh result map
        llvm::Value* mcRes = builder->CreateCall(getOrEmitHashMapNew(), {}, "mcopy.res");
        llvm::AllocaInst* mcResA =
            createEntryBlockAlloca(builder->GetInsertBlock()->getParent(), "mcopy.resmap", mcPtrTy);
        builder->CreateStore(mcRes, mcResA);

        // Read source capacity
        auto* mcCapLoad = builder->CreateAlignedLoad(getDefaultType(), mcSrcPtr, llvm::MaybeAlign(8), "mcopy.cap");
        mcCapLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
        llvm::Value* mcCap = mcCapLoad;
        llvm::Value* mcZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* mcOne  = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* mcTwo  = llvm::ConstantInt::get(getDefaultType(), 2);

        llvm::Function* mcFn  = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* mcPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* mcLoopBB = llvm::BasicBlock::Create(*context, "mcopy.loop", mcFn);
        llvm::BasicBlock* mcTestBB = llvm::BasicBlock::Create(*context, "mcopy.test", mcFn);
        llvm::BasicBlock* mcInsBB  = llvm::BasicBlock::Create(*context, "mcopy.ins",  mcFn);
        llvm::BasicBlock* mcIncBB  = llvm::BasicBlock::Create(*context, "mcopy.inc",  mcFn);
        llvm::BasicBlock* mcDoneBB = llvm::BasicBlock::Create(*context, "mcopy.done", mcFn);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(mcLoopBB)));

        builder->SetInsertPoint(mcLoopBB);
        llvm::PHINode* mcBi = builder->CreatePHI(getDefaultType(), 2, "mcopy.bi");
        mcBi->addIncoming(mcZero, mcPreBB);
        builder->CreateCondBr(builder->CreateICmpULT(mcBi, mcCap), mcTestBB, mcDoneBB);

        builder->SetInsertPoint(mcTestBB);
        // bucket offset = 2 + bi*3
        llvm::Value* mcBoff =
            builder->CreateAdd(builder->CreateMul(mcBi, llvm::ConstantInt::get(getDefaultType(), 3), "",
                                                  /*HasNUW=*/true, /*HasNSW=*/true),
                               mcTwo, "", /*HasNUW=*/true, /*HasNSW=*/true);
        auto* mcHashV = builder->CreateAlignedLoad(
            getDefaultType(), builder->CreateInBoundsGEP(getDefaultType(), mcSrcPtr, mcBoff), llvm::MaybeAlign(8),
            "mcopy.h");
        mcHashV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
        // Live entries have hash >= 2 (0=empty, 1=tombstone)
        llvm::Value* mcLive = builder->CreateICmpUGE(mcHashV, mcTwo, "mcopy.live");
        builder->CreateCondBr(mcLive, mcInsBB, mcIncBB);

        builder->SetInsertPoint(mcInsBB);
        auto* mcKeyV = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mcSrcPtr,
                                       builder->CreateAdd(mcBoff, mcOne, "", /*HasNUW=*/true, /*HasNSW=*/true)),
            llvm::MaybeAlign(8), "mcopy.k");
        mcKeyV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
        auto* mcValV = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mcSrcPtr,
                                       builder->CreateAdd(mcBoff, mcTwo, "", /*HasNUW=*/true, /*HasNSW=*/true)),
            llvm::MaybeAlign(8), "mcopy.v");
        mcValV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
        llvm::Value* mcCurRes = builder->CreateAlignedLoad(mcPtrTy, mcResA, llvm::MaybeAlign(8), "mcopy.cur");
        llvm::Value* mcNewRes = builder->CreateCall(getOrEmitHashMapSet(), {mcCurRes, mcKeyV, mcValV}, "mcopy.new");
        builder->CreateStore(mcNewRes, mcResA);
        builder->CreateBr(mcIncBB);

        builder->SetInsertPoint(mcIncBB);
        llvm::Value* mcBi1 = builder->CreateAdd(mcBi, mcOne, "", true, true);
        mcBi->addIncoming(mcBi1, mcIncBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(mcLoopBB)));

        builder->SetInsertPoint(mcDoneBB);
        llvm::Value* mcFinal = builder->CreateAlignedLoad(mcPtrTy, mcResA, llvm::MaybeAlign(8), "mcopy.final");
        return mcFinal;
    }

    // map_clear(d) → return a fresh empty map (the old map is not freed; caller
    //               should let it go out of scope)
    if (bid == BuiltinId::MAP_CLEAR) {
        validateArgCount(expr, "map_clear", 1);
        // Evaluate the argument for side effects, then discard it
        generateExpression(expr->arguments[0].get());
        return builder->CreateCall(getOrEmitHashMapNew(), {}, "mclear.new");
    }

    // ── End Round-73 builtins ────────────────────────────────────────────────

    // ── Round-74 builtins ────────────────────────────────────────────────────

    // array_sum(arr) → sum of all integer elements; 0 for empty array
    if (bid == BuiltinId::ARRAY_SUM) {
        validateArgCount(expr, "array_sum", 1);
        // Compile-time fold
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                int64_t total = 0;
                bool allInt = true;
                for (const auto& elem : cv->arrVal) {
                    if (elem.kind != ConstValue::Kind::Integer) { allInt = false; break; }
                    total += elem.intVal;
                }
                if (allInt) { optStats_.constFolded++; return llvm::ConstantInt::get(getDefaultType(), total); }
            }
        }
        llvm::Value* sumArg = generateExpression(expr->arguments[0].get());
        sumArg = toDefaultType(sumArg);
        llvm::Value* arrPtr = getArrayPtr(sumArg);
        llvm::Value* asuLenLoad = emitLoadArrayLen(arrPtr, "asum.len");
        llvm::Value* length = asuLenLoad;

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "asum.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "asum.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "asum.done", function);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::PHINode* acc = builder->CreatePHI(getDefaultType(), 2, "asum.acc");
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "asum.idx");
        acc->addIncoming(zero, entryBB);
        idx->addIncoming(zero, entryBB);

        llvm::Value* done = builder->CreateICmpUGE(idx, length, "asum.done");
        auto* asuCondBr = builder->CreateCondBr(done, doneBB, bodyBB);
        if (optimizationLevel >= OptimizationLevel::O2)
            asuCondBr->setMetadata(llvm::LLVMContext::MD_prof,
                                   llvm::MDBuilder(*context).createBranchWeights(1, 2000));

        builder->SetInsertPoint(bodyBB);
        llvm::Value* offset  = builder->CreateAdd(idx, one, "asum.offset", true, true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, offset, "asum.elemptr");
        llvm::Value* elem    = emitLoadArrayElem(elemPtr, "asum.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                             llvm::MDNode::get(*context, {}));
        llvm::Value* newAcc = (inOptMaxFunction || optimizationLevel >= OptimizationLevel::O2)
                                  ? builder->CreateNSWAdd(acc, elem, "asum.newacc")
                                  : builder->CreateAdd(acc, elem, "asum.newacc");
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(offset, bodyBB);
        { attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB))); }

        builder->SetInsertPoint(doneBB);
        return acc;
    }

    // array_sorted(arr) → return a sorted copy (non-mutating, ascending)
    if (bid == BuiltinId::ARRAY_SORTED) {
        validateArgCount(expr, "array_sorted", 1);
        const bool sortStrings = isStringArrayExpr(expr->arguments[0].get());
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* arrLen = emitLoadArrayLen(arrPtr, "asorted.len");
        // Total bytes: (arrLen + 1) * 8
        llvm::Value* one   = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "asorted.slots", true, true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "asorted.bytes", true, true);
        // Allocate copy buffer
        llvm::Value* copyBuf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "asorted.buf");
        llvm::cast<llvm::CallInst>(copyBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        // Copy entire array (header + elements)
        builder->CreateCall(getOrDeclareMemcpy(), {copyBuf, arrPtr, bytes});
        // Sort the copy (skip if <= 1 element)
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* sortBB = llvm::BasicBlock::Create(*context, "asorted.sort", function);
        llvm::BasicBlock* skipBB = llvm::BasicBlock::Create(*context, "asorted.skip", function);
        llvm::Value* needsSort = builder->CreateICmpUGT(arrLen, one, "asorted.needed");
        builder->CreateCondBr(needsSort, sortBB, skipBB);

        builder->SetInsertPoint(sortBB);
        // Reuse the same comparator logic as the `sort` builtin
        auto* localPtrTy = llvm::PointerType::getUnqual(*context);
        auto getOrEmitIntCmpFn = [&]() -> llvm::Function* {
            const char* name = "__omsc_cmp_i64_asc";
            if (auto* fn = module->getFunction(name)) return fn;
            auto* cmpTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {localPtrTy, localPtrTy}, false);
            auto* fn = llvm::Function::Create(cmpTy, llvm::Function::InternalLinkage, name, module.get());
            fn->addFnAttr(llvm::Attribute::NoUnwind); fn->addFnAttr(llvm::Attribute::WillReturn);
            fn->addFnAttr(llvm::Attribute::NoFree);   fn->addFnAttr(llvm::Attribute::NoSync);
            fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
                *context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
            auto savedIP = builder->saveIP();
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            builder->SetInsertPoint(entry);
            auto* a = builder->CreateAlignedLoad(getDefaultType(), fn->getArg(0), llvm::MaybeAlign(8), "a");
            auto* b = builder->CreateAlignedLoad(getDefaultType(), fn->getArg(1), llvm::MaybeAlign(8), "b");
            llvm::cast<llvm::Instruction>(a)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            llvm::cast<llvm::Instruction>(b)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            auto* gt = builder->CreateZExt(builder->CreateICmpSGT(a, b), llvm::Type::getInt32Ty(*context), "gt.zext", false);
            auto* lt = builder->CreateZExt(builder->CreateICmpSLT(a, b), llvm::Type::getInt32Ty(*context), "lt.zext", false);
            builder->CreateRet(builder->CreateSub(gt, lt, "cmp"));
            builder->restoreIP(savedIP);
            return fn;
        };
        auto getOrEmitStrCmpFn = [&]() -> llvm::Function* {
            const char* name = "__omsc_cmp_str_asc";
            if (auto* fn = module->getFunction(name)) return fn;
            auto* cmpTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {localPtrTy, localPtrTy}, false);
            auto* fn = llvm::Function::Create(cmpTy, llvm::Function::InternalLinkage, name, module.get());
            fn->addFnAttr(llvm::Attribute::NoUnwind); fn->addFnAttr(llvm::Attribute::WillReturn);
            fn->addFnAttr(llvm::Attribute::NoFree);   fn->addFnAttr(llvm::Attribute::NoSync);
            auto savedIP = builder->saveIP();
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            builder->SetInsertPoint(entry);
            auto* off16 = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 16);
            auto* aI64  = builder->CreateAlignedLoad(getDefaultType(), fn->getArg(0), llvm::MaybeAlign(8), "a.i64");
            auto* bI64  = builder->CreateAlignedLoad(getDefaultType(), fn->getArg(1), llvm::MaybeAlign(8), "b.i64");
            llvm::cast<llvm::Instruction>(aI64)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            llvm::cast<llvm::Instruction>(bI64)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            auto* aFat = builder->CreateIntToPtr(aI64, localPtrTy, "a.fat");
            auto* bFat = builder->CreateIntToPtr(bI64, localPtrTy, "b.fat");
            auto* aData = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), aFat, off16, "a.data");
            auto* bData = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), bFat, off16, "b.data");
            builder->CreateRet(builder->CreateCall(getOrDeclareStrcmp(), {aData, bData}, "cmp"));
            builder->restoreIP(savedIP);
            return fn;
        };
        llvm::Function* cmpFn = sortStrings ? getOrEmitStrCmpFn() : getOrEmitIntCmpFn();
        llvm::Value* dataPtr  = builder->CreateInBoundsGEP(getDefaultType(), copyBuf, one, "asorted.data");
        builder->CreateCall(getOrDeclareQsort(), {dataPtr, arrLen,
            llvm::ConstantInt::get(getDefaultType(), 8), cmpFn});
        builder->CreateBr(skipBB);
        builder->SetInsertPoint(skipBB);
        return copyBuf;
    }

    // array_reverse(arr) → return a reversed copy of the array
    if (bid == BuiltinId::ARRAY_REVERSE) {
        validateArgCount(expr, "array_reverse", 1);
        llvm::Value* arrArg2 = generateExpression(expr->arguments[0].get());
        arrArg2 = toDefaultType(arrArg2);
        llvm::Value* arrPtr = getArrayPtr(arrArg2);
        llvm::Value* arrLen = emitLoadArrayLen(arrPtr, "arev.len");
        llvm::Value* one   = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "arev.slots", true, true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "arev.bytes", true, true);
        llvm::Value* dstBuf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "arev.buf");
        llvm::cast<llvm::CallInst>(dstBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        // Store length header
        builder->CreateStore(arrLen, dstBuf);
        // Loop: dst[i] = src[len-1-i]  (i in [0, len))
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preBB  = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "arev.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "arev.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "arev.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "arev.idx");
        idx->addIncoming(zero, preBB);
        llvm::Value* done = builder->CreateICmpUGE(idx, arrLen, "arev.done");
        auto* arevBr = builder->CreateCondBr(done, doneBB, bodyBB);
        if (optimizationLevel >= OptimizationLevel::O2)
            arevBr->setMetadata(llvm::LLVMContext::MD_prof,
                                llvm::MDBuilder(*context).createBranchWeights(1, 2000));
        builder->SetInsertPoint(bodyBB);
        // src element index = arrLen - 1 - idx  (offset into data = arrLen - idx, accounting for header)
        llvm::Value* srcOff = builder->CreateSub(arrLen, idx, "arev.srcoff", false, true);
        llvm::Value* srcPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, srcOff, "arev.srcptr");
        llvm::Value* elem   = emitLoadArrayElem(srcPtr, "arev.elem");
        // dst slot offset = idx + 1 (skip header)
        llvm::Value* dstOff = builder->CreateAdd(idx, one, "arev.dstoff", true, true);
        llvm::Value* dstPtr = builder->CreateInBoundsGEP(getDefaultType(), dstBuf, dstOff, "arev.dstptr");
        builder->CreateAlignedStore(elem, dstPtr, llvm::MaybeAlign(8))
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "arev.next", true, true);
        idx->addIncoming(nextIdx, bodyBB);
        { attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB))); }
        builder->SetInsertPoint(doneBB);
        return dstBuf;
    }

    // str_words(s) → split s on whitespace (isspace), skipping empty tokens → string[]
    if (bid == BuiltinId::STR_WORDS) {
        validateArgCount(expr, "str_words", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, ptrTy, "sw.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "sw.len");
        llvm::Value* zero   = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one    = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight  = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* srcData = emitStringData(strPtr, "sw.srcdata");

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // ── Pass 1: count words ──────────────────────────────────────────────
        llvm::BasicBlock* cPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* cLoopBB = llvm::BasicBlock::Create(*context, "sw.cnt.loop", function);
        llvm::BasicBlock* cBodyBB = llvm::BasicBlock::Create(*context, "sw.cnt.body", function);
        llvm::BasicBlock* cDoneBB = llvm::BasicBlock::Create(*context, "sw.cnt.done", function);
        builder->CreateBr(cLoopBB);

        builder->SetInsertPoint(cLoopBB);
        llvm::PHINode* ci     = builder->CreatePHI(getDefaultType(), 2, "sw.ci");
        llvm::PHINode* cnt    = builder->CreatePHI(getDefaultType(), 2, "sw.cnt");
        llvm::PHINode* inWord = builder->CreatePHI(llvm::Type::getInt1Ty(*context), 2, "sw.inword");
        ci->addIncoming(zero, cPreBB);
        cnt->addIncoming(zero, cPreBB);
        inWord->addIncoming(llvm::ConstantInt::getFalse(*context), cPreBB);
        llvm::Value* ccond = builder->CreateICmpULT(ci, strLen, "sw.ccond");
        builder->CreateCondBr(ccond, cBodyBB, cDoneBB);

        builder->SetInsertPoint(cBodyBB);
        llvm::Value* cCharPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context),
                                                            srcData, ci, "sw.ccharptr");
        auto* cChLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), cCharPtr, "sw.cch");
        cChLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* cCh32 = builder->CreateZExt(cChLoad, llvm::Type::getInt32Ty(*context), "sw.cch32", false);
        nonNegValues_.insert(cCh32);
        llvm::Value* cIsSpace = builder->CreateICmpNE(
            builder->CreateCall(getOrDeclareIsspace(), {cCh32}, "sw.issp"),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "sw.isspace");
        // inWord transitions: if !isSpace && !prevInWord → new word (cnt++)
        llvm::Value* enterWord = builder->CreateAnd(
            builder->CreateNot(cIsSpace, "sw.notsp"),
            builder->CreateNot(inWord, "sw.notinw"), "sw.enter");
        llvm::Value* newCnt = builder->CreateAdd(
            cnt, builder->CreateZExt(enterWord, getDefaultType(), "sw.inc", false), "sw.newcnt", true, true);
        llvm::Value* newInWord = builder->CreateNot(cIsSpace, "sw.newinword");
        llvm::Value* nextCi = builder->CreateAdd(ci, one, "sw.nextci", true, true);
        ci->addIncoming(nextCi, cBodyBB);
        cnt->addIncoming(newCnt, cBodyBB);
        inWord->addIncoming(newInWord, cBodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(cLoopBB)));

        builder->SetInsertPoint(cDoneBB);
        // cnt = number of words found

        // Allocate result array: (cnt + 1) * 8
        llvm::Value* resSlots = builder->CreateAdd(cnt, one, "sw.rslots", true, true);
        llvm::Value* resBytes = builder->CreateMul(resSlots, eight, "sw.rbytes", true, true);
        llvm::Value* arrBuf   = builder->CreateCall(getOrDeclareMalloc(), {resBytes}, "sw.arr");
        llvm::cast<llvm::CallInst>(arrBuf)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        builder->CreateStore(cnt, arrBuf);

        // ── Pass 2: extract words ────────────────────────────────────────────
        llvm::BasicBlock* ePreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* eLoopBB = llvm::BasicBlock::Create(*context, "sw.ext.loop", function);
        llvm::BasicBlock* eBodyBB = llvm::BasicBlock::Create(*context, "sw.ext.body", function);
        llvm::BasicBlock* eDoneBB = llvm::BasicBlock::Create(*context, "sw.ext.done", function);
        builder->CreateBr(eLoopBB);

        builder->SetInsertPoint(eLoopBB);
        llvm::PHINode* ei      = builder->CreatePHI(getDefaultType(), 2, "sw.ei");
        llvm::PHINode* ew_idx  = builder->CreatePHI(getDefaultType(), 2, "sw.ew_idx");
        llvm::PHINode* ew_start= builder->CreatePHI(getDefaultType(), 2, "sw.ew_start");
        llvm::PHINode* ew_in   = builder->CreatePHI(llvm::Type::getInt1Ty(*context), 2, "sw.ew_in");
        ei->addIncoming(zero, ePreBB);
        ew_idx->addIncoming(zero, ePreBB);
        ew_start->addIncoming(zero, ePreBB);
        ew_in->addIncoming(llvm::ConstantInt::getFalse(*context), ePreBB);
        llvm::Value* econd = builder->CreateICmpULE(ei, strLen, "sw.econd");
        builder->CreateCondBr(econd, eBodyBB, eDoneBB);

        builder->SetInsertPoint(eBodyBB);
        llvm::Value* atEnd = builder->CreateICmpEQ(ei, strLen, "sw.atend");
        llvm::Value* eCharPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context),
                                                            srcData, ei, "sw.echarptr");
        auto* eChLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), eCharPtr, "sw.ech");
        eChLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* eCh32 = builder->CreateZExt(eChLoad, llvm::Type::getInt32Ty(*context), "sw.ech32", false);
        nonNegValues_.insert(eCh32);
        // At end: treat as space
        llvm::Value* eIsSpaceRaw = builder->CreateICmpNE(
            builder->CreateCall(getOrDeclareIsspace(), {eCh32}, "sw.eissp"),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "sw.eisspace");
        llvm::Value* eIsSpace = builder->CreateOr(eIsSpaceRaw, atEnd, "sw.eissporatend");
        // Was in word, now at space/end → emit substring
        llvm::Value* emitWord = builder->CreateAnd(ew_in, eIsSpace, "sw.emit");

        // Word emission block
        llvm::BasicBlock* emitBB = llvm::BasicBlock::Create(*context, "sw.emit", function);
        llvm::BasicBlock* contBB = llvm::BasicBlock::Create(*context, "sw.cont", function);
        builder->CreateCondBr(emitWord, emitBB, contBB);

        builder->SetInsertPoint(emitBB);
        llvm::Value* wordLen  = builder->CreateSub(ei, ew_start, "sw.wlen", false, true);
        llvm::Value* wSrcData = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context),
                                                            srcData, ew_start, "sw.wsrc");
        llvm::Value* wHdr     = emitAllocString(wordLen, wordLen, "sw.whdr");
        llvm::Value* wDst     = emitStringData(wHdr, "sw.wdst");
        builder->CreateCall(getOrDeclareMemcpy(), {wDst, wSrcData, wordLen});
        llvm::Value* wNul = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), wDst, wordLen, "sw.wnul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), wNul)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* wInt  = builder->CreatePtrToInt(wHdr, getDefaultType(), "sw.wint");
        llvm::Value* wSlot = builder->CreateAdd(ew_idx, one, "sw.wslot", true, true);
        llvm::Value* wSlotPtr = builder->CreateInBoundsGEP(getDefaultType(), arrBuf, wSlot, "sw.wslotptr");
        builder->CreateStore(wInt, wSlotPtr);
        llvm::Value* nextEwIdx = builder->CreateAdd(ew_idx, one, "sw.nwidx", true, true);
        builder->CreateBr(contBB);

        builder->SetInsertPoint(contBB);
        // Update tracking state
        llvm::PHINode* mergedIdx   = builder->CreatePHI(getDefaultType(), 2, "sw.midx");
        mergedIdx->addIncoming(ew_idx,    eBodyBB);
        mergedIdx->addIncoming(nextEwIdx, emitBB);
        // New word start: if entering word (was space, now not space) → ei; else keep ew_start
        llvm::Value* enteringWord = builder->CreateAnd(builder->CreateNot(ew_in), builder->CreateNot(eIsSpace));
        llvm::Value* newStart = builder->CreateSelect(enteringWord, ei, ew_start, "sw.newstart");
        llvm::Value* newIn    = builder->CreateNot(eIsSpace, "sw.newin");
        llvm::Value* nextEi   = builder->CreateAdd(ei, one, "sw.nextei", true, true);
        ei->addIncoming(nextEi, contBB);
        ew_idx->addIncoming(mergedIdx, contBB);
        ew_start->addIncoming(newStart, contBB);
        ew_in->addIncoming(newIn, contBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(eLoopBB)));

        builder->SetInsertPoint(eDoneBB);
        return arrBuf;
    }

    // str_title(s) → title-case: first char of each word upper, rest lower
    if (bid == BuiltinId::STR_TITLE) {
        validateArgCount(expr, "str_title", 1);
        // Compile-time fold
        if (auto s = tryFoldStr(expr->arguments[0].get())) {
            std::string result = *s;
            bool afterSpace = true;
            for (size_t i = 0; i < result.size(); ++i) {
                unsigned char c = static_cast<unsigned char>(result[i]);
                if (std::isspace(c)) { afterSpace = true; }
                else if (afterSpace) { result[i] = static_cast<char>(std::toupper(c)); afterSpace = false; }
                else { result[i] = static_cast<char>(std::tolower(c)); }
            }
            llvm::GlobalVariable* gv = internString(result.c_str());
            stringReturningFunctions_.insert("str_title");
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                gv->getValueType(), gv,
                llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, ptrTy, "title.ptr");
        llvm::Value* strLen  = emitStringLen(strPtr, "title.len");
        llvm::Value* hdr     = emitAllocString(strLen, strLen, "title");
        llvm::Value* dstData = emitStringData(hdr, "title.data");
        builder->CreateCall(getOrDeclareStrcpy(), {dstData, emitStringData(strPtr, "title.srcdata")});

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preBB  = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "title.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "title.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "title.done", function);

        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        // PHI: idx (position), afterSpace (bool: previous char was space or start-of-string)
        llvm::PHINode* idx        = builder->CreatePHI(getDefaultType(), 2, "title.idx");
        llvm::PHINode* afterSpace = builder->CreatePHI(llvm::Type::getInt1Ty(*context), 2, "title.aftersp");
        idx->addIncoming(zero, preBB);
        afterSpace->addIncoming(llvm::ConstantInt::getTrue(*context), preBB); // start of string counts as "after space"

        llvm::Value* loopDone = builder->CreateICmpUGE(idx, strLen, "title.loopdone");
        builder->CreateCondBr(loopDone, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* charPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context),
                                                           dstData, idx, "title.charptr");
        auto* chLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "title.ch");
        chLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* ch32 = builder->CreateZExt(chLoad, llvm::Type::getInt32Ty(*context), "title.ch32", false);
        nonNegValues_.insert(ch32);
        // isSpace check
        llvm::Value* isSpaceVal = builder->CreateICmpNE(
            builder->CreateCall(getOrDeclareIsspace(), {ch32}, "title.issp"),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "title.isspace");
        // If afterSpace && !isSpace → toupper; else if !isSpace → tolower; else keep
        llvm::Value* doUpper = builder->CreateAnd(afterSpace, builder->CreateNot(isSpaceVal), "title.doup");
        llvm::Value* upper   = builder->CreateCall(getOrDeclareToupper(), {ch32}, "title.upper");
        llvm::Value* lower   = builder->CreateCall(getOrDeclareTolower(), {ch32}, "title.lower");
        llvm::Value* upper8  = builder->CreateTrunc(upper, llvm::Type::getInt8Ty(*context), "title.up8");
        llvm::Value* lower8  = builder->CreateTrunc(lower, llvm::Type::getInt8Ty(*context), "title.lo8");
        // Choose: if doUpper → upper8; if isSpace → ch (original); else → lower8
        llvm::Value* notSpace8 = builder->CreateSelect(doUpper, upper8, lower8, "title.notsp8");
        llvm::Value* final8    = builder->CreateSelect(isSpaceVal, chLoad, notSpace8, "title.final8");
        auto* st = builder->CreateStore(final8, charPtr);
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);

        llvm::Value* nextIdx       = builder->CreateAdd(idx, one, "title.next", true, true);
        llvm::Value* nextAfterSpace = isSpaceVal; // next iteration: afterSpace = isSpace
        idx->addIncoming(nextIdx, bodyBB);
        afterSpace->addIncoming(nextAfterSpace, bodyBB);
        { attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB))); }

        builder->SetInsertPoint(doneBB);
        stringReturningFunctions_.insert("str_title");
        return hdr;
    }

    // str_swapcase(s) → swap upper↔lower for each ASCII alphabetic character
    // Uses the XOR trick: 'A'–'Z' and 'a'–'z' differ by exactly 0x20;
    // detecting alpha with (c | 0x20) in ['a'..'z'] (unsigned range check)
    if (bid == BuiltinId::STR_SWAPCASE) {
        validateArgCount(expr, "str_swapcase", 1);
        // Compile-time fold
        if (auto s = tryFoldStr(expr->arguments[0].get())) {
            std::string result = *s;
            for (size_t i = 0; i < result.size(); ++i) {
                unsigned char c = static_cast<unsigned char>(result[i]);
                if (std::isupper(c))      result[i] = static_cast<char>(std::tolower(c));
                else if (std::islower(c)) result[i] = static_cast<char>(std::toupper(c));
            }
            llvm::GlobalVariable* gv = internString(result.c_str());
            stringReturningFunctions_.insert("str_swapcase");
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                gv->getValueType(), gv,
                llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, ptrTy, "swap.ptr");
        llvm::Value* strLen  = emitStringLen(strPtr, "swap.len");
        llvm::Value* hdr     = emitAllocString(strLen, strLen, "swap");
        llvm::Value* dstData = emitStringData(hdr, "swap.data");
        builder->CreateCall(getOrDeclareStrcpy(), {dstData, emitStringData(strPtr, "swap.srcdata")});

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
        // Constants for alpha range check: (c | 0x20) in ['a'=97 .. 'z'=122]
        llvm::Value* mask97  = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0x20);
        llvm::Value* alpha_a = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 97);  // 'a'
        llvm::Value* alpha_z = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 122); // 'z'
        llvm::Value* xorMask = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0x20);

        emitCountingLoop("swap", strLen, zero, 4, [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
            llvm::Value* charPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context),
                                                               dstData, idx, "swap.charptr");
            auto* chLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "swap.ch");
            chLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* ch32 = builder->CreateZExt(chLoad, llvm::Type::getInt32Ty(*context), "swap.ch32", false);
            nonNegValues_.insert(ch32);
            // isAlpha: (ch32 | 0x20) >= 'a' && (ch32 | 0x20) <= 'z'
            llvm::Value* ch32Masked = builder->CreateOr(ch32, mask97, "swap.masked");
            llvm::Value* geA = builder->CreateICmpUGE(ch32Masked, alpha_a, "swap.gea");
            llvm::Value* leZ = builder->CreateICmpULE(ch32Masked, alpha_z, "swap.lez");
            llvm::Value* isAlpha = builder->CreateAnd(geA, leZ, "swap.isalpha");
            // flipped = ch ^ 0x20 (swaps case for alpha)
            llvm::Value* flipped = builder->CreateXor(chLoad, xorMask, "swap.flipped");
            llvm::Value* final8  = builder->CreateSelect(isAlpha, flipped, chLoad, "swap.final");
            auto* st = builder->CreateStore(final8, charPtr);
            st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* nextIdx = builder->CreateAdd(idx, one, "swap.next", true, true);
            idx->addIncoming(nextIdx, builder->GetInsertBlock());
            attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        });
        stringReturningFunctions_.insert("str_swapcase");
        return hdr;
    }

    // ── Round-75 builtins ────────────────────────────────────────────────────

    // array_flatten(arr) → flatten one level of array nesting.
    // The outer array's elements are i64 values that are pointers to inner arrays.
    // Returns a new flat array with all inner elements concatenated in order.
    if (bid == BuiltinId::ARRAY_FLATTEN) {
        validateArgCount(expr, "array_flatten", 1);
        llvm::Value* outerArg = generateExpression(expr->arguments[0].get());
        outerArg = toDefaultType(outerArg);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* outerPtr = builder->CreateIntToPtr(outerArg, ptrTy, "flat.outerptr");
        llvm::Value* outerLen = emitLoadArrayLen(outerPtr, "flat.outerlen");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);

        // ── Pass 1: sum all inner-array lengths ─────────────────────────────
        llvm::BasicBlock* p1PreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* p1LoopBB = llvm::BasicBlock::Create(*context, "flat.p1loop", parentFn);
        llvm::BasicBlock* p1BodyBB = llvm::BasicBlock::Create(*context, "flat.p1body", parentFn);
        llvm::BasicBlock* p1DoneBB = llvm::BasicBlock::Create(*context, "flat.p1done", parentFn);
        auto* p1SkipW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(builder->CreateICmpSGT(outerLen, zero, "flat.p1gt0"), p1LoopBB, p1DoneBB, p1SkipW);

        builder->SetInsertPoint(p1LoopBB);
        llvm::PHINode* p1i    = builder->CreatePHI(getDefaultType(), 2, "flat.p1i");
        llvm::PHINode* p1total= builder->CreatePHI(getDefaultType(), 2, "flat.p1total");
        p1i->addIncoming(zero, p1PreBB);
        p1total->addIncoming(zero, p1PreBB);
        builder->CreateCondBr(builder->CreateICmpSLT(p1i, outerLen, "flat.p1cond"), p1BodyBB, p1DoneBB,
                              llvm::MDBuilder(*context).createBranchWeights(1000, 1));

        builder->SetInsertPoint(p1BodyBB);
        llvm::Value* p1slotIdx  = builder->CreateAdd(p1i, one, "flat.p1slot", true, true);
        llvm::Value* p1elemPtr  = builder->CreateInBoundsGEP(getDefaultType(), outerPtr, p1slotIdx, "flat.p1eptr");
        llvm::Value* p1elemI64  = emitLoadArrayElem(p1elemPtr, "flat.p1elem");
        // Each element is a pointer-to-inner-array stored as i64.
        llvm::Value* innerPtr1  = builder->CreateIntToPtr(p1elemI64, ptrTy, "flat.p1iptr");
        llvm::Value* innerLen1  = emitLoadArrayLen(innerPtr1, "flat.p1ilen");
        llvm::Value* p1newTotal = builder->CreateAdd(p1total, innerLen1, "flat.p1ntotal", false, true);
        llvm::Value* p1next     = builder->CreateAdd(p1i, one, "flat.p1next", true, true);
        p1i->addIncoming(p1next, builder->GetInsertBlock());
        p1total->addIncoming(p1newTotal, builder->GetInsertBlock());
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(p1LoopBB)));

        builder->SetInsertPoint(p1DoneBB);
        llvm::PHINode* totalLen = builder->CreatePHI(getDefaultType(), 2, "flat.total");
        totalLen->addIncoming(zero, p1PreBB);
        totalLen->addIncoming(p1total, p1LoopBB);
        nonNegValues_.insert(totalLen);

        // ── Allocate output array (totalLen + 1) * 8 bytes ──────────────────
        llvm::Value* eight  = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots  = builder->CreateAdd(totalLen, one, "flat.slots", true, true);
        llvm::Value* bytes  = builder->CreateMul(slots, eight, "flat.bytes", true, true);
        llvm::Value* outBuf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "flat.outbuf");
        llvm::cast<llvm::CallInst>(outBuf)->addRetAttr(llvm::Attribute::NonNull);
        emitStoreArrayLen(totalLen, outBuf);

        // ── Pass 2: copy elements from each inner array ──────────────────────
        llvm::BasicBlock* p2PreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* p2LoopBB = llvm::BasicBlock::Create(*context, "flat.p2loop", parentFn);
        llvm::BasicBlock* p2BodyBB = llvm::BasicBlock::Create(*context, "flat.p2body", parentFn);
        llvm::BasicBlock* p2DoneBB = llvm::BasicBlock::Create(*context, "flat.p2done", parentFn);
        auto* p2SkipW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(builder->CreateICmpSGT(outerLen, zero, "flat.p2gt0"), p2LoopBB, p2DoneBB, p2SkipW);

        builder->SetInsertPoint(p2LoopBB);
        llvm::PHINode* p2i   = builder->CreatePHI(getDefaultType(), 2, "flat.p2i");
        llvm::PHINode* p2dst = builder->CreatePHI(getDefaultType(), 2, "flat.p2dst");
        p2i->addIncoming(zero, p2PreBB);
        p2dst->addIncoming(one, p2PreBB); // output slot index starts at 1 (skip header)
        builder->CreateCondBr(builder->CreateICmpSLT(p2i, outerLen, "flat.p2cond"), p2BodyBB, p2DoneBB,
                              llvm::MDBuilder(*context).createBranchWeights(1000, 1));

        builder->SetInsertPoint(p2BodyBB);
        llvm::Value* p2slotIdx = builder->CreateAdd(p2i, one, "flat.p2slot", true, true);
        llvm::Value* p2elemPtr = builder->CreateInBoundsGEP(getDefaultType(), outerPtr, p2slotIdx, "flat.p2eptr");
        llvm::Value* p2elemI64 = emitLoadArrayElem(p2elemPtr, "flat.p2elem");
        llvm::Value* innerPtr2 = builder->CreateIntToPtr(p2elemI64, ptrTy, "flat.p2iptr");
        llvm::Value* innerLen2 = emitLoadArrayLen(innerPtr2, "flat.p2ilen");

        // Inner copy loop: for j in 0..innerLen2: outBuf[p2dst+j] = innerArr[j+1]
        llvm::BasicBlock* icPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* icLoopBB = llvm::BasicBlock::Create(*context, "flat.icloop", parentFn);
        llvm::BasicBlock* icBodyBB = llvm::BasicBlock::Create(*context, "flat.icbody", parentFn);
        llvm::BasicBlock* icDoneBB = llvm::BasicBlock::Create(*context, "flat.icdone", parentFn);
        auto* icSkipW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(builder->CreateICmpSGT(innerLen2, zero, "flat.icgt0"), icLoopBB, icDoneBB, icSkipW);

        builder->SetInsertPoint(icLoopBB);
        llvm::PHINode* icj = builder->CreatePHI(getDefaultType(), 2, "flat.icj");
        icj->addIncoming(zero, icPreBB);
        builder->CreateCondBr(builder->CreateICmpSLT(icj, innerLen2, "flat.iccond"), icBodyBB, icDoneBB,
                              llvm::MDBuilder(*context).createBranchWeights(1000, 1));

        builder->SetInsertPoint(icBodyBB);
        llvm::Value* srcIdx = builder->CreateAdd(icj, one, "flat.icsrcidx", true, true);
        llvm::Value* srcPtr = builder->CreateInBoundsGEP(getDefaultType(), innerPtr2, srcIdx, "flat.icsrcptr");
        llvm::Value* elem   = emitLoadArrayElem(srcPtr, "flat.icelem");
        llvm::Value* dstIdx = builder->CreateAdd(p2dst, icj, "flat.icdstidx", false, true);
        llvm::Value* dstPtr = builder->CreateInBoundsGEP(getDefaultType(), outBuf, dstIdx, "flat.icdstptr");
        auto* icSt = builder->CreateStore(elem, dstPtr);
        icSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        llvm::Value* icnext = builder->CreateAdd(icj, one, "flat.icnext", true, true);
        icj->addIncoming(icnext, builder->GetInsertBlock());
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(icLoopBB)));

        builder->SetInsertPoint(icDoneBB);
        // Advance p2dst by innerLen2 and p2i by 1
        llvm::Value* p2dstNext = builder->CreateAdd(p2dst, innerLen2, "flat.p2dstnext", false, true);
        llvm::Value* p2next    = builder->CreateAdd(p2i, one, "flat.p2next", true, true);
        p2i->addIncoming(p2next, builder->GetInsertBlock());
        p2dst->addIncoming(p2dstNext, builder->GetInsertBlock());
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(p2LoopBB)));

        builder->SetInsertPoint(p2DoneBB);
        arrayReturningFunctions_.insert("array_flatten");
        return outBuf;
    }

    // array_min_by(arr, fn) → element of arr with minimum fn(element)
    // array_max_by(arr, fn) → element of arr with maximum fn(element)
    // Both return 0 (and skip the loop) when arr is empty.
    if (bid == BuiltinId::ARRAY_MIN_BY || bid == BuiltinId::ARRAY_MAX_BY) {
        const bool isMax = (bid == BuiltinId::ARRAY_MAX_BY);
        const char* builtinName = isMax ? "array_max_by" : "array_min_by";
        validateArgCount(expr, builtinName, 2);

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, ptrTy, "amby.ptr");
        llvm::Value* arrLen = emitLoadArrayLen(arrPtr, "amby.len");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);

        // Extract the key function name.
        llvm::Function* keyFn = nullptr;
        {
            const std::string kname = extractFnName(expr->arguments[1].get());
            if (!kname.empty()) keyFn = module->getFunction(kname);
        }
        if (!keyFn) {
            llvm::errs() << "warning: " << builtinName
                         << " could not resolve key function; returning 0\n";
            return llvm::ConstantInt::get(getDefaultType(), 0);
        }

        llvm::BasicBlock* preBB  = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "amby.loop", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "amby.body", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "amby.done", parentFn);

        // Load first element as initial best (or skip if empty).
        llvm::Value* firstPtr  = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, one, "amby.fptr");
        llvm::Value* firstElem = emitLoadArrayElem(firstPtr, "amby.felem");
        // firstKey = fn(firstElem)
        llvm::Value* firstKey  = builder->CreateCall(keyFn, {firstElem}, "amby.fkey");
        firstKey = toDefaultType(firstKey);

        // Guard: if len == 0 skip loop, return 0.
        auto* skipW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(builder->CreateICmpSGT(arrLen, zero, "amby.gt0"), loopBB, doneBB, skipW);

        // Loop starting at index 1 (we already initialised best from index 0).
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx      = builder->CreatePHI(getDefaultType(), 2, "amby.idx");
        llvm::PHINode* bestElem = builder->CreatePHI(getDefaultType(), 2, "amby.best");
        llvm::PHINode* bestKey  = builder->CreatePHI(getDefaultType(), 2, "amby.bestkey");
        idx->addIncoming(one, preBB);
        bestElem->addIncoming(firstElem, preBB);
        bestKey->addIncoming(firstKey, preBB);
        builder->CreateCondBr(builder->CreateICmpSLT(idx, arrLen, "amby.cond"), bodyBB, doneBB,
                              llvm::MDBuilder(*context).createBranchWeights(1000, 1));

        builder->SetInsertPoint(bodyBB);
        llvm::Value* slotIdx  = builder->CreateAdd(idx, one, "amby.slot", true, true);
        llvm::Value* elemPtr  = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, slotIdx, "amby.eptr");
        llvm::Value* curElem  = emitLoadArrayElem(elemPtr, "amby.cur");
        llvm::Value* curKey   = builder->CreateCall(keyFn, {curElem}, "amby.ckey");
        curKey = toDefaultType(curKey);
        // isBetter: for max use curKey > bestKey, for min use curKey < bestKey
        llvm::Value* isBetter = isMax
            ? builder->CreateICmpSGT(curKey, bestKey, "amby.isbetter")
            : builder->CreateICmpSLT(curKey, bestKey, "amby.isbetter");
        llvm::Value* newBestElem = builder->CreateSelect(isBetter, curElem, bestElem, "amby.newbest");
        llvm::Value* newBestKey  = builder->CreateSelect(isBetter, curKey,  bestKey,  "amby.newbkey");
        llvm::Value* next = builder->CreateAdd(idx, one, "amby.next", true, true);
        idx->addIncoming(next, builder->GetInsertBlock());
        bestElem->addIncoming(newBestElem, builder->GetInsertBlock());
        bestKey->addIncoming(newBestKey, builder->GetInsertBlock());
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "amby.result");
        result->addIncoming(zero, preBB);          // empty array → 0
        result->addIncoming(bestElem, loopBB);
        return result;
    }

    // str_to_lines(s) → split s on newlines (\n, \r\n) and return string[].
    // Each line is a freshly allocated string fat-pointer (no trailing \n/\r).
    // Empty trailing line (s ends with \n) is excluded (Python str.splitlines behaviour).
    if (bid == BuiltinId::STR_TO_LINES) {
        validateArgCount(expr, "str_to_lines", 1);

        // Compile-time fold: if the argument is a string literal, fold now.
        if (auto sConst = tryFoldStr(expr->arguments[0].get())) {
            // Split by \n (strip \r)
            std::vector<std::string> lines;
            size_t start = 0;
            for (size_t i = 0; i <= sConst->size(); ++i) {
                if (i == sConst->size() || (*sConst)[i] == '\n') {
                    std::string line = sConst->substr(start, i - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    lines.push_back(line);
                    start = i + 1;
                }
            }
            // Remove empty trailing line produced by a terminal \n.
            if (!lines.empty() && lines.back().empty()) lines.pop_back();

            const int64_t nLines = static_cast<int64_t>(lines.size());
            llvm::Value* eight  = llvm::ConstantInt::get(getDefaultType(), 8);
            llvm::Value* slots  = llvm::ConstantInt::get(getDefaultType(), nLines + 1);
            llvm::Value* bytes  = builder->CreateMul(slots, eight, "stl.bytes", true, true);
            llvm::Value* outBuf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "stl.out");
            llvm::cast<llvm::CallInst>(outBuf)->addRetAttr(llvm::Attribute::NonNull);
            emitStoreArrayLen(llvm::ConstantInt::get(getDefaultType(), nLines), outBuf);
            for (int64_t li = 0; li < nLines; ++li) {
                llvm::Value* strPtr = internString(lines[static_cast<size_t>(li)]);
                llvm::Value* asInt  = builder->CreatePtrToInt(strPtr, getDefaultType(), "stl.sint");
                llvm::Value* slot   = llvm::ConstantInt::get(getDefaultType(), li + 1);
                llvm::Value* dst    = builder->CreateInBoundsGEP(getDefaultType(), outBuf, slot, "stl.dst");
                auto* st = builder->CreateStore(asInt, dst);
                st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            }
            arrayReturningFunctions_.insert("str_to_lines");
            stringReturningFunctions_.insert("str_to_lines");
            return outBuf;
        }

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, ptrTy, "stl.sptr");
        llvm::Value* strLen  = emitStringLen(strPtr, "stl.slen");
        llvm::Value* strData = emitStringData(strPtr, "stl.sdata");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* i8zero = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0);
        llvm::Value* nlChar = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), '\n');
        llvm::Value* crChar = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), '\r');

        // ── Pass 1: count lines (newline-separated) ─────────────────────────
        llvm::BasicBlock* c1PreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* c1LoopBB = llvm::BasicBlock::Create(*context, "stl.c1loop", parentFn);
        llvm::BasicBlock* c1BodyBB = llvm::BasicBlock::Create(*context, "stl.c1body", parentFn);
        llvm::BasicBlock* c1DoneBB = llvm::BasicBlock::Create(*context, "stl.c1done", parentFn);
        auto* c1SkipW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(builder->CreateICmpSGT(strLen, zero, "stl.c1gt0"), c1LoopBB, c1DoneBB, c1SkipW);

        builder->SetInsertPoint(c1LoopBB);
        llvm::PHINode* c1i   = builder->CreatePHI(getDefaultType(), 2, "stl.c1i");
        llvm::PHINode* c1cnt = builder->CreatePHI(getDefaultType(), 2, "stl.c1cnt");
        c1i->addIncoming(zero, c1PreBB);
        c1cnt->addIncoming(one, c1PreBB); // start with 1 line (the first line before any \n)
        builder->CreateCondBr(builder->CreateICmpSLT(c1i, strLen, "stl.c1cond"), c1BodyBB, c1DoneBB,
                              llvm::MDBuilder(*context).createBranchWeights(1000, 1));

        builder->SetInsertPoint(c1BodyBB);
        llvm::Value* c1cp  = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strData, c1i, "stl.c1cp");
        llvm::Value* c1ch  = builder->CreateLoad(llvm::Type::getInt8Ty(*context), c1cp, "stl.c1ch");
        llvm::Value* isNL1 = builder->CreateICmpEQ(c1ch, nlChar, "stl.isnl1");
        llvm::Value* c1inc = builder->CreateAdd(c1cnt, one, "stl.c1inc", false, true);
        llvm::Value* c1new = builder->CreateSelect(isNL1, c1inc, c1cnt, "stl.c1new");
        llvm::Value* c1nx  = builder->CreateAdd(c1i, one, "stl.c1nx", true, true);
        c1i->addIncoming(c1nx, builder->GetInsertBlock());
        c1cnt->addIncoming(c1new, builder->GetInsertBlock());
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(c1LoopBB)));

        builder->SetInsertPoint(c1DoneBB);
        llvm::PHINode* rawCount = builder->CreatePHI(getDefaultType(), 2, "stl.rawcnt");
        rawCount->addIncoming(zero, c1PreBB);
        rawCount->addIncoming(c1cnt, c1LoopBB);
        // Subtract 1 if the string ends with \n (trailing empty line not included)
        llvm::Value* lastIdx  = builder->CreateSub(strLen, one, "stl.lastidx", false, true);
        llvm::Value* lastCp   = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strData, lastIdx, "stl.lcp");
        // Guard: only load if strLen > 0 (we're already past the len==0 check above)
        llvm::Value* lastCh   = builder->CreateLoad(llvm::Type::getInt8Ty(*context), lastCp, "stl.lch");
        llvm::Value* endsNL   = builder->CreateICmpEQ(lastCh, nlChar, "stl.endsnl");
        llvm::Value* lineCount = builder->CreateSelect(
            builder->CreateAnd(builder->CreateICmpSGT(strLen, zero), endsNL),
            builder->CreateSub(rawCount, one, "stl.lcdec", false, true),
            rawCount, "stl.linecount");
        nonNegValues_.insert(lineCount);

        // ── Allocate output array ─────────────────────────────────────────────
        llvm::Value* slots  = builder->CreateAdd(lineCount, one, "stl.slots", true, true);
        llvm::Value* bytes  = builder->CreateMul(slots, eight, "stl.bytes", true, true);
        llvm::Value* outBuf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "stl.outbuf");
        llvm::cast<llvm::CallInst>(outBuf)->addRetAttr(llvm::Attribute::NonNull);
        emitStoreArrayLen(lineCount, outBuf);

        // ── Pass 2: extract each line as a fat-pointer string ────────────────
        llvm::BasicBlock* p2PreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* p2LoopBB = llvm::BasicBlock::Create(*context, "stl.p2loop", parentFn);
        llvm::BasicBlock* p2BodyBB = llvm::BasicBlock::Create(*context, "stl.p2body", parentFn);
        llvm::BasicBlock* p2DoneBB = llvm::BasicBlock::Create(*context, "stl.p2done", parentFn);
        auto* p2SkipW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(builder->CreateICmpSGT(lineCount, zero, "stl.p2gt0"), p2LoopBB, p2DoneBB, p2SkipW);

        builder->SetInsertPoint(p2LoopBB);
        llvm::PHINode* p2i    = builder->CreatePHI(getDefaultType(), 2, "stl.p2i");
        llvm::PHINode* p2pos  = builder->CreatePHI(getDefaultType(), 2, "stl.p2pos"); // byte offset of line start
        llvm::PHINode* p2out  = builder->CreatePHI(getDefaultType(), 2, "stl.p2out"); // output slot index
        p2i->addIncoming(zero, p2PreBB);
        p2pos->addIncoming(zero, p2PreBB);
        p2out->addIncoming(one, p2PreBB);
        builder->CreateCondBr(builder->CreateICmpSLT(p2i, lineCount, "stl.p2cond"), p2BodyBB, p2DoneBB,
                              llvm::MDBuilder(*context).createBranchWeights(1000, 1));

        // Body: find the end of the current line (scan forward to \n or end)
        builder->SetInsertPoint(p2BodyBB);
        // Inner scan loop to find end of line
        llvm::BasicBlock* scanPreBB  = builder->GetInsertBlock();
        llvm::BasicBlock* scanLoopBB = llvm::BasicBlock::Create(*context, "stl.scan", parentFn);
        llvm::BasicBlock* scanDoneBB = llvm::BasicBlock::Create(*context, "stl.scandone", parentFn);
        builder->CreateBr(scanLoopBB);

        builder->SetInsertPoint(scanLoopBB);
        llvm::PHINode* scani = builder->CreatePHI(getDefaultType(), 2, "stl.scani");
        scani->addIncoming(p2pos, scanPreBB);
        llvm::Value* scanCond = builder->CreateICmpSLT(scani, strLen, "stl.scancond");
        // Also stop at \n
        llvm::BasicBlock* scanCheckBB = llvm::BasicBlock::Create(*context, "stl.scancheck", parentFn);
        builder->CreateCondBr(scanCond, scanCheckBB, scanDoneBB);

        builder->SetInsertPoint(scanCheckBB);
        llvm::Value* scanCp = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strData, scani, "stl.scancp");
        llvm::Value* scanCh = builder->CreateLoad(llvm::Type::getInt8Ty(*context), scanCp, "stl.scanch");
        llvm::Value* isNLs  = builder->CreateICmpEQ(scanCh, nlChar, "stl.isnls");
        llvm::Value* scanNext = builder->CreateAdd(scani, one, "stl.scannx", true, true);
        scani->addIncoming(scanNext, scanCheckBB);
        builder->CreateCondBr(isNLs, scanDoneBB, scanLoopBB);

        builder->SetInsertPoint(scanDoneBB);
        llvm::PHINode* lineEnd = builder->CreatePHI(getDefaultType(), 3, "stl.lineend");
        lineEnd->addIncoming(scani, scanLoopBB);   // hit end-of-string
        lineEnd->addIncoming(scani, scanCheckBB);  // hit \n

        // lineLen = lineEnd - p2pos  (strip trailing \r if present)
        llvm::Value* rawLineLen = builder->CreateSub(lineEnd, p2pos, "stl.rawll", false, true);
        // Check if last char of the line is \r
        llvm::BasicBlock* crCheckBB  = llvm::BasicBlock::Create(*context, "stl.crcheck", parentFn);
        llvm::BasicBlock* crMergeBB  = llvm::BasicBlock::Create(*context, "stl.crmerge", parentFn);
        builder->CreateCondBr(builder->CreateICmpSGT(rawLineLen, zero, "stl.llgt0"), crCheckBB, crMergeBB);

        builder->SetInsertPoint(crCheckBB);
        llvm::Value* crIdx = builder->CreateSub(lineEnd, one, "stl.cridx", false, true);
        llvm::Value* crCp  = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strData, crIdx, "stl.crcp");
        llvm::Value* crCh  = builder->CreateLoad(llvm::Type::getInt8Ty(*context), crCp, "stl.crch");
        llvm::Value* isCR  = builder->CreateICmpEQ(crCh, crChar, "stl.iscr");
        builder->CreateBr(crMergeBB);

        builder->SetInsertPoint(crMergeBB);
        llvm::PHINode* isCRPhi = builder->CreatePHI(builder->getInt1Ty(), 2, "stl.iscrphi");
        isCRPhi->addIncoming(builder->getFalse(), scanDoneBB);
        isCRPhi->addIncoming(isCR, crCheckBB);
        llvm::Value* lineLen = builder->CreateSelect(
            isCRPhi,
            builder->CreateSub(rawLineLen, one, "stl.lldec", false, true),
            rawLineLen, "stl.linelen");
        nonNegValues_.insert(lineLen);

        // Allocate a fat-pointer string for this line: len=lineLen, data=strData+p2pos
        llvm::Value* lineHdr  = emitAllocString(lineLen, lineLen, "stl.lhdr");
        llvm::Value* lineData = emitStringData(lineHdr, "stl.ldata");
        llvm::Value* srcStart = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strData, p2pos, "stl.srcst");
        // Copy lineLen bytes + null terminator
        llvm::Value* copySize = builder->CreateAdd(lineLen, one, "stl.cpsize", true, true);
        builder->CreateCall(getOrDeclareMemcpy(), {lineData, srcStart, copySize});
        // Ensure null terminator at position lineLen
        llvm::Value* nullPos = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), lineData, lineLen, "stl.np");
        builder->CreateStore(i8zero, nullPos);

        // Store fat-pointer as i64 into output array
        llvm::Value* lineHdrInt = builder->CreatePtrToInt(lineHdr, getDefaultType(), "stl.lhint");
        llvm::Value* outSlotPtr = builder->CreateInBoundsGEP(getDefaultType(), outBuf, p2out, "stl.outslot");
        auto* outSt = builder->CreateStore(lineHdrInt, outSlotPtr);
        outSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);

        // Advance: next line starts at lineEnd+1 (skip the \n); p2out++; p2i++
        llvm::Value* nextPos = builder->CreateAdd(lineEnd, one, "stl.nextpos", true, true);
        llvm::Value* nextOut = builder->CreateAdd(p2out, one, "stl.nextout", true, true);
        llvm::Value* nextI   = builder->CreateAdd(p2i, one, "stl.nexti", true, true);
        p2i->addIncoming(nextI, builder->GetInsertBlock());
        p2pos->addIncoming(nextPos, builder->GetInsertBlock());
        p2out->addIncoming(nextOut, builder->GetInsertBlock());
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(p2LoopBB)));

        builder->SetInsertPoint(p2DoneBB);
        arrayReturningFunctions_.insert("str_to_lines");
        stringReturningFunctions_.insert("str_to_lines");
        return outBuf;
    }

    // ── End Round-75 builtins ────────────────────────────────────────────────
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
            std::string msg = expr->line > 0 ? std::string("Runtime error: assertion failed at line ") +
                                                   std::to_string(expr->line) + "\n"
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
        // ── Compile-time str_len folding ────────────────────────────
        if (auto strConst = tryFoldStr(expr->arguments[0].get()))
            return llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(strConst->size()));
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // String may be a raw pointer (from a literal/local) or an i64 holding a pointer.
        llvm::Value* strPtr = arg->getType()->isPointerTy()
                                  ? arg
                                  : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "strlen.ptr");
        llvm::Value* result = emitStringLen(strPtr, "strlen.result");
        return result;
    }

    if (bid == BuiltinId::CHAR_AT) {
        validateArgCount(expr, "char_at", 2);

        // ── Compile-time char_at folding ────────────────────────────
        if (auto strConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto idxConst = tryFoldInt(expr->arguments[1].get())) {
                const int64_t idx = *idxConst;
                const int64_t len = static_cast<int64_t>(strConst->size());
                if (idx >= 0 && idx < len) {
                    const char ch = (*strConst)[static_cast<size_t>(idx)];
                    return llvm::ConstantInt::get(getDefaultType(),
                                                  static_cast<uint64_t>(static_cast<unsigned char>(ch)));
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
        llvm::Value* strLen = emitStringLen(strPtr, "charat.strlen");
        // ULT(idx, strLen) ≡ (SGE(idx,0) && SLT(idx,strLen)) when strLen ≥ 0.
        llvm::Value* valid = builder->CreateICmpULT(idxArg, strLen, "charat.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "charat.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "charat.fail", function);
        // char_at OOB is extremely unlikely.
        llvm::MDNode* charAtW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(valid, okBB, failBB, charAtW);

        builder->SetInsertPoint(failBB);
        {
            std::string msg = expr->line > 0 ? std::string("Runtime error: char_at index out of bounds at line ") +
                                                   std::to_string(expr->line) + "\n"
                                             : "Runtime error: char_at index out of bounds\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "charat_oob_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Load char via GEP (use data pointer, not fat-pointer header)
        llvm::Value* strData = emitStringData(strPtr, "charat.data");
        llvm::Value* charPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strData, idxArg, "charat.gep");
        auto* charVal = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "charat.char");
        charVal->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        // Zero-extend to i64; result is always in [0, 256).
        // Use zext nneg (LLVM 18+) to communicate non-negativity; !range is not
        // valid on zext instructions in LLVM 18.
        auto* result = builder->CreateZExt(charVal, getDefaultType(), "charat.ext",
                                           /*IsNonNeg=*/false);
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
        llvm::Value* lhsData = emitStringData(lhsPtr, "streq.lhsdata");
        llvm::Value* rhsData = emitStringData(rhsPtr, "streq.rhsdata");
        llvm::Value* cmpResult = builder->CreateCall(getOrDeclareStrcmp(), {lhsData, rhsData}, "streq.cmp");
        // strcmp returns 0 on equality; convert to boolean (1 if equal, 0 otherwise)
        llvm::Value* isEqual = builder->CreateICmpEQ(cmpResult, builder->getInt32(0), "streq.eq");
        return emitBoolZExt(isEqual, "streq.result");
    }

    if (bid == BuiltinId::STR_CONCAT) {
        validateArgCount(expr, "str_concat", 2);
        llvm::Value* lhsArg = generateExpression(expr->arguments[0].get());
        llvm::Value* rhsArg = generateExpression(expr->arguments[1].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* lhsPtr =
            lhsArg->getType()->isPointerTy() ? lhsArg : builder->CreateIntToPtr(lhsArg, ptrTy, "concat.lhs");
        llvm::Value* rhsPtr =
            rhsArg->getType()->isPointerTy() ? rhsArg : builder->CreateIntToPtr(rhsArg, ptrTy, "concat.rhs");

        llvm::Value* len1 = emitStringLen(lhsPtr, "concat.len1");
        llvm::Value* len2 = emitStringLen(rhsPtr, "concat.len2");
        llvm::Value* totalLen = builder->CreateAdd(len1, len2, "concat.totallen", /*HasNUW=*/true, /*HasNSW=*/true);
        nonNegValues_.insert(totalLen);

        // Allocate a new string header + data: {len, cap, chars...}
        llvm::Value* hdr = emitAllocString(totalLen, totalLen, "concat");

        // Get pointer to the character data area (header + 16 bytes).
        llvm::Value* data = emitStringData(hdr, "concat.data");

        // Copy LHS characters then RHS characters into data area.
        builder->CreateCall(getOrDeclareMemcpy(), {data, emitStringData(lhsPtr, "concat.ldata"), len1});
        llvm::Value* dst2 = builder->CreateInBoundsGEP(builder->getInt8Ty(), data, len1, "concat.dst2");
        builder->CreateCall(getOrDeclareMemcpy(), {dst2, emitStringData(rhsPtr, "concat.rdata"), len2});

        // NUL-terminate.
        llvm::Value* endPtr = builder->CreateInBoundsGEP(builder->getInt8Ty(), data, totalLen, "concat.end");
        builder->CreateStore(builder->getInt8(0), endPtr)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);

        stringReturningFunctions_.insert("str_concat");
        return hdr;
    }

    if (bid == BuiltinId::LOG2) {
        validateArgCount(expr, "log2", 1);
        llvm::Value* n = generateExpression(expr->arguments[0].get());
        n = toDefaultType(n);
        // Integer log2 via CTZ intrinsic: 63 - clz(n).
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        llvm::Value* bits = llvm::ConstantInt::get(getDefaultType(), 63);

        // n <= 0 → return -1
        llvm::Value* isPositive = builder->CreateICmpSGT(n, zero, "log2.pos");

        // clz(n) returns number of leading zeros; log2(n) = 63 - clz(n)
        llvm::Function* ctlzIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctlz, {getDefaultType()});
        // is_zero_poison=true since we guard with isPositive
        llvm::Value* clz = builder->CreateCall(ctlzIntrinsic, {n, builder->getTrue()}, "log2.clz");
        // clz ∈ [0, 63] (is_zero_poison=true + isPositive guard), so
        // 63 - clz ∈ [0, 63]: the subtraction is nuw+nsw.
        llvm::Value* log2val = builder->CreateSub(bits, clz, "log2.val",
                                                  /*HasNUW=*/true, /*HasNSW=*/true);

        return builder->CreateSelect(isPositive, log2val, negOne, "log2.result");
    }

    if (bid == BuiltinId::GCD) {
        validateArgCount(expr, "gcd", 2);
        // Constant-fold gcd(a, b) when both are compile-time constants.
        if (auto va = tryFoldInt(expr->arguments[0].get())) {
            if (auto vb = tryFoldInt(expr->arguments[1].get())) {
                uint64_t a = static_cast<uint64_t>(std::abs(*va));
                uint64_t b = static_cast<uint64_t>(std::abs(*vb));
                while (b) {
                    uint64_t t = b;
                    b = a % b;
                    a = t;
                }
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
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::Function* cttzFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::cttz, {getDefaultType()});

        // Compute k = ctz(a | b) in the entry block (always dominates doneBB).
        llvm::Value* aOrB = builder->CreateOr(a, b, "gcd.aorb");
        llvm::Value* k = builder->CreateCall(cttzFn, {aOrB, builder->getFalse()}, "gcd.k");

        llvm::BasicBlock* mainBB = llvm::BasicBlock::Create(*context, "gcd.main", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "gcd.loop", function);
        llvm::BasicBlock* contBB = llvm::BasicBlock::Create(*context, "gcd.cont", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "gcd.done", function);

        // Edge case: if a == 0 return b, if b == 0 return a.
        // Combined: if (a == 0 || b == 0) return a | b.
        llvm::Value* edgeCase = builder->CreateOr(builder->CreateICmpEQ(a, zero, "gcd.a0"),
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
        // hi = max(phiA, bOdd), lo = min(phiA, bOdd): hi ≥ lo → nuw+nsw.
        llvm::Value* diff = builder->CreateSub(hi, lo, "gcd.diff",
                                               /*HasNUW=*/true, /*HasNSW=*/true);

        // Continue if diff != 0
        llvm::Value* done = builder->CreateICmpEQ(diff, zero, "gcd.dz");
        phiA->addIncoming(lo, loopBB);
        phiB->addIncoming(diff, loopBB);
        builder->CreateCondBr(done, contBB, loopBB);

        // Multiply result by 2^k (common factor)
        builder->SetInsertPoint(contBB);
        // lo is the GCD of the odd-part loop (positive), k = ctz(abs(a)|abs(b)) ≤ 62.
        // Result = lo * 2^k = gcd(a,b) ≤ min(|a|,|b|) ≤ INT64_MAX → nuw+nsw.
        llvm::Value* shifted = builder->CreateShl(lo, k, "gcd.shifted", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateBr(doneBB);

        // Final merge
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "gcd.result");
        result->addIncoming(aOrB, entryBB);   // edge case: a|b = max(a,b) when one is 0
        result->addIncoming(shifted, contBB); // normal case
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
                llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        const bool isFloat = val->getType()->isDoubleTy();
        if (!isFloat)
            val = toDefaultType(val);
        if (isFloat) {
            // Float: use a 32-byte buffer and %g format to preserve decimal places.
            llvm::Value* maxLen = llvm::ConstantInt::get(getDefaultType(), 31);
            llvm::Value* hdr = emitAllocString(maxLen, maxLen, "tostr");
            llvm::Value* bufData = emitStringData(hdr, "tostr.data");
            llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 32);
            llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_float_fmt", true);
            if (!fmtStr)
                fmtStr = builder->CreateGlobalString("%g", "tostr_float_fmt");
            llvm::Value* written =
                builder->CreateCall(getOrDeclareSnprintf(), {bufData, bufSize, fmtStr, val}, "tostr.written");
            llvm::Value* actualLen = builder->CreateZExt(written, getDefaultType(), "tostr.len", /*IsNonNeg=*/false);
            nonNegValues_.insert(actualLen);
            emitStoreStringLen(actualLen, hdr);
            stringReturningFunctions_.insert("to_string");
            return hdr;
        }
        // Integer: 21 bytes is enough for any 64-bit signed decimal plus null terminator.
        llvm::Value* maxLen = llvm::ConstantInt::get(getDefaultType(), 20);
        llvm::Value* hdr = emitAllocString(maxLen, maxLen, "tostr");
        llvm::Value* bufData = emitStringData(hdr, "tostr.data");
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 21);
        llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_fmt", true);
        if (!fmtStr) {
            fmtStr = builder->CreateGlobalString("%lld", "tostr_fmt");
        }
        llvm::Value* written =
            builder->CreateCall(getOrDeclareSnprintf(), {bufData, bufSize, fmtStr, val}, "tostr.written");
        llvm::Value* actualLen = builder->CreateZExt(written, getDefaultType(), "tostr.len", /*IsNonNeg=*/false);
        nonNegValues_.insert(actualLen);
        emitStoreStringLen(actualLen, hdr);
        stringReturningFunctions_.insert("to_string");
        return hdr;
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
        llvm::Value* strLen = emitStringLen(strPtr, "strfind.len");
        // memchr(strPtr, ch, strLen)
        llvm::Value* chTrunc = builder->CreateTrunc(chArg, llvm::Type::getInt32Ty(*context), "strfind.ch32");
        llvm::Value* strfindData = emitStringData(strPtr, "strfind.data");
        llvm::Value* found = builder->CreateCall(getOrDeclareMemchr(), {strfindData, chTrunc, strLen}, "strfind.found");
        // If memchr returns null, return -1; otherwise return (found - strfindData)
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNull = builder->CreateICmpEQ(found, nullPtr, "strfind.isnull");
        llvm::Value* offset =
            builder->CreatePtrDiff(llvm::Type::getInt8Ty(*context), found, strfindData, "strfind.offset");
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        return builder->CreateSelect(isNull, negOne, offset, "strfind.result");
    }

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
        if (inputIsDouble)
            return result;
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

    if (bid == BuiltinId::TO_INT) {
        validateArgCount(expr, "to_int", 1);

        // ── Compile-time to_int folding ─────────────────────────────
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
            llvm::Value* strPtr =
                arg->getType()->isPointerTy() ? arg : builder->CreateIntToPtr(arg, ptrTy, "toint.strptr");
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            auto* base10 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10);
            return builder->CreateCall(getOrDeclareStrtoll(), {emitStringData(strPtr, "toint.data"), nullPtr, base10},
                                       "toint.parsed");
        }
        return toDefaultType(arg);
    }

    if (bid == BuiltinId::TO_FLOAT) {
        validateArgCount(expr, "to_float", 1);

        // ── Compile-time to_float folding ───────────────────────────
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
            llvm::Value* strPtr =
                arg->getType()->isPointerTy() ? arg : builder->CreateIntToPtr(arg, ptrTy, "tofloat.strptr");
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            return builder->CreateCall(getOrDeclareStrtod(), {emitStringData(strPtr, "tofloat.data"), nullPtr},
                                       "tofloat.parsed");
        }
        return ensureFloat(arg);
    }

    // -----------------------------------------------------------------------

    if (bid == BuiltinId::STR_SUBSTR) {
        validateArgCount(expr, "str_substr", 3);

        // ── Compile-time str_substr folding ─────────────────────────
        if (auto strConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto startConst = tryFoldInt(expr->arguments[1].get())) {
                if (auto lenConst = tryFoldInt(expr->arguments[2].get())) {
                    const auto& s = *strConst;
                    int64_t slen = static_cast<int64_t>(s.size());
                    int64_t startVal = *startConst, lenVal = *lenConst;
                    if (startVal < 0)
                        startVal = 0;
                    if (startVal > slen)
                        startVal = slen;
                    int64_t maxLen = slen - startVal;
                    if (lenVal < 0)
                        lenVal = 0;
                    if (lenVal > maxLen)
                        lenVal = maxLen;
                    std::string result = s.substr(static_cast<size_t>(startVal), static_cast<size_t>(lenVal));
                    llvm::GlobalVariable* gv = internString(result);
                    return llvm::ConstantExpr::getInBoundsGetElementPtr(
                        gv->getValueType(), gv,
                        llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
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
        llvm::Value* strLen = emitStringLen(strPtr, "substr.strlen");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        // Clamp start: max(0, min(start, strLen))
        llvm::Value* startNeg = builder->CreateICmpSLT(startArg, zero, "substr.startneg");
        startArg = builder->CreateSelect(startNeg, zero, startArg, "substr.startclamp");
        nonNegValues_.insert(startArg);
        llvm::Value* startOverflow = builder->CreateICmpSGT(startArg, strLen, "substr.startover");
        startArg = builder->CreateSelect(startOverflow, strLen, startArg, "substr.startfinal");
        nonNegValues_.insert(startArg);
        // Clamp length: max(0, min(len, strLen - start))
        // After clamping: startArg ∈ [0, strLen], so remaining is nuw+nsw.
        llvm::Value* remaining = builder->CreateSub(strLen, startArg, "substr.remaining",
                                                    /*HasNUW=*/true, /*HasNSW=*/true);
        nonNegValues_.insert(remaining);
        llvm::Value* lenNeg = builder->CreateICmpSLT(lenArg, zero, "substr.lenneg");
        lenArg = builder->CreateSelect(lenNeg, zero, lenArg, "substr.lenclamp");
        nonNegValues_.insert(lenArg);
        llvm::Value* lenOverflow = builder->CreateICmpSGT(lenArg, remaining, "substr.lenover");
        lenArg = builder->CreateSelect(lenOverflow, remaining, lenArg, "substr.lenfinal");
        nonNegValues_.insert(lenArg);

        llvm::Value* hdr = emitAllocString(lenArg, lenArg, "substr");
        llvm::Value* substrData = emitStringData(strPtr, "substr.data");
        llvm::Value* srcPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), substrData, startArg, "substr.src");
        llvm::Value* dstData = emitStringData(hdr, "substr.dst");
        builder->CreateCall(getOrDeclareMemcpy(), {dstData, srcPtr, lenArg});
        // Null-terminate: dstData[len] = 0
        llvm::Value* endPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), dstData, lenArg, "substr.end");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), endPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        stringReturningFunctions_.insert("str_substr");
        return hdr;
    }

    if (bid == BuiltinId::STR_UPPER) {
        validateArgCount(expr, "str_upper", 1);
        // NOTE: Cannot fold str_upper to an interned global — callers may
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "upper.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "upper.len");
        llvm::Value* hdr = emitAllocString(strLen, strLen, "upper");
        llvm::Value* upperData = emitStringData(hdr, "upper.data");
        builder->CreateCall(getOrDeclareStrcpy(), {upperData, emitStringData(strPtr, "upper.srcdata")});
        // Loop: for i = 0; i < strLen; i++ { upperData[i] = toupper(upperData[i]); }
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* upperBuf = upperData;
        emitCountingLoop("upper", strLen, zero, 4, [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
            llvm::Value* charPtr =
                builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), upperBuf, idx, "upper.charptr");
            auto* chLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "upper.ch");
            chLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* ch32 = builder->CreateZExt(chLoad, llvm::Type::getInt32Ty(*context), "upper.ch32",
                                                    /*IsNonNeg=*/false);
            nonNegValues_.insert(ch32);
            llvm::Value* upper = builder->CreateCall(getOrDeclareToupper(), {ch32}, "upper.toupper");
            llvm::Value* upper8 = builder->CreateTrunc(upper, llvm::Type::getInt8Ty(*context), "upper.trunc");
            auto* upperStore = builder->CreateStore(upper8, charPtr);
            upperStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* nextIdx = builder->CreateAdd(idx, one, "upper.next", /*HasNUW=*/true, /*HasNSW=*/true);
            idx->addIncoming(nextIdx, builder->GetInsertBlock());
            attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        });
        stringReturningFunctions_.insert("str_upper");
        return hdr;
    }

    if (bid == BuiltinId::STR_LOWER) {
        validateArgCount(expr, "str_lower", 1);
        // NOTE: Cannot fold str_lower to an interned global — callers may
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "lower.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "lower.len");
        llvm::Value* hdr = emitAllocString(strLen, strLen, "lower");
        llvm::Value* lowerData = emitStringData(hdr, "lower.data");
        builder->CreateCall(getOrDeclareStrcpy(), {lowerData, emitStringData(strPtr, "lower.srcdata")});
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* lowerBuf = lowerData;
        emitCountingLoop("lower", strLen, zero, 4, [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
            llvm::Value* charPtr =
                builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), lowerBuf, idx, "lower.charptr");
            auto* chLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "lower.ch");
            chLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* ch32 = builder->CreateZExt(chLoad, llvm::Type::getInt32Ty(*context), "lower.ch32",
                                                    /*IsNonNeg=*/false);
            nonNegValues_.insert(ch32);
            llvm::Value* lower = builder->CreateCall(getOrDeclareTolower(), {ch32}, "lower.tolower");
            llvm::Value* lower8 = builder->CreateTrunc(lower, llvm::Type::getInt8Ty(*context), "lower.trunc");
            auto* lowerStore = builder->CreateStore(lower8, charPtr);
            lowerStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* nextIdx = builder->CreateAdd(idx, one, "lower.next", /*HasNUW=*/true, /*HasNSW=*/true);
            idx->addIncoming(nextIdx, builder->GetInsertBlock());
            attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        });
        stringReturningFunctions_.insert("str_lower");
        return hdr;
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
            llvm::Value* len = emitStringLen(haystackPtr, "contains.len");
            // Cast char to i32 for memchr.  Use unsigned char intermediate to
            auto unsignedCharValue = static_cast<uint32_t>(static_cast<unsigned char>(singleCharVal));
            llvm::Value* charVal = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), unsignedCharValue);
            result = builder->CreateCall(
                getOrDeclareMemchr(), {emitStringData(haystackPtr, "contains.hdata"), charVal, len}, "contains.memchr");
        } else {
            llvm::Value* needleArg = generateExpression(needleExpr);
            llvm::Value* needlePtr =
                needleArg->getType()->isPointerTy()
                    ? needleArg
                    : builder->CreateIntToPtr(needleArg, llvm::PointerType::getUnqual(*context), "contains.needle");
            result = builder->CreateCall(
                getOrDeclareStrstr(),
                {emitStringData(haystackPtr, "contains.hdata2"), emitStringData(needlePtr, "contains.ndata")},
                "contains.strstr");
        }
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNotNull = builder->CreateICmpNE(result, nullPtr, "contains.notnull");
        return emitBoolZExt(isNotNull, "contains.result");
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
        llvm::Value* indexofHData = emitStringData(haystackPtr, "indexof.hdata");
        llvm::Value* result;
        if (isSingleChar) {
            llvm::Value* len = emitStringLen(haystackPtr, "indexof.len");
            // Cast char to i32 for memchr (see str_contains above for rationale).
            auto unsignedCharValue = static_cast<uint32_t>(static_cast<unsigned char>(singleCharVal));
            llvm::Value* charVal = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), unsignedCharValue);
            result = builder->CreateCall(getOrDeclareMemchr(), {indexofHData, charVal, len}, "indexof.memchr");
        } else {
            llvm::Value* needleArg = generateExpression(needleExpr);
            llvm::Value* needlePtr =
                needleArg->getType()->isPointerTy()
                    ? needleArg
                    : builder->CreateIntToPtr(needleArg, llvm::PointerType::getUnqual(*context), "indexof.needle");
            result = builder->CreateCall(getOrDeclareStrstr(),
                                         {indexofHData, emitStringData(needlePtr, "indexof.ndata")}, "indexof.strstr");
        }
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNull = builder->CreateICmpEQ(result, nullPtr, "indexof.isnull");
        llvm::Value* offset =
            builder->CreatePtrDiff(llvm::Type::getInt8Ty(*context), result, indexofHData, "indexof.offset");
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
            strArg->getType()->isPointerTy() ? strArg : builder->CreateIntToPtr(strArg, ptrTy, "replace.str");
        llvm::Value* oldPtr =
            oldArg->getType()->isPointerTy() ? oldArg : builder->CreateIntToPtr(oldArg, ptrTy, "replace.old");
        llvm::Value* newPtr =
            newArg->getType()->isPointerTy() ? newArg : builder->CreateIntToPtr(newArg, ptrTy, "replace.new");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* i8zero = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0);

        llvm::Value* oldLen = emitStringLen(oldPtr, "replace.oldlen");
        llvm::Value* newLen = emitStringLen(newPtr, "replace.newlen");
        llvm::Value* strLen = emitStringLen(strPtr, "replace.strlen");
        llvm::Value* replaceStrData = emitStringData(strPtr, "replace.strdata");
        llvm::Value* replaceOldData = emitStringData(oldPtr, "replace.olddata");
        llvm::Value* replaceNewData = emitStringData(newPtr, "replace.newdata");

        // If old is the empty string, just return a copy of str to avoid
        // an infinite loop (strstr("x","") always succeeds).
        llvm::BasicBlock* emptyOldBB = llvm::BasicBlock::Create(*context, "replace.emptyold", function);
        llvm::BasicBlock* replaceMainBB = llvm::BasicBlock::Create(*context, "replace.main", function);
        llvm::Value* oldIsEmpty = builder->CreateICmpEQ(oldLen, zero, "replace.oldempty");
        // Empty-old is a degenerate edge case — heavily favour normal path.
        auto* eoW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(oldIsEmpty, emptyOldBB, replaceMainBB, eoW);

        // Empty old: return copy of str
        builder->SetInsertPoint(emptyOldBB);
        llvm::Value* copyHdr0 = emitAllocString(strLen, strLen, "replace.copy0");
        llvm::Value* copyData0 = emitStringData(copyHdr0, "replace.copydata0");
        llvm::Value* copySize0 = builder->CreateAdd(strLen, one, "replace.copysize0", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemcpy(), {copyData0, replaceStrData, copySize0});

        llvm::BasicBlock* emptyOldExitBB = builder->GetInsertBlock();
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "replace.merge", function);
        builder->CreateBr(mergeBB);

        // ---------------------------------------------------------------
        builder->SetInsertPoint(replaceMainBB);
        llvm::BasicBlock* countLoopBB = llvm::BasicBlock::Create(*context, "replace.countloop", function);
        llvm::BasicBlock* countBodyBB = llvm::BasicBlock::Create(*context, "replace.countbody", function);
        llvm::BasicBlock* countDoneBB = llvm::BasicBlock::Create(*context, "replace.countdone", function);
        builder->CreateBr(countLoopBB);

        // Loop header: PHIs for cursor position and running count.
        builder->SetInsertPoint(countLoopBB);
        llvm::PHINode* cCursor = builder->CreatePHI(ptrTy, 2, "replace.ccursor");
        cCursor->addIncoming(replaceStrData, replaceMainBB);
        llvm::PHINode* cCount = builder->CreatePHI(getDefaultType(), 2, "replace.ccount");
        cCount->addIncoming(zero, replaceMainBB);

        // Single strstr call per iteration.
        llvm::Value* cFound = builder->CreateCall(getOrDeclareStrstr(), {cCursor, replaceOldData}, "replace.cfound");
        llvm::Value* cIsNull = builder->CreateICmpEQ(cFound, nullPtr, "replace.cisnull");
        builder->CreateCondBr(cIsNull, countDoneBB, countBodyBB);

        // Body: increment count, advance cursor past the matched occurrence.
        builder->SetInsertPoint(countBodyBB);
        llvm::Value* newCount = builder->CreateAdd(cCount, one, "replace.newcount", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextCursor =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), cFound, oldLen, "replace.nextcursor");
        cCursor->addIncoming(nextCursor, countBodyBB);
        cCount->addIncoming(newCount, countBodyBB);
        builder->CreateBr(countLoopBB);

        // Done: cCount holds the total number of occurrences.
        builder->SetInsertPoint(countDoneBB);
        // cCount is the final count; use it directly (single predecessor from countLoopBB).
        llvm::Value* totalCount = cCount;

        // ---------------------------------------------------------------
        llvm::Value* lenDiff = builder->CreateSub(newLen, oldLen, "replace.lendiff", /*HasNUW=*/false, /*HasNSW=*/true);
        llvm::Value* extraLen =
            builder->CreateMul(totalCount, lenDiff, "replace.extralen", /*HasNUW=*/false, /*HasNSW=*/true);
        llvm::Value* resultLen =
            builder->CreateAdd(strLen, extraLen, "replace.resultlen", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* resultHdr = emitAllocString(resultLen, resultLen, "replace.result");
        llvm::Value* resultBufData = emitStringData(resultHdr, "replace.resultdata");

        // ---------------------------------------------------------------
        llvm::BasicBlock* buildLoopBB = llvm::BasicBlock::Create(*context, "replace.buildloop", function);
        llvm::BasicBlock* buildBodyBB = llvm::BasicBlock::Create(*context, "replace.buildbody", function);
        llvm::BasicBlock* buildDoneBB = llvm::BasicBlock::Create(*context, "replace.builddone", function);
        builder->CreateBr(buildLoopBB);

        builder->SetInsertPoint(buildLoopBB);
        llvm::PHINode* bSrc = builder->CreatePHI(ptrTy, 2, "replace.bsrc");
        bSrc->addIncoming(replaceStrData, countDoneBB);
        llvm::PHINode* bDst = builder->CreatePHI(ptrTy, 2, "replace.bdst");
        bDst->addIncoming(resultBufData, countDoneBB);

        llvm::Value* bFound = builder->CreateCall(getOrDeclareStrstr(), {bSrc, replaceOldData}, "replace.bfound");
        llvm::Value* bIsNull = builder->CreateICmpEQ(bFound, nullPtr, "replace.bnull");
        builder->CreateCondBr(bIsNull, buildDoneBB, buildBodyBB);

        // Body: copy prefix, then replacement, advance
        builder->SetInsertPoint(buildBodyBB);
        llvm::Value* prefLen = builder->CreatePtrDiff(llvm::Type::getInt8Ty(*context), bFound, bSrc, "replace.preflen");
        builder->CreateCall(getOrDeclareMemcpy(), {bDst, bSrc, prefLen});
        llvm::Value* dstAfterPref =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), bDst, prefLen, "replace.dstpref");
        builder->CreateCall(getOrDeclareMemcpy(), {dstAfterPref, replaceNewData, newLen});
        llvm::Value* dstAfterNew =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), dstAfterPref, newLen, "replace.dstnew");
        llvm::Value* srcAfterOld =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), bFound, oldLen, "replace.srcold");
        bSrc->addIncoming(srcAfterOld, buildBodyBB);
        bDst->addIncoming(dstAfterNew, buildBodyBB);
        builder->CreateBr(buildLoopBB);

        // Done: copy remaining tail and null-terminate.
        // bSrc and bDst are live values from the loop header (single predecessor).
        builder->SetInsertPoint(buildDoneBB);
        // Copy remaining chars: tail = strLen - (bSrc - strPtr)
        llvm::Value* consumed =
            builder->CreatePtrDiff(llvm::Type::getInt8Ty(*context), bSrc, replaceStrData, "replace.consumed");
        // consumed ≤ strLen (bSrc never exceeds strPtr+strLen), so sub is nuw+nsw.
        llvm::Value* tail = builder->CreateSub(strLen, consumed, "replace.tail",
                                               /*HasNUW=*/true, /*HasNSW=*/true);
        nonNegValues_.insert(tail);
        builder->CreateCall(getOrDeclareMemcpy(), {bDst, bSrc, tail});
        llvm::Value* endPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), bDst, tail, "replace.end");
        builder->CreateStore(i8zero, endPtr);

        llvm::BasicBlock* buildExitBB = builder->GetInsertBlock();
        builder->CreateBr(mergeBB);

        // ---------------------------------------------------------------
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* resultPhi = builder->CreatePHI(ptrTy, 2, "replace.result");
        resultPhi->addIncoming(copyHdr0, emptyOldExitBB);
        resultPhi->addIncoming(resultHdr, buildExitBB);
        stringReturningFunctions_.insert("str_replace");
        return resultPhi;
    }

    if (bid == BuiltinId::STR_TRIM) {
        validateArgCount(expr, "str_trim", 1);
        // NOTE: Cannot fold str_trim to an interned global — callers may
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "trim.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "trim.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Compute the character data pointer once, before any loops, so it
        // dominates all uses in startBodyBB, endBodyBB, and the final block.
        llvm::Value* trimStrData = emitStringData(strPtr, "trim.data");

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
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), trimStrData, startIdx, "trim.startcharptr");
        auto* startCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), startCharPtr, "trim.startchar");
        startCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* startChar32 =
            builder->CreateZExt(startCharLoad, llvm::Type::getInt32Ty(*context), "trim.startchar32",
                                /*IsNonNeg=*/false);
        nonNegValues_.insert(startChar32);
        llvm::Value* isStartSpace = builder->CreateCall(getOrDeclareIsspace(), {startChar32}, "trim.isspace");
        llvm::Value* isStartSpaceBool = builder->CreateICmpNE(isStartSpace, builder->getInt32(0), "trim.isspacebool");
        llvm::Value* nextStartIdx =
            builder->CreateAdd(startIdx, one, "trim.nextstartidx", /*HasNUW=*/true, /*HasNSW=*/true);
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
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), trimStrData, prevEndIdx, "trim.endcharptr");
        auto* endCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), endCharPtr, "trim.endchar");
        endCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* endChar32 = builder->CreateZExt(endCharLoad, llvm::Type::getInt32Ty(*context), "trim.endchar32",
                                                     /*IsNonNeg=*/false);
        nonNegValues_.insert(endChar32);
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
        // trimStart ≤ trimEnd (scan invariant), so sub is nuw+nsw.
        llvm::Value* trimLen = builder->CreateSub(trimEnd, trimStart, "trim.len2",
                                                  /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* trimHdr = emitAllocString(trimLen, trimLen, "trim");
        llvm::Value* trimDst = emitStringData(trimHdr, "trim.dst");
        llvm::Value* trimSrc =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), trimStrData, trimStart, "trim.src");
        builder->CreateCall(getOrDeclareMemcpy(), {trimDst, trimSrc, trimLen});
        llvm::Value* trimEndPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), trimDst, trimLen, "trim.endptr");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), trimEndPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        stringReturningFunctions_.insert("str_trim");
        return trimHdr;
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
        llvm::Value* prefixLen;
        if (auto prefixLit = tryFoldStr(expr->arguments[1].get())) {
            // Constant-fold strlen(prefix) — no runtime call needed.
            prefixLen = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(prefixLit->size()));
        } else {
            prefixLen = emitStringLen(prefixPtr, "startswith.plen");
        }
        llvm::Value* cmpResult = builder->CreateCall(
            getOrDeclareStrncmp(),
            {emitStringData(strPtr, "startswith.sdata"), emitStringData(prefixPtr, "startswith.pdata"), prefixLen},
            "startswith.cmp");
        llvm::Value* isEqual = builder->CreateICmpEQ(cmpResult, builder->getInt32(0), "startswith.eq");
        return emitBoolZExt(isEqual, "startswith.result");
    }

    if (bid == BuiltinId::STR_ENDS_WITH) {
        validateArgCount(expr, "str_ends_with", 2);

        // ── Compile-time str_ends_with folding ──────────────────────
        // When both arguments are compile-time string constants, fold at compile time.
        if (auto strConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto suffixConst = tryFoldStr(expr->arguments[1].get())) {
                const auto& s = *strConst;
                const auto& suffix = *suffixConst;
                bool result =
                    s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
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
        llvm::Value* strLen = emitStringLen(strPtr, "endswith.strlen");
        // Optimization: when the suffix is a compile-time string literal,
        // constant-fold strlen(suffix) to avoid the runtime call.
        llvm::Value* sufLen;
        if (auto suffixLit = tryFoldStr(expr->arguments[1].get())) {
            sufLen = llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(suffixLit->size()));
        } else {
            sufLen = emitStringLen(suffixPtr, "endswith.suflen");
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
        // In checkBB: sufLen ≤ strLen (proven by tooLong guard), so sub is nuw+nsw.
        llvm::Value* offset = builder->CreateSub(strLen, sufLen, "endswith.offset",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* endswithStrData = emitStringData(strPtr, "endswith.sdata");
        llvm::Value* tailPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), endswithStrData, offset, "endswith.tail");
        llvm::Value* cmpResult = builder->CreateCall(
            getOrDeclareMemcmp(), {tailPtr, emitStringData(suffixPtr, "endswith.sfdata"), sufLen}, "endswith.cmp");
        llvm::Value* isEqual = builder->CreateICmpEQ(cmpResult, builder->getInt32(0), "endswith.eq");
        llvm::Value* resultCheck = emitBoolZExt(isEqual, "endswith.result");
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
        if (auto strConst = tryFoldStr(expr->arguments[0].get())) {
            if (auto countConst = tryFoldInt(expr->arguments[1].get())) {
                int64_t count = *countConst;
                // Only fold for small results (≤ 256 bytes) to avoid
                // bloating the data section.
                if (count >= 0 && count * static_cast<int64_t>(strConst->size()) <= 256) {
                    std::string result;
                    result.reserve(static_cast<size_t>(count) * strConst->size());
                    for (int64_t i = 0; i < count; ++i)
                        result += *strConst;
                    llvm::GlobalVariable* gv = internString(result);
                    return llvm::ConstantExpr::getInBoundsGetElementPtr(
                        gv->getValueType(), gv,
                        llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
                }
            }
        }

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* countArg = generateExpression(expr->arguments[1].get());
        countArg = toDefaultType(countArg);
        // Clamp negative counts to 0 to prevent integer overflow in the
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isNeg = builder->CreateICmpSLT(countArg, zero, "repeat.isneg");
        countArg = builder->CreateSelect(isNeg, zero, countArg, "repeat.clamp");
        nonNegValues_.insert(countArg);
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "repeat.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "repeat.len");
        llvm::Value* totalLen = builder->CreateMul(strLen, countArg, "repeat.total", /*HasNUW=*/true, /*HasNSW=*/true);
        nonNegValues_.insert(totalLen);
        llvm::Value* hdr = emitAllocString(totalLen, totalLen, "repeat");
        llvm::Value* buf = emitStringData(hdr, "repeat.data");
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
        builder->CreateCall(getOrDeclareMemcpy(), {dst, emitStringData(strPtr, "repeat.data"), strLen});
        llvm::Value* nextOffset =
            builder->CreateAdd(offset, strLen, "repeat.nextoff", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "repeat.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        offset->addIncoming(nextOffset, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        builder->SetInsertPoint(doneBB);
        // Null-terminate: buf[totalLen] = '\0'
        llvm::Value* endPtr = builder->CreateInBoundsGEP(builder->getInt8Ty(), buf, totalLen, "repeat.end");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), endPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        stringReturningFunctions_.insert("str_repeat");
        return hdr;
    }

    if (bid == BuiltinId::STR_REVERSE) {
        validateArgCount(expr, "str_reverse", 1);

        // NOTE: Cannot fold str_reverse to an interned global — callers may

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strrev.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "strrev.len");
        llvm::Value* hdr = emitAllocString(strLen, strLen, "strrev");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* strrevBuf = emitStringData(hdr, "strrev.data");
        llvm::Value* buf = strrevBuf;
        llvm::Value* srStrLen = strLen;
        llvm::Value* srStrPtr = emitStringData(strPtr, "strrev.sdata");
        emitCountingLoop("strrev", strLen, zero, 4, [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
            llvm::Value* revIdx =
                builder->CreateSub(builder->CreateSub(srStrLen, one, "strrev.lenm1", /*HasNUW=*/true, /*HasNSW=*/true),
                                   idx, "strrev.revidx", /*HasNUW=*/true, /*HasNSW=*/true);
            llvm::Value* srcPtr2 =
                builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), srStrPtr, revIdx, "strrev.srcptr");
            auto* revLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), srcPtr2, "strrev.ch");
            revLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* dstPtr =
                builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), strrevBuf, idx, "strrev.dstptr");
            auto* revStore = builder->CreateStore(revLoad, dstPtr);
            revStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            llvm::Value* nextIdx = builder->CreateAdd(idx, one, "strrev.next", /*HasNUW=*/true, /*HasNSW=*/true);
            idx->addIncoming(nextIdx, builder->GetInsertBlock());
            attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        });
        // Null-terminate
        llvm::Value* endPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, strLen, "strrev.endptr");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), endPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        stringReturningFunctions_.insert("str_reverse");
        return hdr;
    }

    // -----------------------------------------------------------------------

    if (bid == BuiltinId::PUSH) {
        validateArgCount(expr, "push", 2);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        // Array layout: [length, elem0, elem1, ...]
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* pushLenLoad = emitLoadArrayLen(arrPtr, "push.oldlen");
        llvm::Value* oldLen = pushLenLoad;
        llvm::Value* newLen = builder->CreateAdd(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "push.newlen",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
        // oldLen ≥ 0 (from emitLoadArrayLen range metadata), so newLen = oldLen+1 ≥ 1.
        nonNegValues_.insert(newLen);
        // Only call realloc when the new slot count crosses a power-of-2
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
        llvm::Value* atBoundary =
            builder->CreateAnd(isPow2, builder->CreateNot(belowMin, "push.abovemin"), "push.atbound");
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
        llvm::Value* slotsM1 = builder->CreateSub(slots, one64, "push.pm1", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Function* ctlzFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctlz, {getDefaultType()});
        // is_zero_poison=true: slots is always >= 2 here (newLen >= 1), so
        // slotsM1 is always >= 1, never zero.
        llvm::Value* lz = builder->CreateCall(ctlzFn, {slotsM1, llvm::ConstantInt::getTrue(*context)}, "push.lz");
        llvm::Value* shift = builder->CreateSub(llvm::ConstantInt::get(getDefaultType(), 64), lz, "push.shift",
                                                /*HasNUW=*/false, /*HasNSW=*/true);
        llvm::Value* cap = builder->CreateShl(one64, shift, "push.cap", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* useMin = builder->CreateICmpSLT(cap, minSlots, "push.usemin");
        cap = builder->CreateSelect(useMin, minSlots, cap, "push.finalcap");
        llvm::Value* newSize = builder->CreateMul(cap, llvm::ConstantInt::get(getDefaultType(), 8), "push.bytes",
                                                  /*HasNUW=*/true, /*HasNSW=*/true);
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
        llvm::Value* newElemIdx = builder->CreateAdd(oldLen, llvm::ConstantInt::get(getDefaultType(), 1),
                                                     "push.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* newElemPtr = builder->CreateInBoundsGEP(getDefaultType(), newBuf, newElemIdx, "push.elemptr");
        emitStoreArrayElem(valArg, newElemPtr);
        // Return new array pointer as i64
        return newBuf;
    }

    if (bid == BuiltinId::POP) {
        validateArgCount(expr, "pop", 1);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = getArrayPtr(arrArg);
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
            std::string msg = expr->line > 0 ? std::string("Runtime error: pop from empty array at line ") +
                                                   std::to_string(expr->line) + "\n"
                                             : "Runtime error: pop from empty array\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "pop_empty_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Return the last element
        llvm::Value* lastIdx = builder->CreateAdd(
            builder->CreateSub(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "pop.lastoff", /*HasNUW=*/true,
                               /*HasNSW=*/true),
            llvm::ConstantInt::get(getDefaultType(), 1), "pop.lastidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* lastPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, lastIdx, "pop.lastptr");
        llvm::Value* lastVal = emitLoadArrayElem(lastPtr, "pop.lastval");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(lastVal)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                                llvm::MDNode::get(*context, {}));
        // Decrease length in-place
        llvm::Value* newLen = builder->CreateSub(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "pop.newlen",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
        // Guard above ensures oldLen ≥ 1, so newLen = oldLen-1 ≥ 0.
        nonNegValues_.insert(newLen);
        auto* popLenSt = builder->CreateAlignedStore(newLen, arrPtr, llvm::MaybeAlign(8));
        popLenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        return lastVal;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::SHIFT) {
        validateArgCount(expr, "shift", 1);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* oldLen = emitLoadArrayLen(arrPtr, "shift.oldlen");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* isEmpty = builder->CreateICmpSLE(oldLen, zero, "shift.empty");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "shift.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "shift.fail", function);
        // Shifting an empty array is exceptional.
        llvm::MDNode* shW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(isEmpty, failBB, okBB, shW);

        builder->SetInsertPoint(failBB);
        {
            std::string msg = expr->line > 0 ? std::string("Runtime error: shift from empty array at line ") +
                                                   std::to_string(expr->line) + "\n"
                                             : "Runtime error: shift from empty array\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "shift_empty_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Save the first element (arr[1]).
        llvm::Value* firstPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, one, "shift.firstptr");
        llvm::Value* firstVal = emitLoadArrayElem(firstPtr, "shift.firstval");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(firstVal)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                                 llvm::MDNode::get(*context, {}));

        // Move arr[2..oldLen+1) → arr[1..oldLen).  Source and dest overlap, so use memmove.
        // Byte count = (oldLen - 1) * 8.
        llvm::Value* newLen = builder->CreateSub(oldLen, one, "shift.newlen", /*HasNUW=*/true, /*HasNSW=*/true);
        // We are in okBB (oldLen ≥ 1), so newLen = oldLen-1 ≥ 0. HasNUW is safe.
        nonNegValues_.insert(newLen);
        llvm::Value* moveBytes = builder->CreateMul(newLen, eight, "shift.movebytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* dst = firstPtr; // arr + 1
        llvm::Value* src = builder->CreateInBoundsGEP(getDefaultType(), arrPtr,
                                                      llvm::ConstantInt::get(getDefaultType(), 2), "shift.src");
        builder->CreateCall(getOrDeclareMemmove(), {dst, src, moveBytes});

        // Decrease length in-place.
        auto* lenSt = builder->CreateAlignedStore(newLen, arrPtr, llvm::MaybeAlign(8));
        lenSt->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        return firstVal;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::UNSHIFT) {
        validateArgCount(expr, "unshift", 2);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* oldLen = emitLoadArrayLen(arrPtr, "ush.oldlen");
        llvm::Value* one64 = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* zero64 = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* newLen = builder->CreateAdd(oldLen, one64, "ush.newlen", /*HasNUW=*/true, /*HasNSW=*/true);
        // oldLen ≥ 0 (range metadata), so newLen = oldLen+1 ≥ 1.
        nonNegValues_.insert(newLen);

        // Same growth policy as push: realloc only at power-of-2 boundary or
        // first time we cross the minimum capacity (16 slots).
        llvm::Value* oldSlots = builder->CreateAdd(oldLen, one64, "ush.oldslots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* minSlots = llvm::ConstantInt::get(getDefaultType(), 16);
        llvm::Value* oldSlotsM1 = builder->CreateSub(oldSlots, one64, "ush.osm1", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* andCheck = builder->CreateAnd(oldSlots, oldSlotsM1, "ush.ispow2");
        llvm::Value* isPow2 = builder->CreateICmpEQ(andCheck, zero64, "ush.ispow2cmp");
        llvm::Value* belowMin = builder->CreateICmpSLT(oldSlots, minSlots, "ush.belowmin");
        llvm::Value* atBoundary =
            builder->CreateAnd(isPow2, builder->CreateNot(belowMin, "ush.abovemin"), "ush.atbound");
        llvm::Value* needsGrow = builder->CreateOr(belowMin, atBoundary, "ush.needsgrow");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* growBB = llvm::BasicBlock::Create(*context, "ush.grow", function);
        llvm::BasicBlock* nogrowBB = llvm::BasicBlock::Create(*context, "ush.nogrow", function);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "ush.merge", function);
        llvm::MDNode* growW = llvm::MDBuilder(*context).createBranchWeights(1, 99);
        builder->CreateCondBr(needsGrow, growBB, nogrowBB, growW);

        // Grow path: nextPow2(newSlots) via ctlz.
        builder->SetInsertPoint(growBB);
        llvm::Value* slots = builder->CreateAdd(newLen, one64, "ush.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slotsM1 = builder->CreateSub(slots, one64, "ush.pm1", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Function* ctlzFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ctlz, {getDefaultType()});
        llvm::Value* lz = builder->CreateCall(ctlzFn, {slotsM1, llvm::ConstantInt::getTrue(*context)}, "ush.lz");
        llvm::Value* shiftAmt = builder->CreateSub(llvm::ConstantInt::get(getDefaultType(), 64), lz, "ush.shift",
                                                   /*HasNUW=*/false, /*HasNSW=*/true);
        llvm::Value* cap = builder->CreateShl(one64, shiftAmt, "ush.cap", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* useMin = builder->CreateICmpSLT(cap, minSlots, "ush.usemin");
        cap = builder->CreateSelect(useMin, minSlots, cap, "ush.finalcap");
        llvm::Value* newSize = builder->CreateMul(cap, eight, "ush.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* grownBuf = builder->CreateCall(getOrDeclareRealloc(), {arrPtr, newSize}, "ush.newbuf");
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(nogrowBB);
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* newBuf = builder->CreatePHI(llvm::PointerType::getUnqual(*context), 2, "ush.buf");
        newBuf->addIncoming(grownBuf, growBB);
        newBuf->addIncoming(arrPtr, nogrowBB);


        // memmove arr[1..oldLen+1) → arr[2..newLen+1) — overlapping, dest > src.
        llvm::Value* moveBytes = builder->CreateMul(oldLen, eight, "ush.movebytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* src = builder->CreateInBoundsGEP(getDefaultType(), newBuf, one64, "ush.src");
        llvm::Value* dst = builder->CreateInBoundsGEP(getDefaultType(), newBuf,
                                                      llvm::ConstantInt::get(getDefaultType(), 2), "ush.dst");
        builder->CreateCall(getOrDeclareMemmove(), {dst, src, moveBytes});

        // Store the new value at index 1 (the first slot after the header).
        llvm::Value* firstPtr = builder->CreateInBoundsGEP(getDefaultType(), newBuf, one64, "ush.firstptr");
        emitStoreArrayElem(valArg, firstPtr);

        // Update length header.
        emitStoreArrayLen(newLen, newBuf);
        return newBuf;
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
        llvm::Value* arrPtr = getArrayPtr(arrArg);
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
        llvm::Value* arrPtr = getArrayPtr(arrArg);
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
        // array_contains returns 0 or 1 — mark non-negative.
        // Note: !range is not valid on phi instructions in LLVM 18.
        nonNegValues_.insert(result);
        return result;
    }

    if (bid == BuiltinId::SORT) {
        validateArgCount(expr, "sort", 1);
        const bool sortStrings = isStringArrayExpr(expr->arguments[0].get());
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* sortLenLoad = emitLoadArrayLen(arrPtr, "sort.len");
        llvm::Value* arrLen = sortLenLoad;

        // Early exit: skip qsort for arrays with 0 or 1 elements (already sorted).
        // This avoids the overhead of a function call to qsort for trivial cases.
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* sortBB = llvm::BasicBlock::Create(*context, "sort.call", function);
        llvm::BasicBlock* skipBB = llvm::BasicBlock::Create(*context, "sort.skip", function);
        llvm::Value* needsSort =
            builder->CreateICmpUGT(arrLen, llvm::ConstantInt::get(getDefaultType(), 1), "sort.needed");
        builder->CreateCondBr(needsSort, sortBB, skipBB);
        builder->SetInsertPoint(sortBB);

        // Use libc qsort() — O(n log n) instead of the previous O(n²)

        auto getOrEmitIntCmp = [&]() -> llvm::Function* {
            const char* name = "__omsc_cmp_i64_asc";
            if (auto* fn = module->getFunction(name))
                return fn;
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            auto* cmpTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, ptrTy}, false);
            auto* fn = llvm::Function::Create(cmpTy, llvm::Function::InternalLinkage, name, module.get());
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
            // Do NOT use IsNonNeg=true on zext i1: i1 value 1 (true) is -1
            // as a signed 1-bit integer, so `zext nneg i1 true` = POISON.
            auto* gt =
                builder->CreateZExt(builder->CreateICmpSGT(a, b, "gt"), llvm::Type::getInt32Ty(*context), "gt.zext",
                                    /*IsNonNeg=*/false);
            auto* lt =
                builder->CreateZExt(builder->CreateICmpSLT(a, b, "lt"), llvm::Type::getInt32Ty(*context), "lt.zext",
                                    /*IsNonNeg=*/false);
            builder->CreateRet(builder->CreateSub(gt, lt, "cmp"));
            builder->restoreIP(savedIP);
            return fn;
        };

        auto getOrEmitStrCmp = [&]() -> llvm::Function* {
            const char* name = "__omsc_cmp_str_asc";
            if (auto* fn = module->getFunction(name))
                return fn;
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            auto* cmpTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, ptrTy}, false);
            auto* fn = llvm::Function::Create(cmpTy, llvm::Function::InternalLinkage, name, module.get());
            fn->addFnAttr(llvm::Attribute::NoUnwind);
            fn->addFnAttr(llvm::Attribute::WillReturn);
            fn->addFnAttr(llvm::Attribute::NoFree);
            fn->addFnAttr(llvm::Attribute::NoSync);
            // String comparator: load i64 fat-string pointers, GEP to data
            // (offset 16 = past {len, cap}), then strcmp on the C-string data.
            auto savedIP = builder->saveIP();
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            builder->SetInsertPoint(entry);
            auto* aSlotPtr = fn->getArg(0); // ptr to i64 slot
            auto* bSlotPtr = fn->getArg(1); // ptr to i64 slot
            auto* aI64 = builder->CreateAlignedLoad(getDefaultType(), aSlotPtr, llvm::MaybeAlign(8), "a.i64");
            auto* bI64 = builder->CreateAlignedLoad(getDefaultType(), bSlotPtr, llvm::MaybeAlign(8), "b.i64");
            llvm::cast<llvm::Instruction>(aI64)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            llvm::cast<llvm::Instruction>(bI64)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            auto* aFat = builder->CreateIntToPtr(aI64, ptrTy, "a.fat");
            auto* bFat = builder->CreateIntToPtr(bI64, ptrTy, "b.fat");
            // Skip the fat-string header (i64 len + i64 cap = 16 bytes) to reach the char data.
            auto* off16 = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 16);
            auto* aData = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), aFat, off16, "a.data");
            auto* bData = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), bFat, off16, "b.data");
            auto* result = builder->CreateCall(getOrDeclareStrcmp(), {aData, bData}, "cmp");
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
        // ── ro-global fast path ─────────────────────────────────────────────
        if (pendingArrayReadOnlyGlobal_ && optimizationLevel >= OptimizationLevel::O2) {
            auto getConstInt = [](const Expression* e, int64_t& out) -> bool {
                if (!e)
                    return false;
                if (e->type == ASTNodeType::LITERAL_EXPR) {
                    auto* lit = static_cast<const LiteralExpr*>(e);
                    if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
                        out = lit->intValue;
                        return true;
                    }
                }
                if (e->type == ASTNodeType::UNARY_EXPR) {
                    auto* un = static_cast<const UnaryExpr*>(e);
                    if (un->op == "-" && un->operand && un->operand->type == ASTNodeType::LITERAL_EXPR) {
                        auto* lit = static_cast<const LiteralExpr*>(un->operand.get());
                        if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
                            out = -lit->intValue;
                            return true;
                        }
                    }
                }
                return false;
            };
            int64_t nVal = 0, vVal = 0;
            if (getConstInt(expr->arguments[0].get(), nVal) && getConstInt(expr->arguments[1].get(), vVal) &&
                nVal >= 2 && nVal <= 1024) {
                const size_t totalSlots = 1 + static_cast<size_t>(nVal);
                auto* arrTy = llvm::ArrayType::get(getDefaultType(), totalSlots);
                llvm::Constant* initArray = nullptr;
                if (vVal == 0) {
                    // Zero fill: build [N, 0, 0, ...] by hand because
                    // zeroinitializer would also zero the length slot.
                    std::vector<llvm::Constant*> initVals(totalSlots, llvm::ConstantInt::get(getDefaultType(), 0));
                    initVals[0] = llvm::ConstantInt::get(getDefaultType(), nVal);
                    initArray = llvm::ConstantArray::get(arrTy, initVals);
                } else {
                    std::vector<llvm::Constant*> initVals;
                    initVals.reserve(totalSlots);
                    initVals.push_back(llvm::ConstantInt::get(getDefaultType(), nVal));
                    auto* fillC = llvm::ConstantInt::get(getDefaultType(), vVal);
                    for (size_t i = 0; i < static_cast<size_t>(nVal); ++i)
                        initVals.push_back(fillC);
                    initArray = llvm::ConstantArray::get(arrTy, initVals);
                }
                auto* gv = new llvm::GlobalVariable(*module, arrTy, /*isConstant=*/true,
                                                    llvm::GlobalValue::PrivateLinkage, initArray, "fill.ro.const");
                gv->setAlignment(llvm::Align(16));
                gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                return gv;
            }
        }
        llvm::Value* sizeArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        sizeArg = toDefaultType(sizeArg);
        valArg = toDefaultType(valArg);
        // Clamp negative sizes to 0 to prevent integer overflow in the
        {
            llvm::Value* zeroClamp = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* isNeg = builder->CreateICmpSLT(sizeArg, zeroClamp, "fill.isneg");
            sizeArg = builder->CreateSelect(isNeg, zeroClamp, sizeArg, "fill.clamp");
            nonNegValues_.insert(sizeArg);
        }
        // Allocate: (size + 1) * 8 bytes.  Header slot stores the length.
        llvm::Value* slots = builder->CreateAdd(sizeArg, llvm::ConstantInt::get(getDefaultType(), 1), "fill.slots",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Optimization: when filling with zero, use calloc() instead of
        bool isZeroFill = false;
        if (auto* constVal = llvm::dyn_cast<llvm::ConstantInt>(valArg)) {
            isZeroFill = constVal->isZero();
        }

        llvm::Value* buf;
        if (isZeroFill) {
            // calloc(slots, 8) returns pre-zeroed memory — both the header
            buf = builder->CreateCall(getOrDeclareCalloc(), {slots, eight}, "fill.buf");
            llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
            // Advertise exact allocation when sizeArg is constant (common case after
            // constant propagation: `array_fill(100, 0)` → known 808-byte buffer).
            if (auto* constSize = llvm::dyn_cast<llvm::ConstantInt>(sizeArg)) {
                const uint64_t exactBytes = (constSize->getZExtValue() + 1) * 8;
                llvm::cast<llvm::CallInst>(buf)->addRetAttr(
                    llvm::Attribute::getWithDereferenceableBytes(*context, exactBytes));
            } else {
                llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
            }
            // Store length in header (calloc zeroed it; overwrite with actual size)
            emitStoreArrayLen(sizeArg, buf);
        } else {
            llvm::Value* bytes = builder->CreateMul(slots, eight, "fill.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
            buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "fill.buf");
            llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
            // Advertise exact allocation size to LLVM when sizeArg is a constant
            // so alias analysis, bounds checks, and vectorization can use it.
            if (auto* constSize = llvm::dyn_cast<llvm::ConstantInt>(sizeArg)) {
                const uint64_t exactBytes = (constSize->getZExtValue() + 1) * 8;
                llvm::cast<llvm::CallInst>(buf)->addRetAttr(
                    llvm::Attribute::getWithDereferenceableBytes(*context, exactBytes));
            } else {
                // Dynamic size: at minimum the 8-byte header is always valid.
                llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
            }
            emitStoreArrayLen(sizeArg, buf);
            // Fill loop
            llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
            // Capture buf/valArg for the lambda
            llvm::Value* fillBuf = buf;
            llvm::Value* fillVal = valArg;
            emitCountingLoop("fill", sizeArg, zero, 4, [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
                llvm::Value* elemIdx = builder->CreateAdd(idx, one, "fill.elemidx",
                                                          /*HasNUW=*/true, /*HasNSW=*/true);
                llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), fillBuf, elemIdx, "fill.elemptr");
                emitStoreArrayElem(fillVal, elemPtr);
                llvm::Value* nextIdx = builder->CreateAdd(idx, one, "fill.next",
                                                          /*HasNUW=*/true, /*HasNSW=*/true);
                idx->addIncoming(nextIdx, builder->GetInsertBlock());
                attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
            });
        }
        return buf;
    }

    if (bid == BuiltinId::ARRAY_CONCAT) {
        validateArgCount(expr, "array_concat", 2);
        llvm::Value* arr1Arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arr2Arg = generateExpression(expr->arguments[1].get());
        arr1Arg = toDefaultType(arr1Arg);
        arr2Arg = toDefaultType(arr2Arg);
        llvm::Value* arr1Ptr = getArrayPtr(arr1Arg);
        llvm::Value* arr2Ptr = getArrayPtr(arr2Arg);
        llvm::Value* acatLen1Load = emitLoadArrayLen(arr1Ptr, "aconcat.len1");
        llvm::Value* len1 = acatLen1Load;
        llvm::Value* acatLen2Load = emitLoadArrayLen(arr2Ptr, "aconcat.len2");
        llvm::Value* len2 = acatLen2Load;
        llvm::Value* totalLen = builder->CreateAdd(len1, len2, "aconcat.total", /*HasNUW=*/true, /*HasNSW=*/true);
        // Both len1 and len2 are non-negative (range metadata), so their sum is too.
        nonNegValues_.insert(totalLen);
        // Allocate: (totalLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "aconcat.slots",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, llvm::ConstantInt::get(getDefaultType(), 8), "aconcat.bytes",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "aconcat.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
        // Advertise exact allocation when both source lengths are known constants.
        if (auto* c1 = llvm::dyn_cast<llvm::ConstantInt>(len1)) {
            if (auto* c2 = llvm::dyn_cast<llvm::ConstantInt>(len2)) {
                const uint64_t exactBytes = (c1->getZExtValue() + c2->getZExtValue() + 1) * 8;
                llvm::cast<llvm::CallInst>(buf)->addRetAttr(
                    llvm::Attribute::getWithDereferenceableBytes(*context, exactBytes));
            } else {
                llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
            }
        } else {
            llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        }
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
        return buf;
    }

    if (bid == BuiltinId::ARRAY_SLICE) {
        validateArgCount(expr, "array_slice", 3);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* startArg = generateExpression(expr->arguments[1].get());
        llvm::Value* endArg = generateExpression(expr->arguments[2].get());
        arrArg = toDefaultType(arrArg);
        startArg = toDefaultType(startArg);
        endArg = toDefaultType(endArg);
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* sliceLenLoad = emitLoadArrayLen(arrPtr, "slice.arrlen");
        llvm::Value* arrLen = sliceLenLoad;

        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // ── Partial constant-bounds optimisation ──────────────────────────────
        auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startArg);
        auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endArg);

        if (startCI && startCI->getSExtValue() >= 0) {
            // start is non-negative constant: skip the 2 start-clamp ops.
            // startFinal = clamp to arrLen still needed if start > arrLen:
            llvm::Value* startOver = builder->CreateICmpSGT(startArg, arrLen, "slice.startover");
            startArg = builder->CreateSelect(startOver, arrLen, startArg, "slice.startfinal");

            if (endCI && endCI->getSExtValue() >= startCI->getSExtValue()) {
                // end is also non-negative constant ≥ start: skip endNeg clamp.
                llvm::Value* endOver = builder->CreateICmpSGT(endArg, arrLen, "slice.endover");
                endArg = builder->CreateSelect(endOver, arrLen, endArg, "slice.endfinal");
            } else {
                // end is runtime: clamp end to [start, arrLen].
                llvm::Value* endNeg = builder->CreateICmpSLT(endArg, startArg, "slice.endneg");
                endArg = builder->CreateSelect(endNeg, startArg, endArg, "slice.endclamp");
                llvm::Value* endOver = builder->CreateICmpSGT(endArg, arrLen, "slice.endover");
                endArg = builder->CreateSelect(endOver, arrLen, endArg, "slice.endfinal");
            }
        } else {
            // General path: clamp both start and end.
            llvm::Value* startNeg = builder->CreateICmpSLT(startArg, zero, "slice.startneg");
            startArg = builder->CreateSelect(startNeg, zero, startArg, "slice.startclamp");
            llvm::Value* startOver = builder->CreateICmpSGT(startArg, arrLen, "slice.startover");
            startArg = builder->CreateSelect(startOver, arrLen, startArg, "slice.startfinal");
            llvm::Value* endNeg = builder->CreateICmpSLT(endArg, startArg, "slice.endneg");
            endArg = builder->CreateSelect(endNeg, startArg, endArg, "slice.endclamp");
            llvm::Value* endOver = builder->CreateICmpSGT(endArg, arrLen, "slice.endover");
            endArg = builder->CreateSelect(endOver, arrLen, endArg, "slice.endfinal");
        }

        // endArg ≥ startArg is guaranteed by all clamp paths above — nuw+nsw.
        llvm::Value* sliceLen = builder->CreateSub(endArg, startArg, "slice.len",
                                                   /*HasNUW=*/true, /*HasNSW=*/true);
        // All clamp paths guarantee endArg ≥ startArg, so sliceLen ≥ 0.
        nonNegValues_.insert(sliceLen);
        llvm::Value* buf = emitAllocArray(sliceLen, "slice");
        // Copy elements: arr[start+1..end+1) to buf[1..)
        llvm::Value* srcIdx = builder->CreateAdd(startArg, one, "slice.srcidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* src = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, srcIdx, "slice.src");
        llvm::Value* dst = builder->CreateInBoundsGEP(getDefaultType(), buf, one, "slice.dst");
        llvm::Value* copySize = builder->CreateMul(sliceLen, eight, "slice.copysize", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemcpy(), {dst, src, copySize});
        return buf;
    }

    if (bid == BuiltinId::ARRAY_COPY) {
        validateArgCount(expr, "array_copy", 1);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* acopyLenLoad = emitLoadArrayLen(arrPtr, "acopy.len");
        llvm::Value* arrLen = acopyLenLoad;
        // Allocate: (length + 1) * 8 bytes
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "acopy.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "acopy.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "acopy.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        builder->CreateCall(getOrDeclareMemcpy(), {buf, arrPtr, bytes});
        return buf;
    }

    if (bid == BuiltinId::ARRAY_REMOVE) {
        validateArgCount(expr, "array_remove", 2);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* idxArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        idxArg = toDefaultType(idxArg);
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* aremLenLoad = emitLoadArrayLen(arrPtr, "aremove.len");
        llvm::Value* arrLen = aremLenLoad;
        // Bounds check: 0 <= idx < length.
        // ULT(idx, arrLen) ≡ (SGE(idx,0) && SLT(idx,arrLen)) since arrLen ≥ 0.
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* valid = builder->CreateICmpULT(idxArg, arrLen, "aremove.valid");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "aremove.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "aremove.fail", function);
        // OOB is extremely unlikely.
        llvm::MDNode* removeW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(valid, okBB, failBB, removeW);
        // Out-of-bounds: print error and abort
        builder->SetInsertPoint(failBB);
        {
            std::string msg = expr->line > 0 ? std::string("Runtime error: array_remove index out of bounds at line ") +
                                                   std::to_string(expr->line) + "\n"
                                             : "Runtime error: array_remove index out of bounds\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "aremove_oob_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();
        // In-bounds: save removed value, shift elements left, decrement length
        builder->SetInsertPoint(okBB);
        llvm::Value* elemOffset = builder->CreateAdd(idxArg, one, "aremove.elemoff", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, elemOffset, "aremove.elemptr");
        llvm::Value* removedVal =
            builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "aremove.removed");
        // TBAA: removed value is an array element, never aliases the length header.
        llvm::cast<llvm::LoadInst>(removedVal)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::LoadInst>(removedVal)
                ->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
        // memmove(&arr[idx+1], &arr[idx+2], (length - idx - 1) * 8)
        llvm::Value* srcOffset = builder->CreateAdd(idxArg, llvm::ConstantInt::get(getDefaultType(), 2),
                                                    "aremove.srcoff", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* srcPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, srcOffset, "aremove.srcptr");
        llvm::Value* shiftCount = builder->CreateSub(
            arrLen, builder->CreateAdd(idxArg, one, "aremove.idxp1", /*HasNUW=*/true, /*HasNSW=*/true),
            "aremove.shiftcnt", /*HasNUW=*/true, /*HasNSW=*/true);
        // shiftCount = arrLen - idx - 1 ≥ 0 (since idx < arrLen guarantees arrLen ≥ idx+1).
        nonNegValues_.insert(shiftCount);
        llvm::Value* shiftBytes =
            builder->CreateMul(shiftCount, eight, "aremove.shiftbytes", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemmove(), {elemPtr, srcPtr, shiftBytes});
        // Decrement length: arrLen ≥ 1 since a valid index exists, so sub is nuw+nsw.
        llvm::Value* newLen = builder->CreateSub(arrLen, one, "aremove.newlen", /*HasNUW=*/true, /*HasNSW=*/true);
        nonNegValues_.insert(newLen);
        emitStoreArrayLen(newLen, arrPtr);
        return removedVal;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_MAP) {
        validateArgCount(expr, "array_map", 2);
        // The second argument must be a function name (string literal or identifier)
        const std::string fnName = extractFnName(expr->arguments[1].get());
        if (fnName.empty()) {
            codegenError("array_map: second argument must be a function name (string literal or identifier)", expr);
        }
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
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* amapLenLoad = emitLoadArrayLen(arrPtr, "amap.len");
        llvm::Value* arrLen = amapLenLoad;

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);

        // Allocate result array: (arrLen + 1) * 8
        llvm::Value* buf = emitAllocArray(arrLen, "amap");

        // Loop: for each element, call mapFn and store result
        llvm::Value* amapBuf = buf;
        llvm::Value* amapArrPtr = arrPtr;
        llvm::Value* amapArrLen = arrLen;
        emitCountingLoop("amap", arrLen, zero, 4, [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
            llvm::Value* elemIdx = builder->CreateAdd(idx, one, "amap.elemidx", /*HasNUW=*/true, /*HasNSW=*/true);
            llvm::Value* srcPtr = builder->CreateInBoundsGEP(getDefaultType(), amapArrPtr, elemIdx, "amap.srcptr");
            llvm::Value* elem = emitLoadArrayElem(srcPtr, "amap.elem");
            if (optimizationLevel >= OptimizationLevel::O1)
                llvm::cast<llvm::Instruction>(elem)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                                 llvm::MDNode::get(*context, {}));
            llvm::Value* mapped = builder->CreateCall(mapFn, {elem}, "amap.mapped");
            mapped = toDefaultType(mapped);
            llvm::Value* dstPtr = builder->CreateInBoundsGEP(getDefaultType(), amapBuf, elemIdx, "amap.dstptr");
            emitStoreArrayElem(mapped, dstPtr);
            llvm::Value* nextIdx = builder->CreateAdd(idx, one, "amap.next", /*HasNUW=*/true, /*HasNSW=*/true);
            idx->addIncoming(nextIdx, builder->GetInsertBlock());
            attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        });
        (void)amapArrLen;
        return buf;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_FILTER) {
        validateArgCount(expr, "array_filter", 2);
        const std::string fnName = extractFnName(expr->arguments[1].get());
        if (fnName.empty()) {
            codegenError("array_filter: second argument must be a function name (string literal or identifier)", expr);
        }
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
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* afiltLenLoad = emitLoadArrayLen(arrPtr, "afilt.len");
        llvm::Value* arrLen = afiltLenLoad;

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Allocate result array with max possible size: (arrLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "afilt.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "afilt.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "afilt.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
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
        // outIdx counts accepted elements: starts at 0, incremented by 1 each accepted
        // element, so it is always in [0, arrLen] — non-negative.
        nonNegValues_.insert(outIdx);
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
        // outIdxMerge ∈ [0, arrLen] by the same argument as outIdx.
        nonNegValues_.insert(outIdxMerge);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "afilt.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, incBB);
        outIdx->addIncoming(outIdxMerge, incBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // Done: store final length
        builder->SetInsertPoint(doneBB);
        emitStoreArrayLen(outIdx, buf);
        return buf;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_REDUCE) {
        validateArgCount(expr, "array_reduce", 3);
        const std::string fnName = extractFnName(expr->arguments[1].get());
        if (fnName.empty()) {
            codegenError("array_reduce: second argument must be a function name (string literal or identifier)", expr);
        }
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
        llvm::Value* arrPtr = getArrayPtr(arrArg);
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
                    if (elem.kind != ConstValue::Kind::Integer) {
                        allInt = false;
                        break;
                    }
                    if (elem.intVal < minVal)
                        minVal = elem.intVal;
                }
                if (allInt) {
                    optStats_.constFolded++;
                    return llvm::ConstantInt::get(getDefaultType(), minVal);
                }
            }
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = getArrayPtr(arg);
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
        llvm::Function* sminFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smin, {getDefaultType()});
        llvm::Value* newMin = builder->CreateCall(sminFn, {elem, curMin}, "amin.newmin");
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        // Eliminates a redundant add and provides SCEV with tight nsw+nuw flags.
        curMin->addIncoming(newMin, bodyBB);
        idx->addIncoming(offset, bodyBB);
        attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // Done: return result
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "amin.result");
        result->addIncoming(zero, emptyBB);
        result->addIncoming(curMin, loopBB);
        return result;
    }

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
                    if (elem.kind != ConstValue::Kind::Integer) {
                        allInt = false;
                        break;
                    }
                    if (elem.intVal > maxVal)
                        maxVal = elem.intVal;
                }
                if (allInt) {
                    optStats_.constFolded++;
                    auto* ci = llvm::ConstantInt::get(getDefaultType(), maxVal);
                    if (maxVal >= 0)
                        nonNegValues_.insert(ci);
                    return ci;
                }
            }
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = getArrayPtr(arg);
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
        curMax->addIncoming(newMax, bodyBB);
        idx->addIncoming(offset, bodyBB);
        attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        // Done: return result
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "amax.result");
        result->addIncoming(zero, emptyBB);
        result->addIncoming(curMax, loopBB);
        return result;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_ANY) {
        validateArgCount(expr, "array_any", 2);
        const std::string fnName = extractFnName(expr->arguments[1].get());
        if (fnName.empty()) {
            codegenError("array_any: second argument must be a function name (string literal or identifier)", expr);
        }
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
        llvm::Value* arrPtr = getArrayPtr(arrArg);
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
        // aany returns 0 or 1 — track non-negativity.
        // Note: !range is not valid on phi instructions in LLVM 18.
        nonNegValues_.insert(result);
        return result;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_EVERY) {
        validateArgCount(expr, "array_every", 2);
        const std::string fnName = extractFnName(expr->arguments[1].get());
        if (fnName.empty()) {
            codegenError("array_every: second argument must be a function name (string literal or identifier)", expr);
        }
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
        llvm::Value* arrPtr = getArrayPtr(arrArg);
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
        // aevery returns 0 or 1 — track non-negativity.
        // Note: !range is not valid on phi instructions in LLVM 18.
        nonNegValues_.insert(result);
        return result;
    }

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
        llvm::Value* arrPtr = getArrayPtr(arg);
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
    if (bid == BuiltinId::ARRAY_COUNT) {
        validateArgCount(expr, "array_count", 2);
        const std::string fnName = extractFnName(expr->arguments[1].get());
        if (fnName.empty()) {
            codegenError("array_count: second argument must be a function name (string literal or identifier)", expr);
        }
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
        llvm::Value* arrPtr = getArrayPtr(arrArg);
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
        // acc counts matching elements: starts at 0, incremented by {0,1} — always ≥ 0.
        nonNegValues_.insert(acc);
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
        llvm::Value* incr = emitBoolZExt(isNonZero, "acnt.incr");
        // acnt.newacc: acc is in [0, length], incr in {0,1}; sum ≤ INT64_MAX so
        // both nsw and nuw are safe and let SCEV prove the accumulator is non-negative.
        llvm::Value* newAcc = builder->CreateAdd(acc, incr, "acnt.newacc", /*HasNUW=*/true, /*HasNSW=*/true);
        nonNegValues_.insert(newAcc); // newAcc = acc + {0,1} ≥ 0
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(offset, bodyBB);
        { attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB))); }

        builder->SetInsertPoint(doneBB);
        return acc;
    }

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
            llvm::Value* strData = emitStringData(arg, "println.data");
            builder->CreateCall(getOrDeclarePuts(), {strData});
        } else {
            // println integer — printf %lld requires a 64-bit argument.
            // Widen narrow integers; truncate integers wider than i64 (e.g. i128).
            if (arg->getType()->isIntegerTy() && !arg->getType()->isIntegerTy(64)) {
                const unsigned bits = arg->getType()->getIntegerBitWidth();
                if (bits > 64) {
                    arg = builder->CreateTrunc(arg, getDefaultType(), "println.trunc");
                } else {
                    const bool isUnsigned = unsignedExprs_.count(arg) || isUnsignedValue(arg);
                    arg = isUnsigned ? builder->CreateZExt(arg, getDefaultType(), "println.zext",
                                                           /*IsNonNeg=*/false)
                                     : builder->CreateSExt(arg, getDefaultType(), "println.sext");
                    if (isUnsigned)
                        nonNegValues_.insert(arg);
                }
            }
            llvm::GlobalVariable* formatStr = module->getGlobalVariable("println_fmt", true);
            if (!formatStr) {
                formatStr = builder->CreateGlobalString("%lld\n", "println_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {formatStr, arg});
        }
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

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
            llvm::Value* strData = emitStringData(arg, "write.data");
            builder->CreateCall(getOrDeclareFputs(), {strData, getOrDeclareStdout()});
        } else {
            // write integer — printf %lld requires a 64-bit argument.
            // Widen narrow integers; truncate integers wider than i64 (e.g. i128).
            if (arg->getType()->isIntegerTy() && !arg->getType()->isIntegerTy(64)) {
                const unsigned bits = arg->getType()->getIntegerBitWidth();
                if (bits > 64) {
                    arg = builder->CreateTrunc(arg, getDefaultType(), "write.trunc");
                } else {
                    const bool isUnsigned = unsignedExprs_.count(arg) || isUnsignedValue(arg);
                    arg = isUnsigned ? builder->CreateZExt(arg, getDefaultType(), "write.zext",
                                                           /*IsNonNeg=*/false)
                                     : builder->CreateSExt(arg, getDefaultType(), "write.sext");
                    if (isUnsigned)
                        nonNegValues_.insert(arg);
                }
            }
            llvm::GlobalVariable* formatStr = module->getGlobalVariable("write_fmt", true);
            if (!formatStr) {
                formatStr = builder->CreateGlobalString("%lld", "write_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {formatStr, arg});
        }
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

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
    if (bid == BuiltinId::RANDOM) {
        validateArgCount(expr, "random", 0);
        // Seed on first call via a global i1 flag (use i1 directly to avoid
        // globalopt converting i32→i1 which can cause issues on some targets).
        llvm::GlobalVariable* seeded = module->getGlobalVariable("__om_rand_seeded", true);
        if (!seeded) {
            seeded = new llvm::GlobalVariable(
                *module, llvm::Type::getInt1Ty(*context), false, llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::getFalse(*context), "__om_rand_seeded");
        }
        llvm::Value* flag = builder->CreateLoad(llvm::Type::getInt1Ty(*context), seeded, "rand.flag");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* seedBB = llvm::BasicBlock::Create(*context, "rand.seed", function);
        llvm::BasicBlock* callBB = llvm::BasicBlock::Create(*context, "rand.call", function);

        // Branch: if flag is true (already seeded) → callBB, else → seedBB
        builder->CreateCondBr(flag, callBB, seedBB);

        builder->SetInsertPoint(seedBB);
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* t = builder->CreateCall(getOrDeclareTimeFunc(), {nullPtr}, "rand.time");
        llvm::Value* t32 = builder->CreateTrunc(t, llvm::Type::getInt32Ty(*context), "rand.time32");
        builder->CreateCall(getOrDeclareSrand(), {t32});
        builder->CreateStore(llvm::ConstantInt::getTrue(*context), seeded);
        builder->CreateBr(callBB);

        builder->SetInsertPoint(callBB);
        llvm::Value* r = builder->CreateCall(getOrDeclareRand(), {}, "rand.val");
        return builder->CreateSExt(r, getDefaultType(), "rand.ext");
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::TIME) {
        validateArgCount(expr, "time", 0);
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        return builder->CreateCall(getOrDeclareTimeFunc(), {nullPtr}, "time.val");
    }

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
    if (bid == BuiltinId::STR_TO_INT) {
        validateArgCount(expr, "str_to_int", 1);
        // Constant-fold str_to_int("literal") at compile time.
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            try {
                int64_t val = static_cast<int64_t>(std::stoll(*sv));
                optStats_.constFolded++;
                auto* ci = llvm::ConstantInt::get(getDefaultType(), val);
                if (val >= 0)
                    nonNegValues_.insert(ci);
                return ci;
            } catch (...) {
            } // NOLINT(bugprone-empty-catch)
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strtoi.ptr");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* base10 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10);
        return builder->CreateCall(getOrDeclareStrtoll(), {emitStringData(strPtr, "strtoi.data"), nullPtr, base10},
                                   "strtoi.val");
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_TO_FLOAT) {
        validateArgCount(expr, "str_to_float", 1);
        // Constant-fold str_to_float("literal") at compile time.
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            try {
                double val = std::stod(*sv);
                optStats_.constFolded++;
                return llvm::ConstantFP::get(getFloatType(), val);
            } catch (...) {
            } // NOLINT(bugprone-empty-catch)
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strtof.ptr");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        return builder->CreateCall(getOrDeclareStrtod(), {emitStringData(strPtr, "strtof.data"), nullPtr},
                                   "strtof.val");
    }

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
        auto* delimCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context),
                                                  emitStringData(delimPtr, "split.delimdata"), "split.delimch");
        delimCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* delimChar32 =
            builder->CreateZExt(delimCharLoad, llvm::Type::getInt32Ty(*context), "split.delimch32",
                                /*IsNonNeg=*/false);
        nonNegValues_.insert(delimChar32);

        // Count delimiters to know array size
        llvm::Value* strLen = emitStringLen(strPtr, "split.strlen");
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
        llvm::Value* splitStrData = emitStringData(strPtr, "split.sdata");
        llvm::Value* charPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), splitStrData, ci, "split.cptr");
        auto* splitChLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "split.ch");
        splitChLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* ch32 = builder->CreateZExt(splitChLoad, llvm::Type::getInt32Ty(*context), "split.ch32",
                                                /*IsNonNeg=*/false);
        nonNegValues_.insert(ch32);
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
        llvm::cast<llvm::CallInst>(arrBuf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
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
        llvm::Value* splitBodyData = emitStringData(strPtr, "split.bodydata");
        llvm::Value* bodyCharPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), splitBodyData, si, "split.bptr");
        auto* bodyChLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), bodyCharPtr, "split.bch");
        bodyChLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* bodyCh32 = builder->CreateZExt(bodyChLoad, llvm::Type::getInt32Ty(*context), "split.bch32",
                                                    /*IsNonNeg=*/false);
        nonNegValues_.insert(bodyCh32);
        llvm::Value* bodyIsDelim = builder->CreateICmpEQ(bodyCh32, delimChar32, "split.bisdelim");
        llvm::Value* shouldSplit = builder->CreateOr(atEnd, bodyIsDelim, "split.shouldsplit");
        builder->CreateCondBr(shouldSplit, splitDelimBB, splitContBB);

        builder->SetInsertPoint(splitDelimBB);
        // Create fat-ptr substring from partStart to si
        llvm::Value* partLen = builder->CreateSub(si, partStart, "split.plen", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* splitSrcData = emitStringData(strPtr, "split.srcdata");
        llvm::Value* srcStart =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), splitSrcData, partStart, "split.srcstart");
        llvm::Value* subHdr = emitAllocString(partLen, partLen, "split.sub");
        llvm::Value* subDst = emitStringData(subHdr, "split.subdst");
        builder->CreateCall(getOrDeclareMemcpy(), {subDst, srcStart, partLen});
        llvm::Value* subNulPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), subDst, partLen, "split.subnul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), subNulPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* subInt = builder->CreatePtrToInt(subHdr, getDefaultType(), "split.subint");
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
        return arrBuf;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_CHARS) {
        validateArgCount(expr, "str_chars", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "chars.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "chars.len");

        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);

        // Allocate array: (len + 1) * 8
        llvm::Value* slots = builder->CreateAdd(strLen, one, "chars.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "chars.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "chars.buf");
        emitStoreArrayLen(strLen, buf);

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
        llvm::Value* charsStrData = emitStringData(strPtr, "chars.sdata");
        llvm::Value* charP =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), charsStrData, idx, "chars.cptr");
        auto* charsChLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charP, "chars.ch");
        charsChLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        // Store the unsigned char code (zero-extended to i64) in the array slot.
        llvm::Value* chCode = builder->CreateZExt(charsChLoad, getDefaultType(), "chars.code");
        nonNegValues_.insert(chCode);
        llvm::Value* arrSlot = builder->CreateAdd(idx, one, "chars.slot", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* arrSlotPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, arrSlot, "chars.slotptr");
        builder->CreateStore(chCode, arrSlotPtr);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "chars.next", /*HasNUW=*/true, /*HasNSW=*/true);
        idx->addIncoming(nextIdx, bodyBB);
        { attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB))); }

        builder->SetInsertPoint(doneBB);
        return buf;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_JOIN) {
        validateArgCount(expr, "str_join", 2);
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* delimArg = generateExpression(expr->arguments[1].get());

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* arrPtr = getArrayPtr(arrArg);
        llvm::Value* delimPtr =
            delimArg->getType()->isPointerTy() ? delimArg : builder->CreateIntToPtr(delimArg, ptrTy, "join.delim");

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* i8zero = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0);

        llvm::Value* joinLenLoad = emitLoadArrayLen(arrPtr, "join.len");
        llvm::Value* arrLen = joinLenLoad;
        llvm::Value* delimLen = emitStringLen(delimPtr, "join.delimlen");

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
        llvm::Value* elemLen = emitStringLen(elemPtr, "join.elemlen");
        llvm::Value* newTotal = builder->CreateAdd(totalLen, elemLen, "join.newtot", /*HasNUW=*/true, /*HasNSW=*/true);
        // Add delimiter length for all elements except the first
        llvm::Value* isFirst = builder->CreateICmpEQ(li, zero, "join.isfirst");
        llvm::Value* delimAdd = builder->CreateSelect(isFirst, zero, delimLen, "join.delimadd");
        llvm::Value* newTotal2 =
            builder->CreateAdd(newTotal, delimAdd, "join.newtot2", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextLi = builder->CreateAdd(li, one, "join.nextli", /*HasNUW=*/true, /*HasNSW=*/true);
        li->addIncoming(nextLi, lenBodyBB);
        totalLen->addIncoming(newTotal2, lenBodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(lenLoopBB)));

        // --- Allocate output fat-ptr ---
        builder->SetInsertPoint(lenDoneBB);
        llvm::Value* joinHdr = emitAllocString(totalLen, totalLen, "join");
        llvm::Value* buf = emitStringData(joinHdr, "join.buf");
        // Store null terminator at position 0 initially (in case arr is empty)
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
        builder->CreateCall(getOrDeclareMemcpy(), {dstPtr, emitStringData(delimPtr, "join.ddata"), delimCopyLen});
        llvm::Value* afterDelim =
            builder->CreateAdd(writePos, delimCopyLen, "join.afterdelim", /*HasNUW=*/true, /*HasNSW=*/true);
        // Load and copy element
        llvm::Value* cslot = builder->CreateAdd(ci, one, "join.cslot", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* cslotPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr, cslot, "join.cslotptr");
        llvm::Value* celemInt = emitLoadArrayElem(cslotPtr, "join.celemint");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(celemInt)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                                 llvm::MDNode::get(*context, {}));
        llvm::Value* celemPtr = builder->CreateIntToPtr(celemInt, ptrTy, "join.celemptr");
        llvm::Value* celemLen = emitStringLen(celemPtr, "join.celemlen");
        llvm::Value* elemDst =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, afterDelim, "join.elemdst");
        builder->CreateCall(getOrDeclareMemcpy(), {elemDst, emitStringData(celemPtr, "join.edata"), celemLen});
        llvm::Value* afterElem =
            builder->CreateAdd(afterDelim, celemLen, "join.afterelem", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* nextCi = builder->CreateAdd(ci, one, "join.nextci", /*HasNUW=*/true, /*HasNSW=*/true);
        ci->addIncoming(nextCi, catBodyBB);
        writePos->addIncoming(afterElem, catBodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(catLoopBB)));

        // --- Null-terminate and return ---
        builder->SetInsertPoint(catDoneBB);
        llvm::Value* endPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), buf, writePos, "join.end");
        builder->CreateStore(i8zero, endPtr);
        stringReturningFunctions_.insert("str_join");
        return joinHdr;
    }

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
            strArg->getType()->isPointerTy() ? strArg : builder->CreateIntToPtr(strArg, ptrTy, "scount.str");
        llvm::Value* subPtr =
            subArg->getType()->isPointerTy() ? subArg : builder->CreateIntToPtr(subArg, ptrTy, "scount.sub");

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);

        llvm::Value* subLen = emitStringLen(subPtr, "scount.sublen");

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

        // Compute the string data pointer (initial cursor) BEFORE the branch,
        // so the instruction is in mainBB where it dominates the PHI incoming value.
        // The branch must come AFTER the GEP to avoid adding instructions after a terminator.
        llvm::Value* scountStrData = emitStringData(strPtr, "scount.sdata");
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* cursor = builder->CreatePHI(ptrTy, 2, "scount.cursor");
        cursor->addIncoming(scountStrData, mainBB);
        llvm::PHINode* count = builder->CreatePHI(getDefaultType(), 2, "scount.count");
        count->addIncoming(zero, mainBB);
        // count starts at 0 and is incremented by 1 per match — always ≥ 0.
        nonNegValues_.insert(count);

        llvm::Value* found = builder->CreateCall(getOrDeclareStrstr(),
                                                 {cursor, emitStringData(subPtr, "scount.subdata")}, "scount.found");
        llvm::Value* isNull = builder->CreateICmpEQ(found, nullPtr, "scount.isnull");
        builder->CreateCondBr(isNull, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* newCount = builder->CreateAdd(count, one, "scount.newcount", /*HasNUW=*/true, /*HasNSW=*/true);
        // newCount = count + 1 ≥ 1 since count ≥ 0.
        nonNegValues_.insert(newCount);
        llvm::Value* nextCursor =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), found, subLen, "scount.next");
        cursor->addIncoming(nextCursor, bodyBB);
        count->addIncoming(newCount, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "scount.result");
        result->addIncoming(zero, emptySubBB);
        result->addIncoming(count, doneBB);
        // Both incoming values are non-negative (zero / nuw-accumulated count).
        nonNegValues_.insert(result);
        return result;
    }

    // -----------------------------------------------------------------------

    if (bid == BuiltinId::FILE_READ) {
        validateArgCount(expr, "file_read", 1);
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        llvm::Value* pathPtr =
            pathArg->getType()->isPointerTy()
                ? pathArg
                : builder->CreateIntToPtr(pathArg, llvm::PointerType::getUnqual(*context), "fread.path");
        // pathPtr is a fat-string {i64 len, i64 cap, i8[] data}; fopen needs
        // a plain C string (null-terminated), which starts at offset 16.
        llvm::Value* pathData = emitStringData(pathPtr, "fread.pathdata");

        // mode = "rb"
        llvm::GlobalVariable* mode = module->getGlobalVariable("__fread_mode", true);
        if (!mode)
            mode = builder->CreateGlobalString("rb", "__fread_mode");

        // FILE* fp = fopen(pathData, "rb")
        llvm::Value* fp = builder->CreateCall(getOrDeclareFopen(), {pathData, mode}, "fread.fp");

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

        // Null path: return empty fat-string
        llvm::Value* emptyResult = nullptr;
        builder->SetInsertPoint(nullBB);
        {
            llvm::Value* z64 = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* eh = emitAllocString(z64, z64, "fread.empty");
            builder
                ->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0),
                              emitStringData(eh, "fread.empty.data"))
                ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            emptyResult = eh;
        }
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* nullEndBB = builder->GetInsertBlock();

        // OK path: seek to end, get size, read
        builder->SetInsertPoint(okBB);
        // fseek(fp, 0, SEEK_END=2)
        builder->CreateCall(getOrDeclareFseek(), {fp, llvm::ConstantInt::get(getDefaultType(), 0),
                                                  llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 2)});
        // size = ftell(fp)
        llvm::Value* fileSize = builder->CreateCall(getOrDeclareFtell(), {fp}, "fread.size");

        // ftell returns -1 on error (e.g. non-seekable stream).  Guard against
        // passing a negative value to malloc which would wrap to a huge size.
        llvm::Value* ftellFailed =
            builder->CreateICmpSLT(fileSize, llvm::ConstantInt::get(getDefaultType(), 0), "fread.ftellbad");
        llvm::BasicBlock* ftellOkBB = llvm::BasicBlock::Create(*context, "fread.ftellok", parentFn);
        llvm::BasicBlock* ftellBadBB = llvm::BasicBlock::Create(*context, "fread.ftellbad", parentFn);
        // ftell failure is extremely rare.
        auto* ftellW = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
        builder->CreateCondBr(ftellFailed, ftellBadBB, ftellOkBB, ftellW);

        // ftell error path: close file, return empty fat-string
        builder->SetInsertPoint(ftellBadBB);
        builder->CreateCall(getOrDeclareFclose(), {fp});
        llvm::Value* ftellEmptyBuf = nullptr;
        {
            llvm::Value* z64 = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* eh = emitAllocString(z64, z64, "fread.ftempty");
            builder
                ->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0),
                              emitStringData(eh, "fread.ftempty.data"))
                ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
            ftellEmptyBuf = eh;
        }
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* ftellBadEndBB = builder->GetInsertBlock();

        // ftell OK path: proceed with read
        builder->SetInsertPoint(ftellOkBB);
        // fseek(fp, 0, SEEK_SET=0)
        builder->CreateCall(getOrDeclareFseek(), {fp, llvm::ConstantInt::get(getDefaultType(), 0),
                                                  llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0)});
        // Allocate fat-string header + file content + NUL in one block.
        llvm::Value* okHdr = emitAllocString(fileSize, fileSize, "fread.hdr");
        llvm::Value* okData = emitStringData(okHdr, "fread.hdrdata");
        // fread(data, 1, size, fp)
        builder->CreateCall(getOrDeclareFread(), {okData, llvm::ConstantInt::get(getDefaultType(), 1), fileSize, fp});
        // null terminate (emitAllocString allocates cap+1 bytes, so the slot is there)
        llvm::Value* nullTermPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), okData, fileSize, "fread.nullterm");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nullTermPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        // fclose(fp)
        builder->CreateCall(getOrDeclareFclose(), {fp});
        llvm::Value* okResult = okHdr;
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
        llvm::Value* pathPtr =
            pathArg->getType()->isPointerTy() ? pathArg : builder->CreateIntToPtr(pathArg, ptrTy, "fwrite.path");
        // pathPtr is a fat-string; extract the C-string data at offset 16.
        llvm::Value* pathData = emitStringData(pathPtr, "fwrite.pathdata");
        llvm::Value* contentPtr = contentArg->getType()->isPointerTy()
                                      ? contentArg
                                      : builder->CreateIntToPtr(contentArg, ptrTy, "fwrite.content");

        llvm::GlobalVariable* mode = module->getGlobalVariable("__fwrite_mode", true);
        if (!mode)
            mode = builder->CreateGlobalString("wb", "__fwrite_mode");

        llvm::Value* fp = builder->CreateCall(getOrDeclareFopen(), {pathData, mode}, "fwrite.fp");
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
        llvm::Value* slen = emitStringLen(contentPtr, "fwrite.len");
        builder->CreateCall(getOrDeclareFwrite(), {emitStringData(contentPtr, "fwrite.data"),
                                                   llvm::ConstantInt::get(getDefaultType(), 1), slen, fp});
        builder->CreateCall(getOrDeclareFclose(), {fp});
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "fwrite.result");
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), nullBB);
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), okBB);
        // file_write returns 0 (success) or 1 (fopen failed) — always in [0,2).
        // Note: !range is not valid on phi instructions in LLVM 18.
        nonNegValues_.insert(phi);
        return phi;
    }

    if (bid == BuiltinId::FILE_APPEND) {
        validateArgCount(expr, "file_append", 2);
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        llvm::Value* contentArg = generateExpression(expr->arguments[1].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* pathPtr =
            pathArg->getType()->isPointerTy() ? pathArg : builder->CreateIntToPtr(pathArg, ptrTy, "fappend.path");
        // pathPtr is a fat-string; extract the C-string data at offset 16.
        llvm::Value* pathDataFa = emitStringData(pathPtr, "fappend.pathdata");
        llvm::Value* contentPtr = contentArg->getType()->isPointerTy()
                                      ? contentArg
                                      : builder->CreateIntToPtr(contentArg, ptrTy, "fappend.content");

        llvm::GlobalVariable* mode = module->getGlobalVariable("__fappend_mode", true);
        if (!mode)
            mode = builder->CreateGlobalString("a", "__fappend_mode");

        llvm::Value* fp = builder->CreateCall(getOrDeclareFopen(), {pathDataFa, mode}, "fappend.fp");
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
        llvm::Value* slen = emitStringLen(contentPtr, "fappend.len");
        builder->CreateCall(getOrDeclareFwrite(), {emitStringData(contentPtr, "fappend.data"),
                                                   llvm::ConstantInt::get(getDefaultType(), 1), slen, fp});
        builder->CreateCall(getOrDeclareFclose(), {fp});
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "fappend.result");
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), nullBB);
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), okBB);
        // file_append returns 0 (success) or 1 (fopen failed) — always in [0,2).
        // Note: !range is not valid on phi instructions in LLVM 18.
        nonNegValues_.insert(phi);
        return phi;
    }

    if (bid == BuiltinId::FILE_EXISTS) {
        validateArgCount(expr, "file_exists", 1);
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* pathPtr =
            pathArg->getType()->isPointerTy() ? pathArg : builder->CreateIntToPtr(pathArg, ptrTy, "fexists.path");
        // pathPtr is a fat-string; extract the C-string data at offset 16.
        llvm::Value* pathDataFe = emitStringData(pathPtr, "fexists.pathdata");
        // access(pathData, F_OK=0) returns 0 on success, -1 on failure
        llvm::Value* result = builder->CreateCall(
            getOrDeclareAccess(), {pathDataFe, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0)},
            "fexists.access");
        llvm::Value* isZero =
            builder->CreateICmpEQ(result, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "fexists.cmp");
        return emitBoolZExt(isZero, "fexists.result");
    }

    // -----------------------------------------------------------------------

    if (bid == BuiltinId::MAP_NEW) {
        validateArgCount(expr, "map_new", 0);
        llvm::Value* buf = builder->CreateCall(getOrEmitHashMapNew(), {}, "mapnew.buf");
        return buf;
    }

    if (bid == BuiltinId::MAP_SET) {
        validateArgCount(expr, "map_set", 3);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        llvm::Value* valArg = generateExpression(expr->arguments[2].get());
        keyArg = toDefaultType(keyArg);
        valArg = toDefaultType(valArg);

        llvm::Value* mapPtr = mapArg->getType()->isPointerTy() ? mapArg : getArrayPtr(mapArg);
        llvm::Value* result = builder->CreateCall(getOrEmitHashMapSet(), {mapPtr, keyArg, valArg}, "mapset.result");
        return result;
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
                                  ? mapArg
                                  : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "mapget.ptr");
        return builder->CreateCall(getOrEmitHashMapGet(), {mapPtr, keyArg, defArg}, "mapget.result");
    }

    if (bid == BuiltinId::MAP_HAS) {
        validateArgCount(expr, "map_has", 2);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        keyArg = toDefaultType(keyArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
                                  ? mapArg
                                  : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "maphas.ptr");
        auto* callResult = builder->CreateCall(getOrEmitHashMapHas(), {mapPtr, keyArg}, "maphas.result");
        if (boolRangeMD_)
            callResult->setMetadata(llvm::LLVMContext::MD_range, boolRangeMD_);
        nonNegValues_.insert(callResult);
        return callResult;
    }

    if (bid == BuiltinId::MAP_REMOVE) {
        validateArgCount(expr, "map_remove", 2);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        keyArg = toDefaultType(keyArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
                                  ? mapArg
                                  : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "maprem.ptr");
        llvm::Value* result = builder->CreateCall(getOrEmitHashMapRemove(), {mapPtr, keyArg}, "maprem.result");
        return result;
    }

    if (bid == BuiltinId::MAP_KEYS) {
        validateArgCount(expr, "map_keys", 1);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
                                  ? mapArg
                                  : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "mapkeys.ptr");
        llvm::Value* buf = builder->CreateCall(getOrEmitHashMapKeys(), {mapPtr}, "mapkeys.buf");
        return buf;
    }

    if (bid == BuiltinId::MAP_VALUES) {
        validateArgCount(expr, "map_values", 1);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
                                  ? mapArg
                                  : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "mapvals.ptr");
        llvm::Value* buf = builder->CreateCall(getOrEmitHashMapValues(), {mapPtr}, "mapvals.buf");
        return buf;
    }

    if (bid == BuiltinId::MAP_SIZE) {
        validateArgCount(expr, "map_size", 1);
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = mapArg->getType()->isPointerTy()
                                  ? mapArg
                                  : builder->CreateIntToPtr(toDefaultType(mapArg), ptrTy, "mapsize.ptr");
        llvm::Value* sz = builder->CreateCall(getOrEmitHashMapSize(), {mapPtr}, "mapsize.result");
        // Map entry count is always non-negative; annotate like array lengths.
        nonNegValues_.insert(sz);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::CallInst>(sz)->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        return sz;
    }

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
        // count is clamped to ≥ 0 by the select above; inform LLVM.
        nonNegValues_.insert(count);

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
        { attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB))); }

        builder->SetInsertPoint(doneBB);
        return buf;
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
            std::string msg = expr->line > 0 ? std::string("Runtime error: range step cannot be zero at line ") +
                                                   std::to_string(expr->line) + "\n"
                                             : "Runtime error: range step cannot be zero\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "rstep_zero_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(stepOkBB);

        // count = max((end - start + step - 1) / step, 0) for positive step
        llvm::Value* diff = builder->CreateSub(endArg, startArg, "rstep.diff");
        // For positive step: count = (diff + step - 1) / step if diff > 0
        llvm::Value* adjDiff =
            builder->CreateAdd(diff, builder->CreateSub(stepArg, one, "rstep.stepm1"), "rstep.adjdiff");
        llvm::Value* count = builder->CreateSDiv(adjDiff, stepArg, "rstep.count");
        llvm::Value* isPos = builder->CreateICmpSGT(count, zero, "rstep.ispos");
        count = builder->CreateSelect(isPos, count, zero, "rstep.clampcount");
        // count is clamped to ≥ 0 by the select above; inform LLVM.
        nonNegValues_.insert(count);

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
        llvm::Value* val = builder->CreateAdd(startArg, builder->CreateMul(i, stepArg, "rstep.offset"), "rstep.val");
        // i is ULT-bounded in [0, count); slot = i+1 is in [1, count+1] — nuw+nsw.
        llvm::Value* slot = builder->CreateAdd(i, one, "rstep.slot", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, slot, "rstep.elemptr");
        builder->CreateStore(val, elemPtr);
        llvm::Value* nextI = builder->CreateAdd(i, one, "rstep.next", /*HasNUW=*/true, /*HasNSW=*/true);
        i->addIncoming(nextI, bodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        return buf;
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
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy() ? strArg : builder->CreateIntToPtr(strArg, ptrTy, "charcode.ptr");
        // Must load from the character data (offset 16), not from the fat-pointer
        // header (offset 0 = len field).  emitStringData adds the 16-byte GEP.
        llvm::Value* strData = emitStringData(strPtr, "charcode.data");
        auto* ch = builder->CreateLoad(llvm::Type::getInt8Ty(*context), strData, "charcode.ch");
        ch->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        // Zero-extend to i64; result is always in [0, 256).
        // !range is not valid on zext instructions (LLVM 18); use nonNegValues_ instead.
        auto* result = builder->CreateZExt(ch, getDefaultType(), "charcode.result",
                                           /*IsNonNeg=*/false);
        nonNegValues_.insert(result);
        return result;
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
                llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        const bool isFloat = val->getType()->isDoubleTy();
        if (!isFloat)
            val = toDefaultType(val);
        if (isFloat) {
            llvm::Value* maxLen = llvm::ConstantInt::get(getDefaultType(), 31);
            llvm::Value* hdr = emitAllocString(maxLen, maxLen, "numtostr");
            llvm::Value* bufData = emitStringData(hdr, "numtostr.data");
            llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 32);
            llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_float_fmt", true);
            if (!fmtStr)
                fmtStr = builder->CreateGlobalString("%g", "tostr_float_fmt");
            llvm::Value* written =
                builder->CreateCall(getOrDeclareSnprintf(), {bufData, bufSize, fmtStr, val}, "numtostr.written");
            llvm::Value* actualLen = builder->CreateZExt(written, getDefaultType(), "numtostr.len", /*IsNonNeg=*/false);
            nonNegValues_.insert(actualLen);
            emitStoreStringLen(actualLen, hdr);
            stringReturningFunctions_.insert("number_to_string");
            return hdr;
        }
        llvm::Value* maxLen = llvm::ConstantInt::get(getDefaultType(), 20);
        llvm::Value* hdr = emitAllocString(maxLen, maxLen, "numtostr");
        llvm::Value* bufData = emitStringData(hdr, "numtostr.data");
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 21);
        llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_fmt", true);
        if (!fmtStr)
            fmtStr = builder->CreateGlobalString("%lld", "tostr_fmt");
        llvm::Value* written =
            builder->CreateCall(getOrDeclareSnprintf(), {bufData, bufSize, fmtStr, val}, "numtostr.written");
        llvm::Value* actualLen = builder->CreateZExt(written, getDefaultType(), "numtostr.len", /*IsNonNeg=*/false);
        nonNegValues_.insert(actualLen);
        emitStoreStringLen(actualLen, hdr);
        stringReturningFunctions_.insert("number_to_string");
        return hdr;
    }

    if (bid == BuiltinId::STRING_TO_NUMBER) {
        validateArgCount(expr, "string_to_number", 1);
        // ── Compile-time fold: string_to_number(str_const) ──────────
        if (auto sv = tryFoldStr(expr->arguments[0].get())) {
            try {
                return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(std::stoll(*sv)));
            } catch (...) {
            } // NOLINT(bugprone-empty-catch)
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy() ? strArg : builder->CreateIntToPtr(strArg, ptrTy, "strtonum.ptr");
        // Use strtoll to parse as integer first
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* result = builder->CreateCall(
            getOrDeclareStrtoll(), {strPtr, nullPtr, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10)},
            "strtonum.result");
        return result;
    }

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
            wrapper = llvm::Function::Create(wrapperType, llvm::Function::InternalLinkage, wrapperName, module.get());
            auto* savedBB = builder->GetInsertBlock();
            auto* savedPoint =
                builder->GetInsertPoint() != builder->GetInsertBlock()->end() ? &*builder->GetInsertPoint() : nullptr;
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

        builder->CreateCall(getOrDeclarePthreadCreate(), {tidAlloca, nullAttr, wrapper, nullArg});

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
        static constexpr int64_t kMutexAllocSize = 64;
        llvm::Value* size = llvm::ConstantInt::get(getDefaultType(), kMutexAllocSize);
        llvm::Value* mutex = builder->CreateCall(getOrDeclareMalloc(), {size}, "mutex.ptr");
        llvm::cast<llvm::CallInst>(mutex)->addRetAttr(
            llvm::Attribute::getWithDereferenceableBytes(*context, kMutexAllocSize));
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nullAttr = llvm::ConstantPointerNull::get(ptrTy);
        builder->CreateCall(getOrDeclarePthreadMutexInit(), {mutex, nullAttr});
        // Return as i64
        return mutex;
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
        nonNegValues_.insert(result); // popcount is always in [0, 64]
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
        nonNegValues_.insert(result); // clz is always in [0, 64]
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
        nonNegValues_.insert(result); // ctz is always in [0, 64]
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
        // emitBoolZExt attaches !range [0,2), zext nneg, and nonNegValues_ tracking.
        return emitBoolZExt(result, "ispow2.ext");
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
                while (gb) {
                    uint64_t t = gb;
                    gb = ga % gb;
                    ga = t;
                }
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
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::Function* cttzFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::cttz, {getDefaultType()});

        llvm::Value* aOrB = builder->CreateOr(aAbs, bAbs, "lcm.aorb");
        llvm::Value* k = builder->CreateCall(cttzFn, {aOrB, builder->getFalse()}, "lcm.k");

        llvm::BasicBlock* mainBB = llvm::BasicBlock::Create(*context, "lcm.gcd.main", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "lcm.gcd.loop", function);
        llvm::BasicBlock* contBB = llvm::BasicBlock::Create(*context, "lcm.gcd.cont", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "lcm.gcd.done", function);

        // Edge case: if a == 0 or b == 0, lcm = 0.
        llvm::Value* edgeCase = builder->CreateOr(builder->CreateICmpEQ(aAbs, zero, "lcm.a0"),
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
        // hi = max(phiA, bOdd), lo = min(phiA, bOdd) via select: hi ≥ lo → nuw+nsw.
        llvm::Value* diff = builder->CreateSub(hi, lo, "lcm.diff",
                                               /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* gcdDone = builder->CreateICmpEQ(diff, zero, "lcm.dz");
        phiA->addIncoming(lo, loopBB);
        phiB->addIncoming(diff, loopBB);
        builder->CreateCondBr(gcdDone, contBB, loopBB);

        builder->SetInsertPoint(contBB);
        // lo is the GCD odd-part (positive), k = ctz(|a||b|) ≤ 62.
        // gcdShifted = lo * 2^k = gcd(a,b) ≤ min(|a|,|b|) ≤ INT64_MAX → nuw+nsw.
        llvm::Value* gcdShifted = builder->CreateShl(lo, k, "lcm.gcdval", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateBr(doneBB);

        // lcm(a, b) = |a| / gcd(a, b) * |b|  (divide first to avoid overflow)
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* gcdVal = builder->CreatePHI(getDefaultType(), 2, "lcm.gcd.result");
        gcdVal->addIncoming(zero, entryBB); // edge case: return 0
        gcdVal->addIncoming(gcdShifted, contBB);
        // Handle gcd == 0 (when both inputs are 0): lcm(0, 0) = 0
        llvm::Value* gcdIsZero = builder->CreateICmpEQ(gcdVal, zero, "lcm.gcd.iszero");
        llvm::Value* divResult = builder->CreateUDiv(aAbs, gcdVal, "lcm.div");
        llvm::Value* lcmResult = builder->CreateMul(divResult, bAbs, "lcm.mul", /*HasNUW=*/true, /*HasNSW=*/true);
        auto* lcmFinal = builder->CreateSelect(gcdIsZero, zero, lcmResult, "lcm.result");
        nonNegValues_.insert(lcmFinal); // lcm is always non-negative (uses abs of inputs)
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
                // Guard against shift-by-64 UB: when amt==0, the rotation is identity.
                uint64_t result = (amt == 0) ? v : (v << amt) | (v >> (64 - amt));
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
                // Guard against shift-by-64 UB: when amt==0, the rotation is identity.
                uint64_t result = (amt == 0) ? v : (v >> amt) | (v << (64 - amt));
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
                if (sum > INT64_MAX)
                    sum = INT64_MAX;
                if (sum < INT64_MIN)
                    sum = INT64_MIN;
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
                if (diff > INT64_MAX)
                    diff = INT64_MAX;
                if (diff < INT64_MIN)
                    diff = INT64_MIN;
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
        llvm::BasicBlock* deadBB = llvm::BasicBlock::Create(*context, "unreachable.dead", function);
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
        llvm::Function* expectFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::expect, {getDefaultType()});
        return builder->CreateCall(expectFn, {val, likelyVal}, "expect.result");
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_PRODUCT) {
        validateArgCount(expr, "array_product", 1);
        // Constant-fold array_product([c0, c1, ...]) when all elements are known.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                int64_t product = 1;
                bool allInt = true;
                for (const auto& elem : cv->arrVal) {
                    if (elem.kind != ConstValue::Kind::Integer) {
                        allInt = false;
                        break;
                    }
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
            arg->getType()->isPointerTy()
                ? arg
                : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "aprod.arrptr");
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
        acc->addIncoming(one, entryBB); // product identity is 1
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
        llvm::Value* newAcc = (inOptMaxFunction || optimizationLevel >= OptimizationLevel::O2)
                                  ? builder->CreateNSWMul(acc, elem, "aprod.newacc")
                                  : builder->CreateMul(acc, elem, "aprod.newacc");
        // Reuse offset (= idx+1, nsw+nuw) as the loop induction increment.
        // Eliminates a redundant add and provides SCEV with tight nsw+nuw flags.
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(offset, bodyBB);
        { attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB))); }

        builder->SetInsertPoint(doneBB);
        return acc;
    }

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
                    if (last.intVal >= 0)
                        nonNegValues_.insert(ci);
                    return ci;
                }
            }
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr =
            arrArg->getType()->isPointerTy()
                ? arrArg
                : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "alast.arrptr");
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
            std::string msg = expr->line > 0 ? std::string("Runtime error: array_last called on empty array at line ") +
                                                   std::to_string(expr->line) + "\n"
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
            lastLoad->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
        }
        return lastLoad;
    }

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
            arrArg->getType()->isPointerTy()
                ? arrArg
                : builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "ains.arrptr");
        llvm::Value* ainsLenLoad = emitLoadArrayLen(arrPtr, "ains.len");
        llvm::Value* arrLen = ainsLenLoad;

        // Bounds check: 0 <= idx <= length (insert at end is allowed).
        // ULE(idx, arrLen) ≡ (SGE(idx,0) && SLE(idx,arrLen)) since arrLen ≥ 0.
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* valid = builder->CreateICmpULE(idxArg, arrLen, "ains.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "ains.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "ains.fail", function);
        llvm::MDNode* ainsW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(valid, okBB, failBB, ainsW);

        builder->SetInsertPoint(failBB);
        {
            std::string msg = expr->line > 0 ? std::string("Runtime error: array_insert index out of bounds at line ") +
                                                   std::to_string(expr->line) + "\n"
                                             : "Runtime error: array_insert index out of bounds\n";
            builder->CreateCall(getPrintfFunction(), {builder->CreateGlobalString(msg, "ains_oob_msg")});
        }
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        llvm::Value* newLen = builder->CreateAdd(arrLen, one, "ains.newlen", /*HasNUW=*/true, /*HasNSW=*/true);
        // newLen = arrLen + 1 ≥ 1 since arrLen ≥ 0.
        nonNegValues_.insert(newLen);
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
        llvm::Value* postDstIdx = builder->CreateAdd(idxArg, llvm::ConstantInt::get(getDefaultType(), 2),
                                                     "ains.postdstidx", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* postDst = builder->CreateInBoundsGEP(getDefaultType(), buf, postDstIdx, "ains.postdst");
        llvm::Value* postCount = builder->CreateSub(arrLen, idxArg, "ains.postcnt", /*HasNUW=*/true, /*HasNSW=*/true);
        // postCount = arrLen - idx ≥ 0 since idx ≤ arrLen (ULE bounds check).
        nonNegValues_.insert(postCount);
        llvm::Value* postBytes =
            builder->CreateMul(postCount, eight, "ains.postbytes", /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemcpy(), {postDst, postSrc, postBytes});

        return buf;
    }

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
                                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
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
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strpad.strptr");
        llvm::Value* fillPtr =
            fillArg->getType()->isPointerTy()
                ? fillArg
                : builder->CreateIntToPtr(fillArg, llvm::PointerType::getUnqual(*context), "strpad.fillptr");

        // slen = strlen(str)
        llvm::Value* slen = emitStringLen(strPtr, "strpad.slen");

        // Clamp width to [0, i64_max] — negative width means no padding
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* negWidth = builder->CreateICmpSLT(widthArg, zero, "strpad.negw");
        llvm::Value* effectiveWidth = builder->CreateSelect(negWidth, zero, widthArg, "strpad.width");
        nonNegValues_.insert(effectiveWidth);

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
        llvm::Value* origI64 = strPtr;
        builder->CreateBr(mergeBB);

        // --- Pad path ---
        builder->SetInsertPoint(padBB);
        // Load fill character: first byte of fill string
        llvm::Value* fillByte = builder->CreateAlignedLoad(
            builder->getInt8Ty(), emitStringData(fillPtr, "strpad.fdata"), llvm::MaybeAlign(1), "strpad.fillbyte");
        llvm::cast<llvm::LoadInst>(fillByte)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);

        llvm::Value* padLen =
            builder->CreateSub(effectiveWidth, slen, "strpad.padlen", /*HasNUW=*/true, /*HasNSW=*/true);
        // In padBB: effectiveWidth > slen, so padLen ≥ 1.
        nonNegValues_.insert(padLen);
        // Allocate (effectiveWidth + 1) bytes for result
        llvm::Value* padHdr = emitAllocString(effectiveWidth, effectiveWidth, "strpad");
        llvm::Value* padData = emitStringData(padHdr, "strpad.data");

        if (bid == BuiltinId::STR_PAD_LEFT) {
            // Fill first padLen bytes with fill char
            builder->CreateMemSet(padData, fillByte, padLen, llvm::MaybeAlign(1));
            // Copy str (including null terminator) into padData + padLen
            llvm::Value* copyDst = builder->CreateInBoundsGEP(builder->getInt8Ty(), padData, padLen, "strpad.copydst");
            llvm::Value* copySize = builder->CreateAdd(slen, llvm::ConstantInt::get(getDefaultType(), 1),
                                                       "strpad.copysz", /*HasNUW=*/true, /*HasNSW=*/true);
            builder->CreateCall(getOrDeclareMemcpy(), {copyDst, emitStringData(strPtr, "strpad.sdata"), copySize});
        } else {
            // STR_PAD_RIGHT: copy str into padData, then fill remaining bytes with fill char
            builder->CreateCall(getOrDeclareMemcpy(), {padData, emitStringData(strPtr, "strpad.sdata2"), slen});
            llvm::Value* fillDst = builder->CreateInBoundsGEP(builder->getInt8Ty(), padData, slen, "strpad.filldst");
            builder->CreateMemSet(fillDst, fillByte, padLen, llvm::MaybeAlign(1));
            // Null-terminate
            llvm::Value* nullPtr =
                builder->CreateInBoundsGEP(builder->getInt8Ty(), padData, effectiveWidth, "strpad.nullptr");
            builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nullPtr)
                ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        }

        llvm::Value* padI64 = padHdr;
        builder->CreateBr(mergeBB);

        // --- Merge ---
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* result = builder->CreatePHI(llvm::PointerType::getUnqual(*context), 2, "strpad.result");
        result->addIncoming(origI64, noPadBB);
        result->addIncoming(padI64, padBB);
        stringReturningFunctions_.insert(fnName);
        return result;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::COMMAND) {
        validateArgCount(expr, "command", 1);
        llvm::Value* cmdArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* cmdPtr =
            cmdArg->getType()->isPointerTy() ? cmdArg : builder->CreateIntToPtr(cmdArg, ptrTy, "cmd.ptr");
        // cmdPtr points to the fat-string header {i64 len, i64 cap, i8[] data}.
        // popen/sh needs a plain C string (null-terminated), which starts at
        // offset 16 (after the two i64 header fields).
        llvm::Value* cmdData = emitStringData(cmdPtr, "cmd.data");

        // Declare popen
        auto* popenFn = llvm::dyn_cast_or_null<llvm::Function>(
            module->getOrInsertFunction("popen", llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false)).getCallee());
        if (popenFn) {
            popenFn->addFnAttr(llvm::Attribute::NoUnwind);
            OMSC_ADD_NOCAPTURE(popenFn, 0);
            OMSC_ADD_NOCAPTURE(popenFn, 1);
        }
        // Declare pclose
        auto* pcloseFn = llvm::dyn_cast_or_null<llvm::Function>(
            module
                ->getOrInsertFunction("pclose",
                                      llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false))
                .getCallee());
        if (pcloseFn)
            pcloseFn->addFnAttr(llvm::Attribute::NoUnwind);

        llvm::GlobalVariable* modeR = module->getGlobalVariable("__popen_mode_r", true);
        if (!modeR)
            modeR = builder->CreateGlobalString("r", "__popen_mode_r");

        llvm::Value* fp = builder->CreateCall(popenFn, {cmdData, modeR}, "cmd.fp");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* isNull = builder->CreateICmpEQ(fp, nullPtr, "cmd.isnull");

        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(*context, "cmd.null", parentFn);
        llvm::BasicBlock* readBB = llvm::BasicBlock::Create(*context, "cmd.read", parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "cmd.merge", parentFn);

        builder->CreateCondBr(isNull, nullBB, readBB, llvm::MDBuilder(*context).createBranchWeights(1, 100));

        // Null path → empty fat-string (len=0, NUL-terminated data)
        builder->SetInsertPoint(nullBB);
        llvm::Value* emptyZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* emptyHdr = emitAllocString(emptyZero, emptyZero, "cmd.empty");
        builder
            ->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0),
                          emitStringData(emptyHdr, "cmd.empty.data"))
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* nullEndBB = builder->GetInsertBlock();

        // Read path: read output with fgets into a growable buffer
        builder->SetInsertPoint(readBB);
        llvm::Value* initCap = llvm::ConstantInt::get(getDefaultType(), 4096);
        llvm::AllocaInst* capPtr = createEntryBlockAlloca(parentFn, "cmd.cap", getDefaultType());
        llvm::AllocaInst* sizePtr = createEntryBlockAlloca(parentFn, "cmd.size", getDefaultType());
        llvm::AllocaInst* bufPtr = createEntryBlockAlloca(parentFn, "cmd.bufp", ptrTy);
        builder->CreateStore(initCap, capPtr);
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), sizePtr);
        llvm::Value* initBuf = builder->CreateCall(getOrDeclareMalloc(), {initCap}, "cmd.buf");
        builder->CreateStore(initBuf, bufPtr);

        // chunk buffer: 256 bytes on the "heap" (small, reused each iteration)
        llvm::Value* chunkBuf =
            builder->CreateCall(getOrDeclareMalloc(), {llvm::ConstantInt::get(getDefaultType(), 256)}, "cmd.chunk");
        llvm::Value* chunkSize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 256);

        llvm::BasicBlock* rloopBB = llvm::BasicBlock::Create(*context, "cmd.rloop", parentFn);
        llvm::BasicBlock* appendBB = llvm::BasicBlock::Create(*context, "cmd.append", parentFn);
        llvm::BasicBlock* rdoneBB = llvm::BasicBlock::Create(*context, "cmd.rdone", parentFn);
        llvm::BasicBlock* growBB = llvm::BasicBlock::Create(*context, "cmd.grow", parentFn);
        llvm::BasicBlock* copyBB = llvm::BasicBlock::Create(*context, "cmd.copy", parentFn);

        builder->CreateBr(rloopBB);
        builder->SetInsertPoint(rloopBB);

        // fgets(chunk, 256, fp) → null on EOF/error
        llvm::Value* got = builder->CreateCall(getOrDeclareFgets(), {chunkBuf, chunkSize, fp}, "cmd.got");
        llvm::Value* gotNull = builder->CreateICmpEQ(got, nullPtr, "cmd.gotnull");
        builder->CreateCondBr(gotNull, rdoneBB, appendBB, llvm::MDBuilder(*context).createBranchWeights(1, 1000));

        builder->SetInsertPoint(appendBB);
        llvm::Value* chunkLen = builder->CreateCall(getOrDeclareStrlen(), {chunkBuf}, "cmd.clen");
        nonNegValues_.insert(chunkLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(chunkLen)->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* curSize = builder->CreateAlignedLoad(getDefaultType(), sizePtr, llvm::MaybeAlign(8), "cmd.csz");
        llvm::Value* curCap = builder->CreateAlignedLoad(getDefaultType(), capPtr, llvm::MaybeAlign(8), "cmd.ccap");
        llvm::Value* newSize = builder->CreateAdd(curSize, chunkLen, "cmd.nsz", true, true);
        // newSize = curSize + chunkLen (both non-negative); newSize + 1 can't wrap.
        llvm::Value* needOne = builder->CreateAdd(newSize, llvm::ConstantInt::get(getDefaultType(), 1), "cmd.ns1",
                                                  /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* needGrow = builder->CreateICmpUGT(needOne, curCap, "cmd.needgrow");
        builder->CreateCondBr(needGrow, growBB, copyBB);

        builder->SetInsertPoint(growBB);
        llvm::Value* newCap =
            builder->CreateMul(curCap, llvm::ConstantInt::get(getDefaultType(), 2), "cmd.ncap", true, true);
        llvm::Value* curBufG = builder->CreateAlignedLoad(ptrTy, bufPtr, llvm::MaybeAlign(8), "cmd.cbufg");
        llvm::Value* newBuf = builder->CreateCall(getOrDeclareRealloc(), {curBufG, newCap}, "cmd.nbuf");
        builder->CreateStore(newBuf, bufPtr);
        builder->CreateStore(newCap, capPtr);
        builder->CreateBr(copyBB);

        builder->SetInsertPoint(copyBB);
        llvm::Value* curBufC = builder->CreateAlignedLoad(ptrTy, bufPtr, llvm::MaybeAlign(8), "cmd.cbufc");
        llvm::Value* dst = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), curBufC, curSize, "cmd.dst");
        builder->CreateCall(getOrDeclareMemcpy(), {dst, chunkBuf, chunkLen});
        builder->CreateStore(newSize, sizePtr);
        builder->CreateBr(rloopBB);

        builder->SetInsertPoint(rdoneBB);
        builder->CreateCall(pcloseFn, {fp});
        // Free the temporary chunk buffer now that reading is complete.
        builder->CreateCall(getOrDeclareFree(), {chunkBuf});
        llvm::Value* finalSz = builder->CreateAlignedLoad(getDefaultType(), sizePtr, llvm::MaybeAlign(8), "cmd.fsz");
        llvm::Value* finalBuf = builder->CreateAlignedLoad(ptrTy, bufPtr, llvm::MaybeAlign(8), "cmd.fbuf");
        // NUL-terminate the raw data buffer.
        llvm::Value* ntPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), finalBuf, finalSz, "cmd.nt");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), ntPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        // Wrap the raw buffer into a fat-string header {len, cap, data}.
        // The raw buffer holds only the character data; allocate a proper
        // fat-string, copy the data, and free the temporary raw buffer.
        llvm::Value* readHdr = emitAllocString(finalSz, finalSz, "cmd.rhdr");
        builder->CreateCall(getOrDeclareMemcpy(),
                            {emitStringData(readHdr, "cmd.rhdrdata"), finalBuf,
                             builder->CreateAdd(finalSz, llvm::ConstantInt::get(getDefaultType(), 1), "cmd.cplen",
                                                /*nuw=*/true, /*nsw=*/true)});
        builder->CreateCall(getOrDeclareFree(), {finalBuf});
        llvm::Value* readResult = readHdr;
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* readEndBB = builder->GetInsertBlock();

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* cmdPhi = builder->CreatePHI(llvm::PointerType::getUnqual(*context), 2, "cmd.phi");
        cmdPhi->addIncoming(emptyHdr, nullEndBB);
        cmdPhi->addIncoming(readResult, readEndBB);
        stringReturningFunctions_.insert("command");
        stringReturningFunctions_.insert("shell");
        return cmdPhi;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_FILTER) {
        validateArgCount(expr, "str_filter", 2);
        const std::string fnName = extractFnName(expr->arguments[1].get());
        if (fnName.empty())
            codegenError("str_filter: second argument must be a function name (string literal or identifier)", expr);
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second)
            codegenError("str_filter: unknown function '" + fnName + "'", expr);
        llvm::Function* predFn = calleeIt->second;

        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "sfilt.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "sfilt.len");

        // Allocate output fat-ptr: max capacity = strLen
        llvm::Value* outHdr = emitAllocString(strLen, strLen, "sfilt");
        llvm::Value* outBuf = emitStringData(outHdr, "sfilt.out");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "sfilt.loop", parentFn);
        llvm::BasicBlock* testBB = llvm::BasicBlock::Create(*context, "sfilt.test", parentFn);
        llvm::BasicBlock* addBB = llvm::BasicBlock::Create(*context, "sfilt.add", parentFn);
        llvm::BasicBlock* incBB = llvm::BasicBlock::Create(*context, "sfilt.inc", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "sfilt.done", parentFn);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "sfilt.idx");
        llvm::PHINode* outIdx = builder->CreatePHI(getDefaultType(), 2, "sfilt.outidx");
        idx->addIncoming(zero, preheader);
        outIdx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpULT(idx, strLen, "sfilt.cond");
        builder->CreateCondBr(cond, testBB, doneBB);

        builder->SetInsertPoint(testBB);
        // Load char as i8, zero-extend to i64 for predicate
        llvm::Value* charPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context),
                                                          emitStringData(strPtr, "sfilt.data"), idx, "sfilt.charptr");
        llvm::Value* ch8 =
            builder->CreateAlignedLoad(llvm::Type::getInt8Ty(*context), charPtr, llvm::MaybeAlign(1), "sfilt.ch8");
        llvm::cast<llvm::Instruction>(ch8)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* ch64 = builder->CreateZExt(ch8, getDefaultType(), "sfilt.ch64",
                                                /*IsNonNeg=*/false);
        nonNegValues_.insert(ch64);
        // Call predicate
        llvm::Value* keep =
            builder->CreateICmpNE(builder->CreateCall(predFn, {ch64}, "sfilt.keep_val"), zero, "sfilt.keep");
        builder->CreateCondBr(keep, addBB, incBB);

        builder->SetInsertPoint(addBB);
        llvm::Value* dstPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), outBuf, outIdx, "sfilt.dstptr");
        builder->CreateStore(ch8, dstPtr)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* newOutIdx =
            builder->CreateAdd(outIdx, llvm::ConstantInt::get(getDefaultType(), 1), "sfilt.newout", true, true);
        builder->CreateBr(incBB);

        builder->SetInsertPoint(incBB);
        llvm::PHINode* outMerge = builder->CreatePHI(getDefaultType(), 2, "sfilt.outmerge");
        outMerge->addIncoming(outIdx, testBB);
        outMerge->addIncoming(newOutIdx, addBB);
        llvm::Value* nextIdx =
            builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 1), "sfilt.next", true, true);
        idx->addIncoming(nextIdx, incBB);
        outIdx->addIncoming(outMerge, incBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

        builder->SetInsertPoint(doneBB);
        // NUL-terminate at outIdx and update len
        llvm::Value* nullCharPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), outBuf, outIdx, "sfilt.nullptr");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nullCharPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        emitStoreStringLen(outIdx, outHdr);
        stringReturningFunctions_.insert("str_filter");
        return outHdr;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::MAP_FILTER) {
        validateArgCount(expr, "map_filter", 2);
        const std::string mfFnName = extractFnName(expr->arguments[1].get());
        if (mfFnName.empty())
            codegenError("map_filter: second argument must be a function name (string literal or identifier)", expr);
        auto mfCalleeIt = functions.find(mfFnName);
        if (mfCalleeIt == functions.end() || !mfCalleeIt->second)
            codegenError("map_filter: unknown function '" + mfFnName + "'", expr);
        llvm::Function* mfPredFn = mfCalleeIt->second;

        llvm::Value* mfSrcMap = generateExpression(expr->arguments[0].get());
        llvm::Value* mfMapPtr = emitToArrayPtr(mfSrcMap, "mf.mapptr");
        auto* mfPtrTy = llvm::PointerType::getUnqual(*context);

        // Read cap and count from map header [cap:i64, count:i64, buckets...]
        // Each bucket = [hash:i64, key:i64, val:i64]
        llvm::Value* mfCap = builder->CreateAlignedLoad(getDefaultType(), mfMapPtr, llvm::MaybeAlign(8), "mf.cap");
        llvm::Value* mfZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* mfOne = llvm::ConstantInt::get(getDefaultType(), 1);

        // Create a new empty map for the result using MAP_NEW builtin
        auto mfNewCall = std::make_unique<CallExpr>("map_new", std::vector<std::unique_ptr<Expression>>{});
        mfNewCall->fromStdNamespace = true; // codegen-generated
        mfNewCall->line = expr->line;
        mfNewCall->column = expr->column;
        llvm::Value* mfNewMap = generateCall(mfNewCall.get());

        // Store the new map in an alloca so MAP_SET can update it
        llvm::Function* mfParentFn = builder->GetInsertBlock()->getParent();
        llvm::AllocaInst* mfNewMapA = createEntryBlockAlloca(mfParentFn, "mf.newmap", getDefaultType());
        builder->CreateStore(mfNewMap, mfNewMapA);

        // Loop over buckets
        llvm::BasicBlock* mfPre = builder->GetInsertBlock();
        llvm::BasicBlock* mfLoop = llvm::BasicBlock::Create(*context, "mf.loop", mfParentFn);
        llvm::BasicBlock* mfTest = llvm::BasicBlock::Create(*context, "mf.test", mfParentFn);
        llvm::BasicBlock* mfAdd = llvm::BasicBlock::Create(*context, "mf.add", mfParentFn);
        llvm::BasicBlock* mfInc = llvm::BasicBlock::Create(*context, "mf.inc", mfParentFn);
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
        llvm::Value* mfHash = builder->CreateAlignedLoad(
            getDefaultType(), builder->CreateInBoundsGEP(getDefaultType(), mfMapPtr, mfBoff, "mf.hashp"),
            llvm::MaybeAlign(8), "mf.hash");
        llvm::Value* mfOcc = builder->CreateICmpNE(mfHash, mfZero, "mf.occ");
        builder->CreateCondBr(mfOcc, mfAdd, mfInc);

        builder->SetInsertPoint(mfAdd);
        llvm::Value* mfKeyOff = builder->CreateAdd(mfBoff, mfOne, "mf.koff", true, true);
        llvm::Value* mfKey = builder->CreateAlignedLoad(
            getDefaultType(), builder->CreateInBoundsGEP(getDefaultType(), mfMapPtr, mfKeyOff, "mf.keyp"),
            llvm::MaybeAlign(8), "mf.key");
        llvm::Value* mfValOff =
            builder->CreateAdd(mfBoff, llvm::ConstantInt::get(getDefaultType(), 2), "mf.voff", true, true);
        llvm::Value* mfVal = builder->CreateAlignedLoad(
            getDefaultType(), builder->CreateInBoundsGEP(getDefaultType(), mfMapPtr, mfValOff, "mf.valp"),
            llvm::MaybeAlign(8), "mf.val");
        // Call predicate with key i64
        llvm::Value* mfPredR = builder->CreateCall(mfPredFn, {mfKey}, "mf.predr");
        llvm::Value* mfKeep = builder->CreateICmpNE(
            builder->CreateIntCast(mfPredR, getDefaultType(), false, "mf.pcast"), mfZero, "mf.keep");
        llvm::BasicBlock* mfInsert = llvm::BasicBlock::Create(*context, "mf.insert", mfParentFn);
        builder->CreateCondBr(mfKeep, mfInsert, mfInc);

        builder->SetInsertPoint(mfInsert);
        // Store key and val in allocas, then call map_set via existing codegen.
        auto* mfSetFt =
            llvm::FunctionType::get(getDefaultType(), {getDefaultType(), getDefaultType(), getDefaultType()}, false);
        auto* mfSetHelperFn = llvm::dyn_cast_or_null<llvm::Function>(
            module->getOrInsertFunction("__omsc_map_filter_set_helper", mfSetFt).getCallee());
        if (mfSetHelperFn && mfSetHelperFn->empty()) {
            // Declare as external — the linker will resolve against the OmScript
            mfSetHelperFn->setLinkage(llvm::GlobalValue::PrivateLinkage);
            mfSetHelperFn->addFnAttr(llvm::Attribute::AlwaysInline);
            // Build body: just call into map_set using the passed map i64
            llvm::BasicBlock* mfHBB = llvm::BasicBlock::Create(*context, "entry", mfSetHelperFn);
            llvm::IRBuilder<> hb(mfHBB);
            // Return first arg unchanged (stub — caller already updates newMapA)
            hb.CreateRet(mfSetHelperFn->arg_begin());
        }
        // Actually, use the existing map_set builtin IR path.
        llvm::Value* mfNewMapVal =
            builder->CreateAlignedLoad(getDefaultType(), mfNewMapA, llvm::MaybeAlign(8), "mf.newmapval");
        llvm::Value* mfNewMapPtr = builder->CreateIntToPtr(mfNewMapVal, mfPtrTy, "mf.newmapptr");
        llvm::Value* mfNewCap =
            builder->CreateAlignedLoad(getDefaultType(), mfNewMapPtr, llvm::MaybeAlign(8), "mf.newcap");
        // Compute hash of key string (djb2: hash = 5381; for each byte: hash = hash*33 ^ c)
        // We emit this as a small inline loop.
        llvm::Value* mfKeyPtr = builder->CreateIntToPtr(mfKey, mfPtrTy, "mf.keyptr");
        llvm::Value* mfHashInit = llvm::ConstantInt::get(getDefaultType(), 5381);
        llvm::BasicBlock* mfHashPre = builder->GetInsertBlock();
        llvm::BasicBlock* mfHashLoop = llvm::BasicBlock::Create(*context, "mf.hashloop", mfParentFn);
        llvm::BasicBlock* mfHashDone = llvm::BasicBlock::Create(*context, "mf.hashdone", mfParentFn);
        builder->CreateBr(mfHashLoop);
        builder->SetInsertPoint(mfHashLoop);
        llvm::PHINode* mfHI = builder->CreatePHI(getDefaultType(), 2, "mf.hi");
        llvm::PHINode* mfHHash = builder->CreatePHI(getDefaultType(), 2, "mf.hh");
        mfHI->addIncoming(mfZero, mfHashPre);
        mfHHash->addIncoming(mfHashInit, mfHashPre);
        llvm::Value* mfCp = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), mfKeyPtr, mfHI, "mf.cp");
        llvm::Value* mfCh8 =
            builder->CreateAlignedLoad(llvm::Type::getInt8Ty(*context), mfCp, llvm::MaybeAlign(1), "mf.ch");
        llvm::cast<llvm::LoadInst>(mfCh8)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* mfCh = builder->CreateZExt(mfCh8, getDefaultType(), "mf.ch64",
                                                /*IsNonNeg=*/false);
        nonNegValues_.insert(mfCh);
        llvm::Value* mfEnd = builder->CreateICmpEQ(mfCh, mfZero, "mf.end");
        builder->CreateCondBr(mfEnd, mfHashDone, mfHashLoop);
        // Back edge: hash = hash*33 ^ c
        llvm::Value* mfHH2 = builder->CreateXor(
            builder->CreateMul(mfHHash, llvm::ConstantInt::get(getDefaultType(), 33), "mf.h33"), mfCh, "mf.hxor");
        llvm::Value* mfHI2 = builder->CreateAdd(mfHI, mfOne, "mf.hi1", true, true);
        mfHI->addIncoming(mfHI2, mfHashLoop);
        mfHHash->addIncoming(mfHH2, mfHashLoop);
        builder->SetInsertPoint(mfHashDone);
        // Finalise hash: ensure non-zero (same as OmScript map)
        llvm::Value* mfH0 = builder->CreateICmpEQ(mfHHash, mfZero, "mf.h0");
        llvm::Value* mfHashFinal =
            builder->CreateSelect(mfH0, llvm::ConstantInt::get(getDefaultType(), 1), mfHHash, "mf.hashfinal");
        // Probe new map: slot = hash % cap; linear probe for empty slot
        llvm::Value* mfSlot = builder->CreateURem(mfHashFinal, mfNewCap, "mf.slot");
        llvm::BasicBlock* mfProbePre = builder->GetInsertBlock();
        llvm::BasicBlock* mfProbeLoop = llvm::BasicBlock::Create(*context, "mf.probe", mfParentFn);
        llvm::BasicBlock* mfWriteBB = llvm::BasicBlock::Create(*context, "mf.write", mfParentFn);
        builder->CreateBr(mfProbeLoop);
        builder->SetInsertPoint(mfProbeLoop);
        llvm::PHINode* mfSI = builder->CreatePHI(getDefaultType(), 2, "mf.si");
        mfSI->addIncoming(mfSlot, mfProbePre);
        llvm::Value* mfBOff2 =
            builder->CreateAdd(builder->CreateMul(mfSI, llvm::ConstantInt::get(getDefaultType(), 3), "mf.s3"),
                               llvm::ConstantInt::get(getDefaultType(), 2), "mf.soff");
        llvm::Value* mfSlotHash = builder->CreateAlignedLoad(
            getDefaultType(), builder->CreateInBoundsGEP(getDefaultType(), mfNewMapPtr, mfBOff2, "mf.shp"),
            llvm::MaybeAlign(8), "mf.shash");
        llvm::Value* mfEmpty = builder->CreateICmpEQ(mfSlotHash, mfZero, "mf.empty");
        builder->CreateCondBr(mfEmpty, mfWriteBB, mfProbeLoop);
        // Advance slot (linear probe)
        llvm::Value* mfSI1 = builder->CreateURem(builder->CreateAdd(mfSI, mfOne, "mf.si1a"), mfNewCap, "mf.sinext");
        mfSI->addIncoming(mfSI1, mfProbeLoop);

        builder->SetInsertPoint(mfWriteBB);
        // Write hash, key, val
        auto mfStore = [&](llvm::Value* base, llvm::Value* off, llvm::Value* val) {
            builder->CreateAlignedStore(val, builder->CreateInBoundsGEP(getDefaultType(), base, off, "mf.wp"),
                                        llvm::MaybeAlign(8));
        };
        mfStore(mfNewMapPtr, mfBOff2, mfHashFinal);
        mfStore(mfNewMapPtr, builder->CreateAdd(mfBOff2, mfOne, "mf.koff2"), mfKey);
        mfStore(mfNewMapPtr, builder->CreateAdd(mfBOff2, llvm::ConstantInt::get(getDefaultType(), 2), "mf.voff2"),
                mfVal);
        // Increment count (at offset 1)
        llvm::Value* mfCountPtr = builder->CreateInBoundsGEP(getDefaultType(), mfNewMapPtr, mfOne, "mf.cntp");
        llvm::Value* mfOldCount =
            builder->CreateAlignedLoad(getDefaultType(), mfCountPtr, llvm::MaybeAlign(8), "mf.ocnt");
        builder->CreateAlignedStore(builder->CreateAdd(mfOldCount, mfOne, "mf.ncnt"), mfCountPtr, llvm::MaybeAlign(8));
        builder->CreateBr(mfInc);

        builder->SetInsertPoint(mfInc);
        llvm::Value* mfBi1 = builder->CreateAdd(mfBi, mfOne, "mf.bi1", true, true);
        mfBi->addIncoming(mfBi1, mfInc);
        // Back-edge from mfAdd (not-kept path) and mfWriteBB (kept path)
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(mfLoop)));

        builder->SetInsertPoint(mfDone);
        return builder->CreateAlignedLoad(getDefaultType(), mfNewMapA, llvm::MaybeAlign(8), "mf.result");
    }


    // -----------------------------------------------------------------------
    if (bid == BuiltinId::FILTER) {
        if (expr->arguments.size() != 2)
            codegenError("filter: expected 2 arguments (collection, predicate_fn_name)", expr);
        Expression* collArg = expr->arguments[0].get();
        // Determine collection type via the same static analysis used elsewhere.
        if (isStringExpr(collArg)) {
            // String: delegate to str_filter
            auto synth = std::make_unique<CallExpr>("str_filter", std::vector<std::unique_ptr<Expression>>{});
            synth->fromStdNamespace = true; // codegen-generated
            synth->arguments.push_back(std::move(expr->arguments[0]));
            synth->arguments.push_back(std::move(expr->arguments[1]));
            synth->line = expr->line;
            synth->column = expr->column;
            return generateCall(synth.get());
        } else {
            // Default: array filter
            auto synth = std::make_unique<CallExpr>("array_filter", std::vector<std::unique_ptr<Expression>>{});
            synth->fromStdNamespace = true; // codegen-generated
            synth->arguments.push_back(std::move(expr->arguments[0]));
            synth->arguments.push_back(std::move(expr->arguments[1]));
            synth->line = expr->line;
            synth->column = expr->column;
            return generateCall(synth.get());
        }
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_LSTRIP) {
        validateArgCount(expr, "str_lstrip", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "lstrip.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "lstrip.len");

        llvm::Function* lsParentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* lsPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* lsLoopBB = llvm::BasicBlock::Create(*context, "lstrip.loop", lsParentFn);
        llvm::BasicBlock* lsBodyBB = llvm::BasicBlock::Create(*context, "lstrip.body", lsParentFn);
        llvm::BasicBlock* lsContBB = llvm::BasicBlock::Create(*context, "lstrip.cont", lsParentFn);
        llvm::BasicBlock* lsDoneBB = llvm::BasicBlock::Create(*context, "lstrip.done", lsParentFn);
        llvm::Value* lsZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* lsOne = llvm::ConstantInt::get(getDefaultType(), 1);
        // Compute the data pointer before the loop so it dominates all uses
        // in lsBodyBB (char load) and lsDoneBB (memcpy src).
        llvm::Value* lsStrData = emitStringData(strPtr, "lstrip.data");
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(lsLoopBB)));

        builder->SetInsertPoint(lsLoopBB);
        llvm::PHINode* lsIdx = builder->CreatePHI(getDefaultType(), 2, "lstrip.idx");
        lsIdx->addIncoming(lsZero, lsPreBB);
        builder->CreateCondBr(builder->CreateICmpULT(lsIdx, strLen, "lstrip.cond"), lsBodyBB, lsDoneBB);

        builder->SetInsertPoint(lsBodyBB);
        llvm::Value* lsCharPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), lsStrData, lsIdx, "lstrip.cp");
        auto* lsCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), lsCharPtr, "lstrip.ch");
        lsCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* lsCh32 = builder->CreateZExt(lsCharLoad, llvm::Type::getInt32Ty(*context), "lstrip.ch32",
                                                  /*IsNonNeg=*/false);
        nonNegValues_.insert(lsCh32);
        llvm::Value* lsIsSp = builder->CreateCall(getOrDeclareIsspace(), {lsCh32}, "lstrip.issp");
        builder->CreateCondBr(builder->CreateICmpNE(lsIsSp, builder->getInt32(0), "lstrip.spcond"), lsContBB, lsDoneBB);

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
        llvm::Value* lsHdr = emitAllocString(lsResultLen, lsResultLen, "lstrip");
        llvm::Value* lsDst = emitStringData(lsHdr, "lstrip.dst");
        llvm::Value* lsSrc =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), lsStrData, lsStart, "lstrip.src");
        builder->CreateCall(getOrDeclareMemcpy(), {lsDst, lsSrc, lsResultLen});
        llvm::Value* lsNulPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), lsDst, lsResultLen, "lstrip.nul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), lsNulPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        stringReturningFunctions_.insert("str_lstrip");
        return lsHdr;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_RSTRIP) {
        validateArgCount(expr, "str_rstrip", 1);
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "rstrip.ptr");
        llvm::Value* strLen = emitStringLen(strPtr, "rstrip.len");

        llvm::Function* rsParentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* rsPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* rsLoopBB = llvm::BasicBlock::Create(*context, "rstrip.loop", rsParentFn);
        llvm::BasicBlock* rsBodyBB = llvm::BasicBlock::Create(*context, "rstrip.body", rsParentFn);
        llvm::BasicBlock* rsContBB = llvm::BasicBlock::Create(*context, "rstrip.cont", rsParentFn);
        llvm::BasicBlock* rsDoneBB = llvm::BasicBlock::Create(*context, "rstrip.done", rsParentFn);
        llvm::Value* rsZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* rsOne = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(rsLoopBB)));

        builder->SetInsertPoint(rsLoopBB);
        llvm::PHINode* rsEnd = builder->CreatePHI(getDefaultType(), 2, "rstrip.end");
        rsEnd->addIncoming(strLen, rsPreBB);
        builder->CreateCondBr(builder->CreateICmpUGT(rsEnd, rsZero, "rstrip.cond"), rsBodyBB, rsDoneBB);

        builder->SetInsertPoint(rsBodyBB);
        llvm::Value* rsPrev = builder->CreateSub(rsEnd, rsOne, "rstrip.prev", true, true);
        llvm::Value* rsStrData = emitStringData(strPtr, "rstrip.data");
        llvm::Value* rsCharPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), rsStrData, rsPrev, "rstrip.cp");
        auto* rsCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), rsCharPtr, "rstrip.ch");
        rsCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* rsCh32 = builder->CreateZExt(rsCharLoad, llvm::Type::getInt32Ty(*context), "rstrip.ch32",
                                                  /*IsNonNeg=*/false);
        nonNegValues_.insert(rsCh32);
        llvm::Value* rsIsSp = builder->CreateCall(getOrDeclareIsspace(), {rsCh32}, "rstrip.issp");
        builder->CreateCondBr(builder->CreateICmpNE(rsIsSp, builder->getInt32(0), "rstrip.spcond"), rsContBB, rsDoneBB);

        builder->SetInsertPoint(rsContBB);
        rsEnd->addIncoming(rsPrev, rsContBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(rsLoopBB)));

        builder->SetInsertPoint(rsDoneBB);
        llvm::PHINode* rsFinalEnd = builder->CreatePHI(getDefaultType(), 3, "rstrip.finalend");
        rsFinalEnd->addIncoming(rsEnd, rsLoopBB);
        rsFinalEnd->addIncoming(rsEnd, rsBodyBB);
        // Build result: strPtr[0..rsFinalEnd)
        llvm::Value* rsHdr = emitAllocString(rsFinalEnd, rsFinalEnd, "rstrip");
        llvm::Value* rsDst = emitStringData(rsHdr, "rstrip.dst");
        builder->CreateCall(getOrDeclareMemcpy(), {rsDst, emitStringData(strPtr, "rstrip.sdata"), rsFinalEnd});
        llvm::Value* rsNulPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), rsDst, rsFinalEnd, "rstrip.nul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), rsNulPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        stringReturningFunctions_.insert("str_rstrip");
        return rsHdr;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::STR_REMOVE) {
        validateArgCount(expr, "str_remove", 2);
        // Synthesize str_replace(s, sub, "")
        auto srEmptyLit = std::make_unique<LiteralExpr>(std::string(""));
        srEmptyLit->line = expr->line;
        srEmptyLit->column = expr->column;
        auto srSynth = std::make_unique<CallExpr>("str_replace", std::vector<std::unique_ptr<Expression>>{});
        srSynth->fromStdNamespace = true; // codegen-generated
        srSynth->arguments.push_back(std::move(expr->arguments[0]));
        srSynth->arguments.push_back(std::move(expr->arguments[1]));
        srSynth->arguments.push_back(std::move(srEmptyLit));
        srSynth->line = expr->line;
        srSynth->column = expr->column;
        llvm::Value* srResult = generateCall(srSynth.get());
        stringReturningFunctions_.insert("str_remove");
        return srResult;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_TAKE) {
        validateArgCount(expr, "array_take", 2);
        auto atZeroLit = std::make_unique<LiteralExpr>(0LL);
        atZeroLit->line = expr->line;
        atZeroLit->column = expr->column;
        auto atSynth = std::make_unique<CallExpr>("array_slice", std::vector<std::unique_ptr<Expression>>{});
        atSynth->fromStdNamespace = true; // codegen-generated
        atSynth->arguments.push_back(std::move(expr->arguments[0]));
        atSynth->arguments.push_back(std::move(atZeroLit));
        atSynth->arguments.push_back(std::move(expr->arguments[1]));
        atSynth->line = expr->line;
        atSynth->column = expr->column;
        return generateCall(atSynth.get());
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_DROP) {
        validateArgCount(expr, "array_drop", 2);
        // We need len(arr) as the end argument.  Evaluate arr first, store in alloca.
        llvm::Value* adArr = generateExpression(expr->arguments[0].get());
        adArr = toDefaultType(adArr);
        llvm::Value* adN = generateExpression(expr->arguments[1].get());
        adN = toDefaultType(adN);
        auto* adPtrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* adArrPtr = builder->CreateIntToPtr(adArr, adPtrTy, "adrop.arrptr");
        llvm::Value* adLenLoad = emitLoadArrayLen(adArrPtr, "adrop.len");
        // Clamp n: max(0, min(n, len))
        llvm::Value* adZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* adNNeg = builder->CreateICmpSLT(adN, adZero, "adrop.nneg");
        llvm::Value* adNClamp = builder->CreateSelect(adNNeg, adZero, adN, "adrop.nclamp");
        llvm::Value* adNOver = builder->CreateICmpSGT(adNClamp, adLenLoad, "adrop.nover");
        llvm::Value* adStart = builder->CreateSelect(adNOver, adLenLoad, adNClamp, "adrop.start");
        llvm::Value* adSliceLen = builder->CreateSub(adLenLoad, adStart, "adrop.slen", true, true);
        // Allocate result
        llvm::Value* adOne = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* adEight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* adSlots = builder->CreateAdd(adSliceLen, adOne, "adrop.slots", true, true);
        llvm::Value* adBytes = builder->CreateMul(adSlots, adEight, "adrop.bytes", true, true);
        llvm::Value* adBuf = builder->CreateCall(getOrDeclareMalloc(), {adBytes}, "adrop.buf");
        llvm::cast<llvm::CallInst>(adBuf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        emitStoreArrayLen(adSliceLen, adBuf);
        llvm::Value* adSrcIdx = builder->CreateAdd(adStart, adOne, "adrop.srcidx", true, true);
        llvm::Value* adSrc = builder->CreateInBoundsGEP(getDefaultType(), adArrPtr, adSrcIdx, "adrop.src");
        llvm::Value* adDst = builder->CreateInBoundsGEP(getDefaultType(), adBuf, adOne, "adrop.dst");
        llvm::Value* adCpSz = builder->CreateMul(adSliceLen, adEight, "adrop.cpsz", true, true);
        builder->CreateCall(getOrDeclareMemcpy(), {adDst, adSrc, adCpSz});
        return adBuf;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_UNIQUE) {
        validateArgCount(expr, "array_unique", 1);
        llvm::Value* auArr = generateExpression(expr->arguments[0].get());
        llvm::Value* auArrPtr = emitToArrayPtr(auArr, "auniq.arrptr");
        llvm::Value* auLenLoad = emitLoadArrayLen(auArrPtr, "auniq.len");
        llvm::Value* auZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* auOne = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* auEight = llvm::ConstantInt::get(getDefaultType(), 8);
        // Allocate output buffer same size as input (worst case: all unique)
        llvm::Value* auSlots = builder->CreateAdd(auLenLoad, auOne, "auniq.slots", true, true);
        llvm::Value* auBytes = builder->CreateMul(auSlots, auEight, "auniq.bytes", true, true);
        llvm::Value* auBuf = builder->CreateCall(getOrDeclareMalloc(), {auBytes}, "auniq.buf");
        llvm::cast<llvm::CallInst>(auBuf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        // outLen alloca (count of unique elements written)
        llvm::Function* auParentFn = builder->GetInsertBlock()->getParent();
        llvm::AllocaInst* auOutLenA = createEntryBlockAlloca(auParentFn, "auniq.outlen", getDefaultType());
        builder->CreateStore(auZero, auOutLenA);

        // Loop: for i in 0..arrLen { if i==0 or arr[i]!=arr[i-1]: write }
        llvm::BasicBlock* auPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* auLoopBB = llvm::BasicBlock::Create(*context, "auniq.loop", auParentFn);
        llvm::BasicBlock* auBodyBB = llvm::BasicBlock::Create(*context, "auniq.body", auParentFn);
        llvm::BasicBlock* auDedBB = llvm::BasicBlock::Create(*context, "auniq.ded", auParentFn);
        llvm::BasicBlock* auIncBB = llvm::BasicBlock::Create(*context, "auniq.inc", auParentFn);
        llvm::BasicBlock* auDoneBB = llvm::BasicBlock::Create(*context, "auniq.done", auParentFn);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(auLoopBB)));

        builder->SetInsertPoint(auLoopBB);
        llvm::PHINode* auI = builder->CreatePHI(getDefaultType(), 2, "auniq.i");
        auI->addIncoming(auZero, auPreBB);
        builder->CreateCondBr(builder->CreateICmpULT(auI, auLenLoad, "auniq.cond"), auBodyBB, auDoneBB);

        builder->SetInsertPoint(auBodyBB);
        llvm::Value* auElemIdx = builder->CreateAdd(auI, auOne, "auniq.ei", true, true);
        llvm::Value* auElemPtr = builder->CreateInBoundsGEP(getDefaultType(), auArrPtr, auElemIdx, "auniq.ep");
        llvm::Value* auElemLoad = emitLoadArrayElem(auElemPtr, "auniq.elem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(auElemLoad)
                ->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
        // Check: is this the first element (i==0) or different from the previous?
        llvm::Value* auIsFirst = builder->CreateICmpEQ(auI, auZero, "auniq.isfirst");
        llvm::Value* auPrevIdx = builder->CreateSub(auI, auOne, "auniq.prevei");
        // auPrevIdx is only valid when i>0; PHI select doesn't evaluate
        llvm::Value* auPrevElemIdx = builder->CreateAdd(auPrevIdx, auOne, "auniq.pei", true, true);
        llvm::Value* auPrevElemPtr = builder->CreateInBoundsGEP(getDefaultType(), auArrPtr, auPrevElemIdx, "auniq.pep");
        llvm::Value* auPrevLoad = emitLoadArrayElem(auPrevElemPtr, "auniq.prev");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(auPrevLoad)
                ->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
        llvm::Value* auDiff = builder->CreateICmpNE(auElemLoad, auPrevLoad, "auniq.diff");
        llvm::Value* auKeep = builder->CreateOr(auIsFirst, auDiff, "auniq.keep");
        builder->CreateCondBr(auKeep, auDedBB, auIncBB);

        builder->SetInsertPoint(auDedBB);
        llvm::Value* auOutLen =
            builder->CreateAlignedLoad(getDefaultType(), auOutLenA, llvm::MaybeAlign(8), "auniq.ol");
        llvm::Value* auDstIdx = builder->CreateAdd(auOutLen, auOne, "auniq.di", true, true);
        llvm::Value* auDstPtr = builder->CreateInBoundsGEP(getDefaultType(), auBuf, auDstIdx, "auniq.dp");
        emitStoreArrayElem(auElemLoad, auDstPtr);
        builder->CreateStore(builder->CreateAdd(auOutLen, auOne, "auniq.ol1", true, true), auOutLenA);
        builder->CreateBr(auIncBB);

        builder->SetInsertPoint(auIncBB);
        llvm::Value* auI1 = builder->CreateAdd(auI, auOne, "auniq.i1", true, true);
        auI->addIncoming(auI1, auIncBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(auLoopBB)));

        builder->SetInsertPoint(auDoneBB);
        llvm::Value* auFinalLen =
            builder->CreateAlignedLoad(getDefaultType(), auOutLenA, llvm::MaybeAlign(8), "auniq.fl");
        emitStoreArrayLen(auFinalLen, auBuf);
        return auBuf;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_ROTATE) {
        validateArgCount(expr, "array_rotate", 2);
        llvm::Value* arArr = generateExpression(expr->arguments[0].get());
        llvm::Value* arN = generateExpression(expr->arguments[1].get());
        arArr = toDefaultType(arArr);
        arN = toDefaultType(arN);
        auto* arPtrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* arArrPtr = builder->CreateIntToPtr(arArr, arPtrTy, "arot.arrptr");
        llvm::Value* arLenLoad = emitLoadArrayLen(arArrPtr, "arot.len");
        llvm::Value* arZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* arOne = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* arEight = llvm::ConstantInt::get(getDefaultType(), 8);
        // Allocate result (same size)
        llvm::Value* arSlots = builder->CreateAdd(arLenLoad, arOne, "arot.slots", true, true);
        llvm::Value* arBytes = builder->CreateMul(arSlots, arEight, "arot.bytes", true, true);
        llvm::Value* arBuf = builder->CreateCall(getOrDeclareMalloc(), {arBytes}, "arot.buf");
        llvm::cast<llvm::CallInst>(arBuf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));
        emitStoreArrayLen(arLenLoad, arBuf);
        // Handle empty array
        llvm::Function* arParentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* arDoBB = llvm::BasicBlock::Create(*context, "arot.do", arParentFn);
        llvm::BasicBlock* arLoopBB = llvm::BasicBlock::Create(*context, "arot.loop", arParentFn);
        llvm::BasicBlock* arBodyBB = llvm::BasicBlock::Create(*context, "arot.body", arParentFn);
        llvm::BasicBlock* arDoneBB = llvm::BasicBlock::Create(*context, "arot.done", arParentFn);
        llvm::Value* arEmpty = builder->CreateICmpEQ(arLenLoad, arZero, "arot.empty");
        builder->CreateCondBr(arEmpty, arDoneBB, arDoBB);

        builder->SetInsertPoint(arDoBB);
        // Normalize shift: k = ((n % len) + len) % len
        llvm::Value* arMod1 = builder->CreateSRem(arN, arLenLoad, "arot.mod1");
        llvm::Value* arMod2 = builder->CreateAdd(arMod1, arLenLoad, "arot.mod2");
        llvm::Value* arK = builder->CreateSRem(arMod2, arLenLoad, "arot.k");
        nonNegValues_.insert(arK);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(arLoopBB)));

        builder->SetInsertPoint(arLoopBB);
        llvm::PHINode* arI = builder->CreatePHI(getDefaultType(), 2, "arot.i");
        arI->addIncoming(arZero, arDoBB);
        builder->CreateCondBr(builder->CreateICmpULT(arI, arLenLoad, "arot.cond"), arBodyBB, arDoneBB);

        builder->SetInsertPoint(arBodyBB);
        // src index = (i + k) % len
        // arI < arLenLoad and arK < arLenLoad, so arI+arK < 2*arLenLoad ≤ INT64_MAX → nuw+nsw.
        llvm::Value* arSrcI = builder->CreateURem(
            builder->CreateAdd(arI, arK, "arot.ik", /*HasNUW=*/true, /*HasNSW=*/true), arLenLoad, "arot.srci");
        llvm::Value* arSrcIdx = builder->CreateAdd(arSrcI, arOne, "arot.srcidx", true, true);
        llvm::Value* arSrcPtr = builder->CreateInBoundsGEP(getDefaultType(), arArrPtr, arSrcIdx, "arot.sp");
        llvm::Value* arSrcLoad = emitLoadArrayElem(arSrcPtr, "arot.sv");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(arSrcLoad)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                                  llvm::MDNode::get(*context, {}));
        llvm::Value* arDstIdx = builder->CreateAdd(arI, arOne, "arot.dstidx", true, true);
        llvm::Value* arDstPtr = builder->CreateInBoundsGEP(getDefaultType(), arBuf, arDstIdx, "arot.dp");
        emitStoreArrayElem(arSrcLoad, arDstPtr);
        llvm::Value* arI1 = builder->CreateAdd(arI, arOne, "arot.i1", true, true);
        arI->addIncoming(arI1, arBodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(arLoopBB)));

        builder->SetInsertPoint(arDoneBB);
        return arBuf;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ARRAY_MEAN) {
        validateArgCount(expr, "array_mean", 1);
        // Constant-fold array_mean([c0, c1, ...]) when the array is a compile-time literal.
        if (auto cv = tryFoldExprToConst(expr->arguments[0].get())) {
            if (cv->kind == ConstValue::Kind::Array) {
                bool allInt = true;
                for (const auto& elem : cv->arrVal) {
                    if (elem.kind != ConstValue::Kind::Integer) {
                        allInt = false;
                        break;
                    }
                }
                if (allInt) {
                    optStats_.constFolded++;
                    if (cv->arrVal.empty())
                        return llvm::ConstantInt::get(getDefaultType(), 0);
                    int64_t total = 0;
                    for (const auto& elem : cv->arrVal)
                        total += elem.intVal;
                    return llvm::ConstantInt::get(getDefaultType(), total / static_cast<int64_t>(cv->arrVal.size()));
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
        llvm::Value* amZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* amOne = llvm::ConstantInt::get(getDefaultType(), 1);
        // If empty, return 0
        llvm::Function* amParentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* amPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* amLoopBB = llvm::BasicBlock::Create(*context, "amean.loop", amParentFn);
        llvm::BasicBlock* amBodyBB = llvm::BasicBlock::Create(*context, "amean.body", amParentFn);
        llvm::BasicBlock* amDoneBB = llvm::BasicBlock::Create(*context, "amean.done", amParentFn);
        llvm::AllocaInst* amSumA = createEntryBlockAlloca(amParentFn, "amean.sum", getDefaultType());
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
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(amElemLoad)
                ->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
        llvm::Value* amOldSum = builder->CreateAlignedLoad(getDefaultType(), amSumA, llvm::MaybeAlign(8), "amean.os");
        builder->CreateStore(builder->CreateAdd(amOldSum, amElemLoad, "amean.ns"), amSumA);
        llvm::Value* amI1 = builder->CreateAdd(amI, amOne, "amean.i1", true, true);
        amI->addIncoming(amI1, amBodyBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(amLoopBB)));

        builder->SetInsertPoint(amDoneBB);
        llvm::Value* amFinalSum = builder->CreateAlignedLoad(getDefaultType(), amSumA, llvm::MaybeAlign(8), "amean.fs");
        // Return sum / len (sdiv), or 0 when len == 0
        llvm::Value* amIsEmpty = builder->CreateICmpEQ(amLenLoad, amZero, "amean.isempty");
        llvm::Value* amDiv = builder->CreateSDiv(amFinalSum, amLenLoad, "amean.div");
        return builder->CreateSelect(amIsEmpty, amZero, amDiv, "amean.result");
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::MAP_MERGE) {
        validateArgCount(expr, "map_merge", 2);
        llvm::Value* mmA = generateExpression(expr->arguments[0].get());
        llvm::Value* mmB = generateExpression(expr->arguments[1].get());
        mmA = toDefaultType(mmA);
        mmB = toDefaultType(mmB);
        auto* mmPtrTy = llvm::PointerType::getUnqual(*context);
        // Create new result map (copy of a)
        llvm::Value* mmAPtr = builder->CreateIntToPtr(mmA, mmPtrTy, "mmerge.aptr");
        llvm::Value* mmBPtr = builder->CreateIntToPtr(mmB, mmPtrTy, "mmerge.bptr");
        // result = map_new(); then insert all of a, then all of b (b wins)
        llvm::Value* mmRes = builder->CreateCall(getOrEmitHashMapNew(), {}, "mmerge.res");
        llvm::AllocaInst* mmResA =
            createEntryBlockAlloca(builder->GetInsertBlock()->getParent(), "mmerge.resmap", mmPtrTy);
        builder->CreateStore(mmRes, mmResA);

        // Helper lambda: iterate a map's buckets and insert into result
        auto mmInsertAll = [&](llvm::Value* srcMapPtr, const char* pfx) {
            // Read cap from offset 0
            llvm::Value* mmCap = builder->CreateAlignedLoad(getDefaultType(), srcMapPtr, llvm::MaybeAlign(8),
                                                            (std::string(pfx) + ".cap").c_str());
            llvm::Value* mmZero = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* mmOne = llvm::ConstantInt::get(getDefaultType(), 1);
            llvm::Function* mmFn = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock* mmPreBB = builder->GetInsertBlock();
            llvm::BasicBlock* mmLoopBB = llvm::BasicBlock::Create(*context, (std::string(pfx) + ".loop").c_str(), mmFn);
            llvm::BasicBlock* mmTestBB = llvm::BasicBlock::Create(*context, (std::string(pfx) + ".test").c_str(), mmFn);
            llvm::BasicBlock* mmInsB = llvm::BasicBlock::Create(*context, (std::string(pfx) + ".ins").c_str(), mmFn);
            llvm::BasicBlock* mmIncBB = llvm::BasicBlock::Create(*context, (std::string(pfx) + ".inc").c_str(), mmFn);
            llvm::BasicBlock* mmDoneBB = llvm::BasicBlock::Create(*context, (std::string(pfx) + ".done").c_str(), mmFn);
            attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(mmLoopBB)));
            builder->SetInsertPoint(mmLoopBB);
            llvm::PHINode* mmBi = builder->CreatePHI(getDefaultType(), 2, (std::string(pfx) + ".bi").c_str());
            mmBi->addIncoming(mmZero, mmPreBB);
            builder->CreateCondBr(builder->CreateICmpULT(mmBi, mmCap), mmTestBB, mmDoneBB);
            builder->SetInsertPoint(mmTestBB);
            // bucket offset = 2 + bi*3  (nuw+nsw: bi >= 0 and small relative to capacity)
            llvm::Value* mmBoff =
                builder->CreateAdd(builder->CreateMul(mmBi, llvm::ConstantInt::get(getDefaultType(), 3), "",
                                                      /*HasNUW=*/true, /*HasNSW=*/true),
                                   llvm::ConstantInt::get(getDefaultType(), 2), "",
                                   /*HasNUW=*/true, /*HasNSW=*/true);
            auto* mmHashV = builder->CreateAlignedLoad(
                getDefaultType(), builder->CreateInBoundsGEP(getDefaultType(), srcMapPtr, mmBoff), llvm::MaybeAlign(8));
            mmHashV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
            llvm::Value* mmOcc = builder->CreateICmpNE(mmHashV, mmZero);
            builder->CreateCondBr(mmOcc, mmInsB, mmIncBB);
            builder->SetInsertPoint(mmInsB);
            auto* mmKeyV = builder->CreateAlignedLoad(
                getDefaultType(),
                builder->CreateInBoundsGEP(getDefaultType(), srcMapPtr,
                                           builder->CreateAdd(mmBoff, mmOne, "", /*HasNUW=*/true, /*HasNSW=*/true)),
                llvm::MaybeAlign(8));
            mmKeyV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
            auto* mmValV = builder->CreateAlignedLoad(
                getDefaultType(),
                builder->CreateInBoundsGEP(getDefaultType(), srcMapPtr,
                                           builder->CreateAdd(mmBoff, llvm::ConstantInt::get(getDefaultType(), 2), "",
                                                              /*HasNUW=*/true, /*HasNSW=*/true)),
                llvm::MaybeAlign(8));
            mmValV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
            llvm::Value* mmCurRes = builder->CreateAlignedLoad(mmPtrTy, mmResA, llvm::MaybeAlign(8));
            llvm::Value* mmNewRes = builder->CreateCall(getOrEmitHashMapSet(), {mmCurRes, mmKeyV, mmValV});
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
        return mmFinalRes;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::MAP_INVERT) {
        validateArgCount(expr, "map_invert", 1);
        llvm::Value* miM = generateExpression(expr->arguments[0].get());
        llvm::Value* miMPtr = emitToArrayPtr(miM, "minv.mptr");
        auto* miPtrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* miCap = builder->CreateAlignedLoad(getDefaultType(), miMPtr, llvm::MaybeAlign(8), "minv.cap");
        llvm::Value* miZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* miOne = llvm::ConstantInt::get(getDefaultType(), 1);
        // Create result map
        llvm::Value* miRes = builder->CreateCall(getOrEmitHashMapNew(), {}, "minv.res");
        llvm::AllocaInst* miResA =
            createEntryBlockAlloca(builder->GetInsertBlock()->getParent(), "minv.resmap", miPtrTy);
        builder->CreateStore(miRes, miResA);

        llvm::Function* miFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* miPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* miLoopBB = llvm::BasicBlock::Create(*context, "minv.loop", miFn);
        llvm::BasicBlock* miTestBB = llvm::BasicBlock::Create(*context, "minv.test", miFn);
        llvm::BasicBlock* miInsB = llvm::BasicBlock::Create(*context, "minv.ins", miFn);
        llvm::BasicBlock* miIncBB = llvm::BasicBlock::Create(*context, "minv.inc", miFn);
        llvm::BasicBlock* miDoneBB = llvm::BasicBlock::Create(*context, "minv.done", miFn);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(miLoopBB)));

        builder->SetInsertPoint(miLoopBB);
        llvm::PHINode* miBi = builder->CreatePHI(getDefaultType(), 2, "minv.bi");
        miBi->addIncoming(miZero, miPreBB);
        builder->CreateCondBr(builder->CreateICmpULT(miBi, miCap), miTestBB, miDoneBB);

        builder->SetInsertPoint(miTestBB);
        llvm::Value* miBoff = builder->CreateAdd(builder->CreateMul(miBi, llvm::ConstantInt::get(getDefaultType(), 3),
                                                                    "minv.bof3", /*HasNUW=*/true, /*HasNSW=*/true),
                                                 llvm::ConstantInt::get(getDefaultType(), 2), "minv.boff",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
        auto* miHashV = builder->CreateAlignedLoad(
            getDefaultType(), builder->CreateInBoundsGEP(getDefaultType(), miMPtr, miBoff), llvm::MaybeAlign(8));
        miHashV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
        builder->CreateCondBr(builder->CreateICmpNE(miHashV, miZero), miInsB, miIncBB);

        builder->SetInsertPoint(miInsB);
        auto* miKeyV =
            builder->CreateAlignedLoad(getDefaultType(),
                                       builder->CreateInBoundsGEP(getDefaultType(), miMPtr,
                                                                  builder->CreateAdd(miBoff, miOne, "minv.bkey",
                                                                                     /*HasNUW=*/true, /*HasNSW=*/true)),
                                       llvm::MaybeAlign(8));
        miKeyV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
        auto* miValV = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), miMPtr,
                                       builder->CreateAdd(miBoff, llvm::ConstantInt::get(getDefaultType(), 2),
                                                          "minv.bval", /*HasNUW=*/true, /*HasNSW=*/true)),
            llvm::MaybeAlign(8));
        miValV->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
        // Invert: new key = old value, new value = old key
        llvm::Value* miCurRes = builder->CreateAlignedLoad(miPtrTy, miResA, llvm::MaybeAlign(8));
        llvm::Value* miNewRes = builder->CreateCall(getOrEmitHashMapSet(), {miCurRes, miValV, miKeyV});
        builder->CreateStore(miNewRes, miResA);
        builder->CreateBr(miIncBB);

        builder->SetInsertPoint(miIncBB);
        llvm::Value* miBi1 = builder->CreateAdd(miBi, miOne, "minv.bi1", true, true);
        miBi->addIncoming(miBi1, miIncBB);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(miLoopBB)));

        builder->SetInsertPoint(miDoneBB);
        llvm::Value* miFinalRes = builder->CreateAlignedLoad(miPtrTy, miResA, llvm::MaybeAlign(8), "minv.final");
        return miFinalRes;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::SUDO_COMMAND) {
        validateArgCount(expr, "sudo_command", 2);
        llvm::Value* scCmdArg = generateExpression(expr->arguments[0].get());
        llvm::Value* scPassArg = generateExpression(expr->arguments[1].get());
        auto* scPtrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* scCmdPtr =
            scCmdArg->getType()->isPointerTy() ? scCmdArg : builder->CreateIntToPtr(scCmdArg, scPtrTy, "sudo.cmdptr");
        llvm::Value* scPassPtr = scPassArg->getType()->isPointerTy()
                                     ? scPassArg
                                     : builder->CreateIntToPtr(scPassArg, scPtrTy, "sudo.passptr");

        llvm::Function* scParentFn = builder->GetInsertBlock()->getParent();

        // ---- Step 1: escape single-quotes in the password ----
        // Escaped form: each `'` → `'\''`.  Worst case: 4x expansion.
        llvm::Value* scPassLen = emitStringLen(scPassPtr, "sudo.passlen");
        llvm::Value* scEscMax = builder->CreateAdd(
            builder->CreateMul(scPassLen, llvm::ConstantInt::get(getDefaultType(), 4), "sudo.escmax4",
                               /*HasNUW=*/true, /*HasNSW=*/true),
            llvm::ConstantInt::get(getDefaultType(), 1), "sudo.escmax", true, true);
        llvm::Value* scEscBuf = builder->CreateCall(getOrDeclareMalloc(), {scEscMax}, "sudo.escbuf");
        llvm::AllocaInst* scEscWrA = createEntryBlockAlloca(scParentFn, "sudo.escwr", getDefaultType());
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), scEscWrA);

        llvm::BasicBlock* scEscPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* scEscLoopBB = llvm::BasicBlock::Create(*context, "sudo.escloop", scParentFn);
        llvm::BasicBlock* scEscBodyBB = llvm::BasicBlock::Create(*context, "sudo.escbody", scParentFn);
        llvm::BasicBlock* scEscSqBB = llvm::BasicBlock::Create(*context, "sudo.escsq", scParentFn);
        llvm::BasicBlock* scEscNormBB = llvm::BasicBlock::Create(*context, "sudo.escnorm", scParentFn);
        llvm::BasicBlock* scEscIncBB = llvm::BasicBlock::Create(*context, "sudo.escinc", scParentFn);
        llvm::BasicBlock* scEscDoneBB = llvm::BasicBlock::Create(*context, "sudo.escdone", scParentFn);
        llvm::Value* scEscZero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* scEscOne = llvm::ConstantInt::get(getDefaultType(), 1);
        attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(scEscLoopBB)));

        builder->SetInsertPoint(scEscLoopBB);
        llvm::PHINode* scEscI = builder->CreatePHI(getDefaultType(), 2, "sudo.esci");
        scEscI->addIncoming(scEscZero, scEscPreBB);
        builder->CreateCondBr(builder->CreateICmpULT(scEscI, scPassLen, "sudo.esccond"), scEscBodyBB, scEscDoneBB);

        builder->SetInsertPoint(scEscBodyBB);
        llvm::Value* scPassData = emitStringData(scPassPtr, "sudo.passdata");
        llvm::Value* scCharPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), scPassData, scEscI, "sudo.charptr");
        auto* scCharLoad = builder->CreateLoad(llvm::Type::getInt8Ty(*context), scCharPtr, "sudo.char");
        scCharLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* scIsSq = builder->CreateICmpEQ(
            scCharLoad, llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), '\''), "sudo.issq");
        builder->CreateCondBr(scIsSq, scEscSqBB, scEscNormBB);

        // Single-quote path: emit  ' \ ' '  (4 bytes)
        builder->SetInsertPoint(scEscSqBB);
        llvm::Value* scWrSq = builder->CreateAlignedLoad(getDefaultType(), scEscWrA, llvm::MaybeAlign(8), "sudo.wrsq");
        auto scEmitByte = [&](llvm::Value* wrIdx, uint8_t byte) -> llvm::Value* {
            llvm::Value* p = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), scEscBuf, wrIdx, "sudo.ep");
            builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), byte), p);
            return builder->CreateAdd(wrIdx, scEscOne, "sudo.wr1", true, true);
        };
        llvm::Value* scWrSq1 = scEmitByte(scWrSq, '\'');
        llvm::Value* scWrSq2 = scEmitByte(scWrSq1, '\\');
        llvm::Value* scWrSq3 = scEmitByte(scWrSq2, '\'');
        llvm::Value* scWrSq4 = scEmitByte(scWrSq3, '\'');
        builder->CreateStore(scWrSq4, scEscWrA);
        builder->CreateBr(scEscIncBB);

        // Normal path: emit byte as-is
        builder->SetInsertPoint(scEscNormBB);
        llvm::Value* scWrNorm =
            builder->CreateAlignedLoad(getDefaultType(), scEscWrA, llvm::MaybeAlign(8), "sudo.wrnorm");
        {
            llvm::Value* scNP =
                builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), scEscBuf, scWrNorm, "sudo.np");
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
        llvm::Value* scEscWrFinal =
            builder->CreateAlignedLoad(getDefaultType(), scEscWrA, llvm::MaybeAlign(8), "sudo.escfin");
        llvm::Value* scEscNulPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), scEscBuf, scEscWrFinal, "sudo.escnul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), scEscNulPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);

        // ---- Step 2: build full pipeline command string ----
        llvm::Value* scCmdLen = emitStringLen(scCmdPtr, "sudo.cmdlen");
        // prefix: "printf '%s\n' '" = 16 chars, suffix: "' | sudo -S -- sh -c '" = 22 chars,
        // cmd_suffix: "' 2>&1" = 6 chars, NUL = 1
        llvm::Value* scFmtFixed = llvm::ConstantInt::get(getDefaultType(), 16 + 22 + 6 + 1);
        llvm::Value* scFmtLen = builder->CreateAdd(
            builder->CreateAdd(scFmtFixed, scEscWrFinal, "sudo.flen1", true, true), scCmdLen, "sudo.flen", true, true);
        llvm::Value* scFmtBuf = builder->CreateCall(getOrDeclareMalloc(), {scFmtLen}, "sudo.fmtbuf");

        // Use snprintf to build the command string
        llvm::GlobalVariable* scFmtStr = module->getGlobalVariable("__sudo_fmt", true);
        if (!scFmtStr)
            scFmtStr = builder->CreateGlobalString("printf '%%s\\n' '%s' | sudo -S -- sh -c '%s' 2>&1", "__sudo_fmt");
        builder->CreateCall(getOrDeclareSnprintf(),
                            {scFmtBuf, scFmtLen, scFmtStr, scEscBuf, emitStringData(scCmdPtr, "sudo.cmddata")});

        // ---- Step 3: popen + read loop (identical to COMMAND) ----
        auto* scPopenFn = llvm::dyn_cast_or_null<llvm::Function>(
            module->getOrInsertFunction("popen", llvm::FunctionType::get(scPtrTy, {scPtrTy, scPtrTy}, false))
                .getCallee());
        if (scPopenFn) {
            scPopenFn->addFnAttr(llvm::Attribute::NoUnwind);
            OMSC_ADD_NOCAPTURE(scPopenFn, 0);
            OMSC_ADD_NOCAPTURE(scPopenFn, 1);
        }
        auto* scPcloseFn = llvm::dyn_cast_or_null<llvm::Function>(
            module
                ->getOrInsertFunction("pclose",
                                      llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {scPtrTy}, false))
                .getCallee());
        if (scPcloseFn)
            scPcloseFn->addFnAttr(llvm::Attribute::NoUnwind);

        llvm::GlobalVariable* scModeR = module->getGlobalVariable("__popen_mode_r", true);
        if (!scModeR)
            scModeR = builder->CreateGlobalString("r", "__popen_mode_r");

        llvm::Value* scFp = builder->CreateCall(scPopenFn, {scFmtBuf, scModeR}, "sudo.fp");
        llvm::Value* scNullPtr = llvm::ConstantPointerNull::get(scPtrTy);
        llvm::Value* scIsNull = builder->CreateICmpEQ(scFp, scNullPtr, "sudo.isnull");

        llvm::BasicBlock* scNullBB = llvm::BasicBlock::Create(*context, "sudo.null", scParentFn);
        llvm::BasicBlock* scReadBB = llvm::BasicBlock::Create(*context, "sudo.read", scParentFn);
        llvm::BasicBlock* scMergeBB = llvm::BasicBlock::Create(*context, "sudo.merge", scParentFn);

        builder->CreateCondBr(scIsNull, scNullBB, scReadBB, llvm::MDBuilder(*context).createBranchWeights(1, 100));

        // Null path → empty string
        builder->SetInsertPoint(scNullBB);
        llvm::Value* scEmptyBuf =
            builder->CreateCall(getOrDeclareMalloc(), {llvm::ConstantInt::get(getDefaultType(), 1)}, "sudo.empty");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), scEmptyBuf)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* scEmptyI64 = scEmptyBuf;
        builder->CreateBr(scMergeBB);
        llvm::BasicBlock* scNullEndBB = builder->GetInsertBlock();

        // Read path: growing buffer with fgets
        builder->SetInsertPoint(scReadBB);
        llvm::Value* scInitCap = llvm::ConstantInt::get(getDefaultType(), 4096);
        llvm::AllocaInst* scCapPtr = createEntryBlockAlloca(scParentFn, "sudo.cap", getDefaultType());
        llvm::AllocaInst* scSizePtr = createEntryBlockAlloca(scParentFn, "sudo.size", getDefaultType());
        llvm::AllocaInst* scBufPtr = createEntryBlockAlloca(scParentFn, "sudo.bufp", scPtrTy);
        builder->CreateStore(scInitCap, scCapPtr);
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), scSizePtr);
        llvm::Value* scInitBuf = builder->CreateCall(getOrDeclareMalloc(), {scInitCap}, "sudo.buf");
        builder->CreateStore(scInitBuf, scBufPtr);

        llvm::Value* scChunkBuf =
            builder->CreateCall(getOrDeclareMalloc(), {llvm::ConstantInt::get(getDefaultType(), 256)}, "sudo.chunk");
        llvm::Value* scChunkSize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 256);

        llvm::BasicBlock* scRLoopBB = llvm::BasicBlock::Create(*context, "sudo.rloop", scParentFn);
        llvm::BasicBlock* scAppendBB = llvm::BasicBlock::Create(*context, "sudo.append", scParentFn);
        llvm::BasicBlock* scRDoneBB = llvm::BasicBlock::Create(*context, "sudo.rdone", scParentFn);
        llvm::BasicBlock* scGrowBB = llvm::BasicBlock::Create(*context, "sudo.grow", scParentFn);
        llvm::BasicBlock* scCopyBB = llvm::BasicBlock::Create(*context, "sudo.copy", scParentFn);

        builder->CreateBr(scRLoopBB);
        builder->SetInsertPoint(scRLoopBB);

        llvm::Value* scGot = builder->CreateCall(getOrDeclareFgets(), {scChunkBuf, scChunkSize, scFp}, "sudo.got");
        llvm::Value* scGotNull = builder->CreateICmpEQ(scGot, scNullPtr, "sudo.gotnull");
        builder->CreateCondBr(scGotNull, scRDoneBB, scAppendBB, llvm::MDBuilder(*context).createBranchWeights(1, 1000));

        builder->SetInsertPoint(scAppendBB);
        llvm::Value* scChunkLen = builder->CreateCall(getOrDeclareStrlen(), {scChunkBuf}, "sudo.clen");
        nonNegValues_.insert(scChunkLen);
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(scChunkLen)->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        llvm::Value* scCurSize =
            builder->CreateAlignedLoad(getDefaultType(), scSizePtr, llvm::MaybeAlign(8), "sudo.csz");
        llvm::Value* scCurCap =
            builder->CreateAlignedLoad(getDefaultType(), scCapPtr, llvm::MaybeAlign(8), "sudo.ccap");
        llvm::Value* scNewSize = builder->CreateAdd(scCurSize, scChunkLen, "sudo.nsz", true, true);
        llvm::Value* scNeedOne =
            builder->CreateAdd(scNewSize, llvm::ConstantInt::get(getDefaultType(), 1), "sudo.ns1", true, true);
        llvm::Value* scNeedGrow = builder->CreateICmpUGT(scNeedOne, scCurCap, "sudo.needgrow");
        builder->CreateCondBr(scNeedGrow, scGrowBB, scCopyBB);

        builder->SetInsertPoint(scGrowBB);
        llvm::Value* scNewCap =
            builder->CreateMul(scCurCap, llvm::ConstantInt::get(getDefaultType(), 2), "sudo.ncap", true, true);
        llvm::Value* scCurBufG = builder->CreateAlignedLoad(scPtrTy, scBufPtr, llvm::MaybeAlign(8), "sudo.cbufg");
        llvm::Value* scNewBuf = builder->CreateCall(getOrDeclareRealloc(), {scCurBufG, scNewCap}, "sudo.nbuf");
        builder->CreateStore(scNewBuf, scBufPtr);
        builder->CreateStore(scNewCap, scCapPtr);
        builder->CreateBr(scCopyBB);

        builder->SetInsertPoint(scCopyBB);
        llvm::Value* scCurBufC = builder->CreateAlignedLoad(scPtrTy, scBufPtr, llvm::MaybeAlign(8), "sudo.cbufc");
        llvm::Value* scDst =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), scCurBufC, scCurSize, "sudo.dst");
        builder->CreateCall(getOrDeclareMemcpy(), {scDst, scChunkBuf, scChunkLen});
        builder->CreateStore(scNewSize, scSizePtr);
        builder->CreateBr(scRLoopBB);

        builder->SetInsertPoint(scRDoneBB);
        builder->CreateCall(scPcloseFn, {scFp});
        // Free the temporary chunk buffer now that reading is complete.
        builder->CreateCall(getOrDeclareFree(), {scChunkBuf});
        llvm::Value* scFinalSz =
            builder->CreateAlignedLoad(getDefaultType(), scSizePtr, llvm::MaybeAlign(8), "sudo.fsz");
        llvm::Value* scFinalBuf = builder->CreateAlignedLoad(scPtrTy, scBufPtr, llvm::MaybeAlign(8), "sudo.fbuf");
        llvm::Value* scNtPtr =
            builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), scFinalBuf, scFinalSz, "sudo.nt");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), scNtPtr)
            ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        llvm::Value* scReadResult = scFinalBuf;
        builder->CreateBr(scMergeBB);
        llvm::BasicBlock* scReadEndBB = builder->GetInsertBlock();

        builder->SetInsertPoint(scMergeBB);
        llvm::PHINode* scPhi = builder->CreatePHI(llvm::PointerType::getUnqual(*context), 2, "sudo.phi");
        scPhi->addIncoming(scEmptyI64, scNullEndBB);
        scPhi->addIncoming(scReadResult, scReadEndBB);
        stringReturningFunctions_.insert("sudo_command");
        return scPhi;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ENV_GET) {
        validateArgCount(expr, "env_get", 1);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nameArg = generateExpression(expr->arguments[0].get());
        // nameArg may be an i64 pointer-as-int or already a ptr
        llvm::Value* namePtr =
            nameArg->getType()->isPointerTy() ? nameArg : builder->CreateIntToPtr(nameArg, ptrTy, "env_get.name");
        // namePtr is a fat-string; extract the C-string data at offset 16.
        llvm::Value* nameData = emitStringData(namePtr, "env_get.namedata");
        // Call getenv(nameData) — returns char* or NULL
        llvm::Value* envPtr = builder->CreateCall(getOrDeclareGetenv(), {nameData}, "env_get.res");
        // If NULL, return empty fat-string; otherwise wrap the C-string in a fat-string
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(*context, "env_get.null", parentFn);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "env_get.ok", parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "env_get.merge", parentFn);
        llvm::Value* isNull = builder->CreateICmpEQ(envPtr, llvm::ConstantPointerNull::get(ptrTy), "env_get.isnull");
        // Env variable not set is somewhat uncommon — favour the non-null path.
        llvm::MDNode* egW = llvm::MDBuilder(*context).createBranchWeights(1, 99);
        builder->CreateCondBr(isNull, nullBB, okBB, egW);

        // Null path: return empty fat-string
        builder->SetInsertPoint(nullBB);
        llvm::Value* egEmptyHdr = nullptr;
        {
            llvm::Value* z64 = llvm::ConstantInt::get(getDefaultType(), 0);
            egEmptyHdr = emitAllocString(z64, z64, "env_get.empty");
            builder
                ->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0),
                              emitStringData(egEmptyHdr, "env_get.empty.data"))
                ->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        }
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* nullEndBB = builder->GetInsertBlock();

        // OK path: getenv returned a raw C-string; wrap it in a fat-string.
        builder->SetInsertPoint(okBB);
        llvm::Value* egOkLen = builder->CreateCall(getOrDeclareStrlen(), {envPtr}, "env_get.oklen");
        nonNegValues_.insert(egOkLen);
        llvm::Value* egOkHdr = emitAllocString(egOkLen, egOkLen, "env_get.okhdr");
        llvm::Value* egOkData = emitStringData(egOkHdr, "env_get.okdata");
        llvm::Value* egCpLen = builder->CreateAdd(egOkLen, llvm::ConstantInt::get(getDefaultType(), 1), "env_get.cplen",
                                                  /*HasNUW=*/true, /*HasNSW=*/true);
        builder->CreateCall(getOrDeclareMemcpy(), {egOkData, envPtr, egCpLen});
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* okEndBB = builder->GetInsertBlock();

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(llvm::PointerType::getUnqual(*context), 2, "env_get.phi");
        phi->addIncoming(egEmptyHdr, nullEndBB);
        phi->addIncoming(egOkHdr, okEndBB);
        stringReturningFunctions_.insert("env_get");
        return phi;
    }

    // -----------------------------------------------------------------------
    if (bid == BuiltinId::ENV_SET) {
        validateArgCount(expr, "env_set", 2);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nameArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        llvm::Value* namePtr =
            nameArg->getType()->isPointerTy() ? nameArg : builder->CreateIntToPtr(nameArg, ptrTy, "env_set.name");
        llvm::Value* valPtr =
            valArg->getType()->isPointerTy() ? valArg : builder->CreateIntToPtr(valArg, ptrTy, "env_set.val");
        // Both are fat-strings; extract the C-string data at offset 16.
        llvm::Value* nameDataEs = emitStringData(namePtr, "env_set.namedata");
        llvm::Value* valDataEs = emitStringData(valPtr, "env_set.valdata");
        // setenv(nameData, valData, 1) — overwrite = 1
        llvm::Value* overwrite = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
        llvm::Value* rc = builder->CreateCall(getOrDeclareSetenv(), {nameDataEs, valDataEs, overwrite}, "env_set.rc");
        // setenv returns 0 on success, -1 on failure; convert to 1/0
        llvm::Value* success =
            builder->CreateICmpEQ(rc, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "env_set.ok");
        return emitBoolZExt(success, "env_set.res");
    }

    // ── str_format(fmt, val1[, val2[, val3[, val4]]]) ──────────────────────
    if (bid == BuiltinId::STR_FORMAT) {
        const size_t nArgs = expr->arguments.size();
        if (nArgs < 2 || nArgs > 5)
            codegenError("str_format requires 2 to 5 arguments (fmt + 1 to 4 values)", expr);

        // ── Compile-time fold ───────────────────────────────────────
        if (auto fmtConst = tryFoldStr(expr->arguments[0].get())) {
            bool allConst = true;
            std::vector<ConstValue> constArgs;
            for (size_t i = 1; i < nArgs; ++i) {
                if (auto cv = tryFoldExprToConst(expr->arguments[i].get())) {
                    if (cv->kind != ConstValue::Kind::Integer && cv->kind != ConstValue::Kind::String) {
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
                auto asStr = [](const ConstValue& cv) -> const char* { return cv.strVal.c_str(); };
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
                    if (i0 && i1)
                        written = std::snprintf(buf, sizeof(buf), fmt.c_str(), a0.intVal, a1.intVal);
                    else if (i0 && !i1)
                        written = std::snprintf(buf, sizeof(buf), fmt.c_str(), a0.intVal, asStr(a1));
                    else if (!i0 && i1)
                        written = std::snprintf(buf, sizeof(buf), fmt.c_str(), asStr(a0), a1.intVal);
                    else
                        written = std::snprintf(buf, sizeof(buf), fmt.c_str(), asStr(a0), asStr(a1));
                }
                if (written > 0 && written < static_cast<int>(sizeof(buf))) {
                    optStats_.constFolded++;
                    llvm::GlobalVariable* gv = internString(std::string(buf, static_cast<size_t>(written)));
                    stringReturningFunctions_.insert("str_format");
                    return llvm::ConstantExpr::getInBoundsGetElementPtr(
                        gv->getValueType(), gv,
                        llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
                }
            }
        }

        // ── Runtime path: two-pass snprintf ────────────────────────
        auto* ptrTy = llvm::PointerType::getUnqual(*context);

        // Codegen format string argument (must be a pointer/string).
        llvm::Value* fmtArg = generateExpression(expr->arguments[0].get());
        llvm::Value* fmtPtr =
            fmtArg->getType()->isPointerTy() ? fmtArg : builder->CreateIntToPtr(fmtArg, ptrTy, "strfmt.fmtptr");
        // fmtPtr is a fat-pointer header; snprintf needs the raw char data.
        llvm::Value* fmtStr = emitStringData(fmtPtr, "strfmt.fmtstr");

        // Codegen value arguments, keeping track of their LLVM types for snprintf.
        std::vector<llvm::Value*> valArgs;
        for (size_t i = 1; i < nArgs; ++i) {
            llvm::Value* v = generateExpression(expr->arguments[i].get());
            // Strings are fat-pointer headers — pass the raw char data to snprintf.
            if (v->getType()->isPointerTy())
                v = emitStringData(v, "strfmt.valstr");
            else if (!v->getType()->isDoubleTy())
                v = toDefaultType(v); // ensure i64
            valArgs.push_back(v);
        }

        // Probe call: snprintf(NULL, 0, fmtStr, val...) → required length.
        llvm::Value* nullBuf = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* zero32 = llvm::ConstantInt::get(getDefaultType(), 0);
        std::vector<llvm::Value*> probeCallArgs = {nullBuf, zero32, fmtStr};
        probeCallArgs.insert(probeCallArgs.end(), valArgs.begin(), valArgs.end());
        llvm::Value* probeResult = builder->CreateCall(getOrDeclareSnprintf(), probeCallArgs, "strfmt.probe");
        // snprintf returns the number of characters that would have been written
        // (not counting the null terminator).  Extend to i64 for arithmetic.
        // snprintf returns non-negative on success; zext nneg allows LLVM to infer [0,2^31).
        llvm::Value* neededLen = builder->CreateZExt(probeResult, getDefaultType(), "strfmt.needed",
                                                     /*IsNonNeg=*/false);
        nonNegValues_.insert(neededLen);

        // Allocate a fat-pointer string (header + char data + NUL).
        llvm::Value* result = emitAllocString(neededLen, neededLen, "strfmt");
        llvm::Value* bufData = emitStringData(result, "strfmt.bufdata");
        llvm::Value* allocSize = builder->CreateAdd(neededLen, llvm::ConstantInt::get(getDefaultType(), 1),
                                                    "strfmt.allocsz", /*HasNUW=*/true, /*HasNSW=*/true);

        // Fill call: snprintf(bufData, neededLen + 1, fmtStr, val...).
        std::vector<llvm::Value*> fillCallArgs = {bufData, allocSize, fmtStr};
        fillCallArgs.insert(fillCallArgs.end(), valArgs.begin(), valArgs.end());
        builder->CreateCall(getOrDeclareSnprintf(), fillCallArgs);

        stringReturningFunctions_.insert("str_format");
        return result;
    }

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
        llvm::Value* m = builder->CreateSelect(aLtB, aLenLoad, bLenLoad, "azip.m");
        nonNegValues_.insert(m);

        // outLen = 2 * m  (nuw+nsw: m ≤ INT64_MAX/2)
        llvm::Value* outLen = builder->CreateAdd(m, m, "azip.outlen", /*HasNUW=*/true, /*HasNSW=*/true);
        nonNegValues_.insert(outLen);

        // Allocate (outLen + 1) * 8 bytes
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(outLen, one, "azip.slots", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "azip.bytes", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "azip.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(*context, 8));

        // Store length header
        emitStoreArrayLen(outLen, buf);

        // Emit loop: for i in 0...m: buf[2*i+1] = a[i+1]; buf[2*i+2] = b[i+1]
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preBB = builder->GetInsertBlock();
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
        builder->CreateCondBr(builder->CreateICmpSLT(i, m, "azip.cond"), bodyBB, doneBB,
                              llvm::MDBuilder(*context).createBranchWeights(1000, 1));

        // loop body
        builder->SetInsertPoint(bodyBB);
        // srcIdx = i + 1  (nuw+nsw: i < m ≤ INT64_MAX-1)
        llvm::Value* srcIdx = builder->CreateAdd(i, one, "azip.srcidx", /*HasNUW=*/true, /*HasNSW=*/true);
        // dstIdx_a = 2*i + 1  (nuw+nsw)
        llvm::Value* two_i = builder->CreateAdd(i, i, "azip.2i", /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* dstIdxA = builder->CreateAdd(two_i, one, "azip.dstidxa", /*HasNUW=*/true, /*HasNSW=*/true);
        // dstIdx_b = 2*i + 2  (nuw+nsw)
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);
        llvm::Value* dstIdxB = builder->CreateAdd(two_i, two, "azip.dstidxb", /*HasNUW=*/true, /*HasNSW=*/true);

        // Load a[i] and b[i] (element at srcIdx in the header+data layout)
        llvm::Value* aElemPtr = builder->CreateInBoundsGEP(getDefaultType(), aPtr, srcIdx, "azip.aelemptr");
        llvm::Value* aElemLoad = emitLoadArrayElem(aElemPtr, "azip.aelem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(aElemLoad)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                                  llvm::MDNode::get(*context, {}));

        llvm::Value* bElemPtr = builder->CreateInBoundsGEP(getDefaultType(), bPtr, srcIdx, "azip.belemptr");
        llvm::Value* bElemLoad = emitLoadArrayElem(bElemPtr, "azip.belem");
        if (optimizationLevel >= OptimizationLevel::O1)
            llvm::cast<llvm::Instruction>(bElemLoad)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                                  llvm::MDNode::get(*context, {}));

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
        return buf;
    }

    // ── BigInt builtins ──────────────────────────────────────────────────────

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
    // Comparison builtins: return i32 (0 or 1), widened to i64 for OmScript.
    // ICmpNE converts the i32 0/1 to i1, then emitBoolZExt adds zext nneg +
    // !range [0,2) + nonNegValues_ tracking.
    if (bid == BuiltinId::BIGINT_EQ) {
        auto [a, b] = getBigintBinaryArgs("bigint_eq");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintEq(), {a, b}, "bigint.eq");
        return emitBoolZExt(builder->CreateIsNotNull(r, "bigint.eq.cmp"), "bigint.eq.zext");
    }
    if (bid == BuiltinId::BIGINT_LT) {
        auto [a, b] = getBigintBinaryArgs("bigint_lt");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintLt(), {a, b}, "bigint.lt");
        return emitBoolZExt(builder->CreateIsNotNull(r, "bigint.lt.cmp"), "bigint.lt.zext");
    }
    if (bid == BuiltinId::BIGINT_LE) {
        auto [a, b] = getBigintBinaryArgs("bigint_le");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintLe(), {a, b}, "bigint.le");
        return emitBoolZExt(builder->CreateIsNotNull(r, "bigint.le.cmp"), "bigint.le.zext");
    }
    if (bid == BuiltinId::BIGINT_GT) {
        auto [a, b] = getBigintBinaryArgs("bigint_gt");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintGt(), {a, b}, "bigint.gt");
        return emitBoolZExt(builder->CreateIsNotNull(r, "bigint.gt.cmp"), "bigint.gt.zext");
    }
    if (bid == BuiltinId::BIGINT_GE) {
        auto [a, b] = getBigintBinaryArgs("bigint_ge");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintGe(), {a, b}, "bigint.ge");
        return emitBoolZExt(builder->CreateIsNotNull(r, "bigint.ge.cmp"), "bigint.ge.zext");
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
        return emitBoolZExt(builder->CreateIsNotNull(r, "bigint.iszero.cmp"), "bigint.iszero.zext");
    }
    if (bid == BuiltinId::BIGINT_IS_NEGATIVE) {
        llvm::Value* a = getBigintUnaryArg("bigint_is_negative");
        llvm::Value* r = builder->CreateCall(getOrDeclareBigintIsNegative(), {a}, "bigint.isneg");
        return emitBoolZExt(builder->CreateIsNotNull(r, "bigint.isneg.cmp"), "bigint.isneg.zext");
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
        llvm::Value* prod = builder->CreateMul(aWide, bWide, "mulhi.prod");
        llvm::Value* hi128 = builder->CreateAShr(prod, llvm::ConstantInt::get(i128Ty, 64), "mulhi.hi128");
        return builder->CreateTrunc(hi128, getDefaultType(), "mulhi.result");
    }

    // ── mulhi_u(a, b) — unsigned high 64 bits of 128-bit product ─────────────
    if (bid == BuiltinId::UINT_MULHI) {
        validateArgCount(expr, "mulhi_u", 2);
        if (auto a = tryFoldInt(expr->arguments[0].get()))
            if (auto b = tryFoldInt(expr->arguments[1].get())) {
                using U128 = unsigned __int128;
                U128 product =
                    static_cast<U128>(static_cast<uint64_t>(*a)) * static_cast<U128>(static_cast<uint64_t>(*b));
                int64_t hi = static_cast<int64_t>(static_cast<uint64_t>(product >> 64));
                return llvm::ConstantInt::get(getDefaultType(), hi);
            }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        llvm::Type* i128Ty = llvm::Type::getIntNTy(*context, 128);
        // nneg: widening unsigned i64→i128 produces a value in [0, 2^64-1].
        llvm::Value* aWide = builder->CreateZExt(a, i128Ty, "umulhi.a", /*IsNonNeg=*/false);
        llvm::Value* bWide = builder->CreateZExt(b, i128Ty, "umulhi.b", /*IsNonNeg=*/false);
        llvm::Value* prod = builder->CreateMul(aWide, bWide, "umulhi.prod",
                                               /*HasNUW=*/true, /*HasNSW=*/false);
        llvm::Value* hi128 = builder->CreateLShr(prod, llvm::ConstantInt::get(i128Ty, 64), "umulhi.hi128");
        return builder->CreateTrunc(hi128, getDefaultType(), "umulhi.result");
    }

    // ── absdiff(a, b) — |a - b| without signed overflow ─────────────────────
    if (bid == BuiltinId::INT_ABSDIFF) {
        validateArgCount(expr, "absdiff", 2);
        if (auto a = tryFoldInt(expr->arguments[0].get()))
            if (auto b = tryFoldInt(expr->arguments[1].get())) {
                using I128 = __int128;
                I128 diff = static_cast<I128>(*a) - static_cast<I128>(*b);
                if (diff < 0)
                    diff = -diff;
                return llvm::ConstantInt::get(getDefaultType(), static_cast<int64_t>(diff));
            }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        // Use select: diff = a>=b ? a-b : b-a.  Both subtractions are safe
        // because we only execute the non-overflowing branch.
        llvm::Value* cmp = builder->CreateICmpSGE(a, b, "absdiff.ge");
        llvm::Value* sub1 = builder->CreateSub(a, b, "absdiff.sub1");
        llvm::Value* sub2 = builder->CreateSub(b, a, "absdiff.sub2");
        llvm::Value* result = builder->CreateSelect(cmp, sub1, sub2, "absdiff.result");
        nonNegValues_.insert(result);
        return result;
    }

    // ── fast_sqrt(x) — sqrt with fast-math flags (RSqrt-eligible on x86) ────
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
        return emitBoolZExt(cmp, "is_nan.result");
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
        return emitBoolZExt(either, "is_inf.result");
    }

    // ── 2D Column-Major Matrix Builtins ──────────────────────────────────────
    auto matElemPtr = [&](llvm::Value* mPtr, llvm::Value* rowsV, llvm::Value* iV, llvm::Value* jV,
                          const char* name) -> llvm::Value* {
        // slot = 2 + j * rows + i  (all i64 arithmetic, NSW to aid SCEV)
        llvm::Value* jRows = builder->CreateMul(jV, rowsV, (std::string(name) + ".jrows").c_str(),
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slot = builder->CreateAdd(jRows, iV, (std::string(name) + ".slot0").c_str(),
                                               /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slot2 = builder->CreateAdd(slot, llvm::ConstantInt::get(getDefaultType(), 2),
                                                (std::string(name) + ".slot2").c_str(),
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        return builder->CreateInBoundsGEP(getDefaultType(), mPtr, slot2, (std::string(name) + ".ptr").c_str());
    };

    // ── mat_new(rows, cols) ─────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_NEW) {
        validateArgCount(expr, "mat_new", 2);
        llvm::Value* rowsV = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* colsV = toDefaultType(generateExpression(expr->arguments[1].get()));
        // Clamp negative dimensions to 0 to prevent overflow in size calculation.
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        rowsV = builder->CreateSelect(builder->CreateICmpSLT(rowsV, zero, "mat.rows.neg"), zero, rowsV, "mat.rows");
        colsV = builder->CreateSelect(builder->CreateICmpSLT(colsV, zero, "mat.cols.neg"), zero, colsV, "mat.cols");
        // slots = rows * cols + 2  (two i64 header slots)
        llvm::Value* elems = builder->CreateMul(rowsV, colsV, "mat.elems",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slots = builder->CreateAdd(elems, llvm::ConstantInt::get(getDefaultType(), 2), "mat.slots",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        // calloc(slots, 8) → zero-initialised; header must still be written.
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* buf = builder->CreateCall(getOrDeclareCalloc(), {slots, eight}, "mat.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
        // Write header: buf[0] = rows, buf[1] = cols
        llvm::Value* hdr0 =
            builder->CreateInBoundsGEP(getDefaultType(), buf, llvm::ConstantInt::get(getDefaultType(), 0), "mat.hdr0");
        builder->CreateAlignedStore(rowsV, hdr0, llvm::MaybeAlign(8));
        llvm::Value* hdr1 =
            builder->CreateInBoundsGEP(getDefaultType(), buf, llvm::ConstantInt::get(getDefaultType(), 1), "mat.hdr1");
        builder->CreateAlignedStore(colsV, hdr1, llvm::MaybeAlign(8));
        // mat_new returns a matrix handle — treat it like an array-returning function
        // so the codegen knows its result is a heap pointer.
        arrayReturningFunctions_.insert("mat_new");
        return buf;
    }

    // ── mat_fill(rows, cols, val) ───────────────────────────────────────────
    if (bid == BuiltinId::MAT_FILL) {
        validateArgCount(expr, "mat_fill", 3);
        llvm::Value* rowsV = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* colsV = toDefaultType(generateExpression(expr->arguments[1].get()));
        llvm::Value* valV = toDefaultType(generateExpression(expr->arguments[2].get()));
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        rowsV = builder->CreateSelect(builder->CreateICmpSLT(rowsV, zero, "matf.rows.neg"), zero, rowsV, "matf.rows");
        colsV = builder->CreateSelect(builder->CreateICmpSLT(colsV, zero, "matf.cols.neg"), zero, colsV, "matf.cols");
        llvm::Value* elems = builder->CreateMul(rowsV, colsV, "matf.elems",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slots = builder->CreateAdd(elems, llvm::ConstantInt::get(getDefaultType(), 2), "matf.slots",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* bytes = builder->CreateMul(slots, eight, "matf.bytes",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "matf.buf");
        llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
        // Write header
        builder->CreateAlignedStore(
            rowsV,
            builder->CreateInBoundsGEP(getDefaultType(), buf, llvm::ConstantInt::get(getDefaultType(), 0), "matf.hdr0"),
            llvm::MaybeAlign(8));
        builder->CreateAlignedStore(
            colsV,
            builder->CreateInBoundsGEP(getDefaultType(), buf, llvm::ConstantInt::get(getDefaultType(), 1), "matf.hdr1"),
            llvm::MaybeAlign(8));
        // Fill loop over all elements (column-major order, stride-1 access).
        // Use emitCountingLoop to get vectorized fill.
        llvm::Value* fillBuf = buf;
        llvm::Value* fillVal = valV;
        emitCountingLoop("matf.fill", elems, zero, 4, [&](llvm::PHINode* idx, llvm::BasicBlock* loopBB) {
            // slot = idx + 2  (skip the two header slots)
            llvm::Value* slot = builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 2), "matf.slot",
                                                   /*HasNUW=*/true, /*HasNSW=*/true);
            llvm::Value* ep = builder->CreateInBoundsGEP(getDefaultType(), fillBuf, slot, "matf.ep");
            emitStoreArrayElem(fillVal, ep);
            llvm::Value* next = builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 1), "matf.next",
                                                   /*HasNUW=*/true, /*HasNSW=*/true);
            idx->addIncoming(next, builder->GetInsertBlock());
            attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));
        });
        arrayReturningFunctions_.insert("mat_fill");
        return buf;
    }

    // ── mat_rows(m) ─────────────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_ROWS) {
        validateArgCount(expr, "mat_rows", 1);
        llvm::Value* mVal = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* mPtr = builder->CreateIntToPtr(mVal, llvm::PointerType::getUnqual(*context), "matr.ptr");
        llvm::Value* hdr0 = builder->CreateInBoundsGEP(getDefaultType(), mPtr,
                                                       llvm::ConstantInt::get(getDefaultType(), 0), "matr.hdr0");
        auto* ld = builder->CreateAlignedLoad(getDefaultType(), hdr0, llvm::MaybeAlign(8), "mat.rows.val");
        if (optimizationLevel >= OptimizationLevel::O1)
            ld->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        nonNegValues_.insert(ld);
        return ld;
    }

    // ── mat_cols(m) ─────────────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_COLS) {
        validateArgCount(expr, "mat_cols", 1);
        llvm::Value* mVal = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* mPtr = builder->CreateIntToPtr(mVal, llvm::PointerType::getUnqual(*context), "matc.ptr");
        llvm::Value* hdr1 = builder->CreateInBoundsGEP(getDefaultType(), mPtr,
                                                       llvm::ConstantInt::get(getDefaultType(), 1), "matc.hdr1");
        auto* ld = builder->CreateAlignedLoad(getDefaultType(), hdr1, llvm::MaybeAlign(8), "mat.cols.val");
        if (optimizationLevel >= OptimizationLevel::O1)
            ld->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        nonNegValues_.insert(ld);
        return ld;
    }

    // ── mat_get(m, i, j) ────────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_GET) {
        validateArgCount(expr, "mat_get", 3);
        llvm::Value* mVal = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* iVal = toDefaultType(generateExpression(expr->arguments[1].get()));
        llvm::Value* jVal = toDefaultType(generateExpression(expr->arguments[2].get()));
        llvm::Value* mPtr = builder->CreateIntToPtr(mVal, llvm::PointerType::getUnqual(*context), "matg.ptr");
        auto* rowsLd = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mPtr, llvm::ConstantInt::get(getDefaultType(), 0),
                                       "matg.hdr0"),
            llvm::MaybeAlign(8), "matg.rows");
        if (optimizationLevel >= OptimizationLevel::O1)
            rowsLd->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        nonNegValues_.insert(rowsLd);
        llvm::Value* rowsV = rowsLd;
        llvm::Value* ep = matElemPtr(mPtr, rowsV, iVal, jVal, "matg");
        auto* ld = builder->CreateAlignedLoad(getDefaultType(), ep, llvm::MaybeAlign(8), "mat.get.val");
        if (optimizationLevel >= OptimizationLevel::O1)
            ld->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
        if (currentLoopAccessGroup_)
            ld->setMetadata(llvm::LLVMContext::MD_access_group, currentLoopAccessGroup_);
        return ld;
    }

    // ── mat_set(m, i, j, val) ───────────────────────────────────────────────
    if (bid == BuiltinId::MAT_SET) {
        validateArgCount(expr, "mat_set", 4);
        llvm::Value* mVal = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* iVal = toDefaultType(generateExpression(expr->arguments[1].get()));
        llvm::Value* jVal = toDefaultType(generateExpression(expr->arguments[2].get()));
        llvm::Value* valV = toDefaultType(generateExpression(expr->arguments[3].get()));
        llvm::Value* mPtr = builder->CreateIntToPtr(mVal, llvm::PointerType::getUnqual(*context), "mats.ptr");
        auto* rowsLdS = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mPtr, llvm::ConstantInt::get(getDefaultType(), 0),
                                       "mats.hdr0"),
            llvm::MaybeAlign(8), "mats.rows");
        if (optimizationLevel >= OptimizationLevel::O1)
            rowsLdS->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        nonNegValues_.insert(rowsLdS);
        llvm::Value* rowsV = rowsLdS;
        llvm::Value* ep = matElemPtr(mPtr, rowsV, iVal, jVal, "mats");
        auto* st = builder->CreateAlignedStore(valV, ep, llvm::MaybeAlign(8));
        if (currentLoopAccessGroup_)
            st->setMetadata(llvm::LLVMContext::MD_access_group, currentLoopAccessGroup_);
        return mVal; // return the matrix pointer for chaining
    }

    // ── mat_transp(m) ────────────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_TRANSP) {
        validateArgCount(expr, "mat_transp", 1);
        llvm::Value* mVal = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* mPtr = builder->CreateIntToPtr(mVal, llvm::PointerType::getUnqual(*context), "matt.ptr");
        auto* rowsLdT = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mPtr, llvm::ConstantInt::get(getDefaultType(), 0),
                                       "matt.hdr0"),
            llvm::MaybeAlign(8), "matt.rows");
        if (optimizationLevel >= OptimizationLevel::O1)
            rowsLdT->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        nonNegValues_.insert(rowsLdT);
        llvm::Value* rowsV = rowsLdT;
        auto* colsLdT = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), mPtr, llvm::ConstantInt::get(getDefaultType(), 1),
                                       "matt.hdr1"),
            llvm::MaybeAlign(8), "matt.cols");
        if (optimizationLevel >= OptimizationLevel::O1)
            colsLdT->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        nonNegValues_.insert(colsLdT);
        llvm::Value* colsV = colsLdT;
        // Allocate transposed matrix: mat_new(cols, rows)
        llvm::Value* elems = builder->CreateMul(rowsV, colsV, "matt.elems",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* slots = builder->CreateAdd(elems, llvm::ConstantInt::get(getDefaultType(), 2), "matt.slots",
                                                /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* tBuf = builder->CreateCall(getOrDeclareCalloc(), {slots, eight}, "matt.buf");
        llvm::cast<llvm::CallInst>(tBuf)->addRetAttr(llvm::Attribute::NonNull);
        // Header of transpose: rows_T = cols, cols_T = rows
        builder->CreateAlignedStore(colsV,
                                    builder->CreateInBoundsGEP(getDefaultType(), tBuf,
                                                               llvm::ConstantInt::get(getDefaultType(), 0),
                                                               "matt.thdr0"),
                                    llvm::MaybeAlign(8));
        builder->CreateAlignedStore(rowsV,
                                    builder->CreateInBoundsGEP(getDefaultType(), tBuf,
                                                               llvm::ConstantInt::get(getDefaultType(), 1),
                                                               "matt.thdr1"),
                                    llvm::MaybeAlign(8));
        // Copy loop: T[j, i] = A[i, j]  for all i in 0..rows, j in 0..cols
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        emitCountingLoop("matt.j", colsV, zero, 1, [&](llvm::PHINode* jIdx, llvm::BasicBlock* jLoopBB) {
            llvm::Value* jIdxV = jIdx; // j column index
            emitCountingLoop("matt.i", rowsV, zero, 4, [&](llvm::PHINode* iIdx, llvm::BasicBlock* iLoopBB) {
                // Load A[i, j] (column-major: slot = j*rows + i + 2)
                llvm::Value* epA = matElemPtr(mPtr, rowsV, iIdx, jIdxV, "matt.a");
                auto* aElem = builder->CreateAlignedLoad(getDefaultType(), epA, llvm::MaybeAlign(8), "matt.aval");
                if (optimizationLevel >= OptimizationLevel::O1)
                    aElem->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
                // Store T[j, i]: T has rowsT=cols, so slot = i*cols + j + 2
                llvm::Value* epT = matElemPtr(tBuf, colsV, jIdxV, iIdx, "matt.t");
                builder->CreateAlignedStore(aElem, epT, llvm::MaybeAlign(8));
                llvm::Value* ni = builder->CreateAdd(iIdx, llvm::ConstantInt::get(getDefaultType(), 1), "matt.ni",
                                                     /*HasNUW=*/true, /*HasNSW=*/true);
                iIdx->addIncoming(ni, builder->GetInsertBlock());
                attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(iLoopBB)));
            });
            llvm::Value* nj = builder->CreateAdd(jIdx, llvm::ConstantInt::get(getDefaultType(), 1), "matt.nj",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
            jIdx->addIncoming(nj, builder->GetInsertBlock());
            builder->CreateBr(jLoopBB);
        });
        arrayReturningFunctions_.insert("mat_transp");
        return tBuf;
    }

    // ── mat_mul(a, b) ────────────────────────────────────────────────────────
    if (bid == BuiltinId::MAT_MUL) {
        validateArgCount(expr, "mat_mul", 2);
        llvm::Value* aVal = toDefaultType(generateExpression(expr->arguments[0].get()));
        llvm::Value* bVal = toDefaultType(generateExpression(expr->arguments[1].get()));
        llvm::Value* aPtr = builder->CreateIntToPtr(aVal, llvm::PointerType::getUnqual(*context), "matm.aptr");
        llvm::Value* bPtr = builder->CreateIntToPtr(bVal, llvm::PointerType::getUnqual(*context), "matm.bptr");
        // Dimension loads
        auto* mDimLd = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), aPtr, llvm::ConstantInt::get(getDefaultType(), 0),
                                       "matm.a.hdr0"),
            llvm::MaybeAlign(8), "matm.m");
        auto* kDimLd = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), aPtr, llvm::ConstantInt::get(getDefaultType(), 1),
                                       "matm.a.hdr1"),
            llvm::MaybeAlign(8), "matm.k");
        auto* nDimLd = builder->CreateAlignedLoad(
            getDefaultType(),
            builder->CreateInBoundsGEP(getDefaultType(), bPtr, llvm::ConstantInt::get(getDefaultType(), 1),
                                       "matm.b.hdr1"),
            llvm::MaybeAlign(8), "matm.n");
        if (optimizationLevel >= OptimizationLevel::O1) {
            mDimLd->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            kDimLd->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            nDimLd->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        }
        nonNegValues_.insert(mDimLd);
        nonNegValues_.insert(kDimLd);
        nonNegValues_.insert(nDimLd);
        llvm::Value* mDim = mDimLd;
        llvm::Value* kDim = kDimLd;
        llvm::Value* nDim = nDimLd;
        // Allocate result matrix C(m, n) — zero-initialised via calloc
        llvm::Value* cElems = builder->CreateMul(mDim, nDim, "matm.celems",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* cSlots = builder->CreateAdd(cElems, llvm::ConstantInt::get(getDefaultType(), 2), "matm.cslots",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* cBuf = builder->CreateCall(getOrDeclareCalloc(), {cSlots, eight}, "matm.cbuf");
        llvm::cast<llvm::CallInst>(cBuf)->addRetAttr(llvm::Attribute::NonNull);
        // Write C header
        builder->CreateAlignedStore(mDim,
                                    builder->CreateInBoundsGEP(getDefaultType(), cBuf,
                                                               llvm::ConstantInt::get(getDefaultType(), 0),
                                                               "matm.chdr0"),
                                    llvm::MaybeAlign(8));
        builder->CreateAlignedStore(nDim,
                                    builder->CreateInBoundsGEP(getDefaultType(), cBuf,
                                                               llvm::ConstantInt::get(getDefaultType(), 1),
                                                               "matm.chdr1"),
                                    llvm::MaybeAlign(8));
        // Triple loop: j (outer) → p (middle) → i (inner, vectorized)
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        emitCountingLoop("matm.j", nDim, zero, 1, [&](llvm::PHINode* jIdx, llvm::BasicBlock* jLoopBB) {
            emitCountingLoop("matm.p", kDim, zero, 1, [&](llvm::PHINode* pIdx, llvm::BasicBlock* pLoopBB) {
                // b_pj = B[p, j]  (scalar)
                llvm::Value* bEp = matElemPtr(bPtr, kDim, pIdx, jIdx, "matm.b");
                llvm::Value* b_pj = builder->CreateAlignedLoad(getDefaultType(), bEp, llvm::MaybeAlign(8), "matm.bpj");
                if (optimizationLevel >= OptimizationLevel::O1)
                    llvm::cast<llvm::LoadInst>(b_pj)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                                  llvm::MDNode::get(*context, {}));
                // Inner loop: C[i, j] += b_pj * A[i, p]  (vectorized)
                emitCountingLoop("matm.i", mDim, zero, 4, [&](llvm::PHINode* iIdx, llvm::BasicBlock* iLoopBB) {
                    // A[i, p]: slot = p*m + i + 2
                    llvm::Value* aEp = matElemPtr(aPtr, mDim, iIdx, pIdx, "matm.a");
                    llvm::Value* a_ip =
                        builder->CreateAlignedLoad(getDefaultType(), aEp, llvm::MaybeAlign(8), "matm.aip");
                    if (optimizationLevel >= OptimizationLevel::O1)
                        llvm::cast<llvm::LoadInst>(a_ip)->setMetadata(llvm::LLVMContext::MD_noundef,
                                                                      llvm::MDNode::get(*context, {}));
                    // C[i, j]: slot = j*m + i + 2
                    llvm::Value* cEp = matElemPtr(cBuf, mDim, iIdx, jIdx, "matm.c");
                    llvm::Value* c_ij =
                        builder->CreateAlignedLoad(getDefaultType(), cEp, llvm::MaybeAlign(8), "matm.cij");
                    // c_ij += b_pj * a_ip
                    llvm::Value* prod = builder->CreateMul(b_pj, a_ip, "matm.prod",
                                                           /*HasNUW=*/false, /*HasNSW=*/true);
                    llvm::Value* sum = builder->CreateAdd(c_ij, prod, "matm.sum",
                                                          /*HasNUW=*/false, /*HasNSW=*/true);
                    builder->CreateAlignedStore(sum, cEp, llvm::MaybeAlign(8));
                    llvm::Value* ni = builder->CreateAdd(iIdx, llvm::ConstantInt::get(getDefaultType(), 1), "matm.ni",
                                                         /*HasNUW=*/true, /*HasNSW=*/true);
                    iIdx->addIncoming(ni, builder->GetInsertBlock());
                    attachLoopMetadataVec(llvm::cast<llvm::BranchInst>(builder->CreateBr(iLoopBB)));
                });
                llvm::Value* np = builder->CreateAdd(pIdx, llvm::ConstantInt::get(getDefaultType(), 1), "matm.np",
                                                     /*HasNUW=*/true, /*HasNSW=*/true);
                pIdx->addIncoming(np, builder->GetInsertBlock());
                builder->CreateBr(pLoopBB);
            });
            llvm::Value* nj = builder->CreateAdd(jIdx, llvm::ConstantInt::get(getDefaultType(), 1), "matm.nj",
                                                 /*HasNUW=*/true, /*HasNSW=*/true);
            jIdx->addIncoming(nj, builder->GetInsertBlock());
            builder->CreateBr(jLoopBB);
        });
        arrayReturningFunctions_.insert("mat_mul");
        return cBuf;
    }

    // ── newRegion() ── create a region handle (malloc-backed arena pointer) ──
    if (bid == BuiltinId::NEW_REGION) {
        validateArgCount(expr, "newRegion", 0);
        // Emit: call i8* @malloc(i64 8)  (8-byte region header)
        llvm::Function* mallocFn = llvm::cast<llvm::Function>(
            module
                ->getOrInsertFunction("malloc", llvm::FunctionType::get(llvm::PointerType::getUnqual(*context),
                                                                        {llvm::Type::getInt64Ty(*context)}, false))
                .getCallee());
        auto* hdr =
            builder->CreateCall(mallocFn, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 8)}, "region.hdr");
        // Return as a pointer-sized integer so it fits in the default i64 type.
        return hdr;
    }

    // ── alloc(region, size) ── allocate `size` bytes via malloc ──────────────
    if (bid == BuiltinId::ALLOC) {
        validateArgCount(expr, "alloc", 2);
        // Second argument is the size.
        llvm::Value* sizeVal = generateExpression(expr->arguments[1].get());
        if (!sizeVal->getType()->isIntegerTy())
            sizeVal = builder->CreatePtrToInt(sizeVal, getDefaultType(), "alloc.sz");
        llvm::Function* mallocFn = llvm::cast<llvm::Function>(
            module
                ->getOrInsertFunction("malloc", llvm::FunctionType::get(llvm::PointerType::getUnqual(*context),
                                                                        {llvm::Type::getInt64Ty(*context)}, false))
                .getCallee());
        auto* ptr = builder->CreateCall(
            mallocFn, {builder->CreateIntCast(sizeVal, llvm::Type::getInt64Ty(*context), false, "alloc.sz64")},
            "region.alloc");
        return ptr;
    }

    // ── malloc(size) ── allocate `size` bytes, return raw ptr ────────────────
    if (bid == BuiltinId::MALLOC) {
        validateArgCount(expr, "malloc", 1);
        llvm::Value* sizeVal = generateExpression(expr->arguments[0].get());
        if (!sizeVal->getType()->isIntegerTy())
            sizeVal = builder->CreatePtrToInt(sizeVal, getDefaultType(), "malloc.sz");
        llvm::Function* mallocFn = getOrDeclareMalloc();
        auto* rawPtr = builder->CreateCall(
            mallocFn, {builder->CreateIntCast(sizeVal, llvm::Type::getInt64Ty(*context), false, "malloc.sz64")},
            "malloc.ptr");
        return rawPtr;
    }

    // ── free(ptr) ── deallocate a pointer returned by malloc ─────────────────
    if (bid == BuiltinId::FREE) {
        validateArgCount(expr, "free", 1);
        llvm::Value* ptrVal = generateExpression(expr->arguments[0].get());
        // Accept both pointer-typed values and integer-encoded pointers.
        if (!ptrVal->getType()->isPointerTy())
            ptrVal = builder->CreateIntToPtr(ptrVal, llvm::PointerType::getUnqual(*context), "free.itop");
        builder->CreateCall(getOrDeclareFree(), {ptrVal});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // ── sizeof(typename) ── byte size of a type as a compile-time i64 ────────
    if (bid == BuiltinId::SIZEOF) {
        validateArgCount(expr, "sizeof", 1);
        // Try to extract the type name from a single identifier argument.
        std::string typeName;
        if (expr->arguments[0]->type == ASTNodeType::IDENTIFIER_EXPR) {
            typeName = static_cast<IdentifierExpr*>(expr->arguments[0].get())->name;
        }
        llvm::Type* ty = typeName.empty() ? getDefaultType() : resolveAnnotatedType(typeName);
        const llvm::DataLayout& DL = module->getDataLayout();
        uint64_t sz = DL.getTypeAllocSize(ty);
        return llvm::ConstantInt::get(getDefaultType(), sz);
    }

    // ── HTTP client builtins ─────────────────────────────────────────────────
    // All HTTP builtins return a string (pointer encoded as i64) or an i64
    // status code.  The returned strings are malloc'd by the runtime and must
    // be free()'d by the caller; the string ownership model is identical to
    // other string-returning builtins such as command() and bigint_tostring().

    // http_get(url) → body string
    if (bid == BuiltinId::HTTP_GET) {
        validateArgCount(expr, "http_get", 1);
        llvm::Value* urlVal = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* urlPtr =
            urlVal->getType()->isPointerTy() ? urlVal : builder->CreateIntToPtr(urlVal, ptrTy, "hget.url");
        llvm::Value* result = builder->CreateCall(getOrDeclareHttpGet(), {urlPtr}, "hget.body");
        return result;
    }

    // http_post(url, body[, content_type]) → body string
    if (bid == BuiltinId::HTTP_POST) {
        if (expr->arguments.size() < 2 || expr->arguments.size() > 3)
            codegenError("http_post() requires 2 or 3 arguments: (url, body[, content_type])", expr);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        auto toPtr = [&](llvm::Value* v, const char* nm) -> llvm::Value* {
            return v->getType()->isPointerTy() ? v : builder->CreateIntToPtr(v, ptrTy, nm);
        };
        llvm::Value* urlPtr = toPtr(generateExpression(expr->arguments[0].get()), "hpost.url");
        llvm::Value* bodyPtr = toPtr(generateExpression(expr->arguments[1].get()), "hpost.body");
        llvm::Value* ctPtr;
        if (expr->arguments.size() == 3) {
            ctPtr = toPtr(generateExpression(expr->arguments[2].get()), "hpost.ct");
        } else {
            // Default Content-Type: pass NULL (runtime uses "application/octet-stream").
            ctPtr = llvm::ConstantPointerNull::get(ptrTy);
        }
        llvm::Value* result = builder->CreateCall(getOrDeclareHttpPost(), {urlPtr, bodyPtr, ctPtr}, "hpost.body");
        return result;
    }

    // http_request(method, url, body, headers) → body string
    if (bid == BuiltinId::HTTP_REQUEST) {
        if (expr->arguments.size() < 2 || expr->arguments.size() > 4)
            codegenError("http_request() requires 2–4 arguments: (method, url[, body[, headers]])", expr);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        auto toPtr = [&](llvm::Value* v, const char* nm) -> llvm::Value* {
            return v->getType()->isPointerTy() ? v : builder->CreateIntToPtr(v, ptrTy, nm);
        };
        auto nullPtr = [&]() -> llvm::Value* { return llvm::ConstantPointerNull::get(ptrTy); };
        llvm::Value* methodPtr = toPtr(generateExpression(expr->arguments[0].get()), "hreq.method");
        llvm::Value* urlPtr = toPtr(generateExpression(expr->arguments[1].get()), "hreq.url");
        llvm::Value* bodyPtr = (expr->arguments.size() >= 3)
                                   ? toPtr(generateExpression(expr->arguments[2].get()), "hreq.body")
                                   : nullPtr();
        llvm::Value* hdrsPtr = (expr->arguments.size() >= 4)
                                   ? toPtr(generateExpression(expr->arguments[3].get()), "hreq.hdrs")
                                   : nullPtr();
        llvm::Value* result =
            builder->CreateCall(getOrDeclareHttpRequest(), {methodPtr, urlPtr, bodyPtr, hdrsPtr}, "hreq.body");
        return result;
    }

    // http_status(url) → HTTP status code (int)
    if (bid == BuiltinId::HTTP_STATUS) {
        validateArgCount(expr, "http_status", 1);
        llvm::Value* urlVal = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* urlPtr =
            urlVal->getType()->isPointerTy() ? urlVal : builder->CreateIntToPtr(urlVal, ptrTy, "hstat.url");
        return builder->CreateCall(getOrDeclareHttpGetStatus(), {urlPtr}, "hstat.code");
    }

    // funcptr_from(fn_name) → native function pointer (stored as ptr in funcptr alloca).
    // Returns the function's address as a native LLVM pointer — no ptrtoint needed
    // because funcptr allocas are now ptr-typed (resolveAnnotatedType returns ptr).
    if (bid == BuiltinId::FUNCPTR_FROM) {
        validateArgCount(expr, "funcptr_from", 1);
        // Argument may be a string literal OR a bare identifier naming a function.
        Expression* nameExpr = expr->arguments[0].get();
        std::string fnName;
        if (nameExpr->type == ASTNodeType::LITERAL_EXPR) {
            auto* lit = static_cast<LiteralExpr*>(nameExpr);
            if (lit->literalType == LiteralExpr::LiteralType::STRING)
                fnName = lit->stringValue;
        } else if (nameExpr->type == ASTNodeType::IDENTIFIER_EXPR) {
            // Allow funcptr_from(add) — bare identifier refers to a function name.
            fnName = static_cast<IdentifierExpr*>(nameExpr)->name;
        }
        if (fnName.empty())
            codegenError("funcptr_from: argument must be a function name (identifier or string literal)", expr);
        // Look up or declare the target function with signature () -> i64.
        llvm::Function* targetFn = module->getFunction(fnName);
        if (!targetFn) {
            // Forward-declare with the default return type; the linker will resolve it.
            llvm::FunctionType* fty = llvm::FunctionType::get(getDefaultType(), /*Params=*/{}, /*isVarArg=*/false);
            targetFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, fnName, module.get());
        }
        // Return the function pointer directly as a native LLVM ptr value.
        // This avoids the ptrtoint+inttoptr round-trip that harmed alias analysis.
        return targetFn;
    }

    // funcptr_new(byte_array, n) → executable funcptr from an array of machine-code bytes.
    // Each element of byte_array is an i64 whose low 8 bits are one byte of machine code.
    // Allocates a region of executable memory (mmap/VirtualAlloc), packs the low bytes
    // of the array elements into it, and returns the native ptr (no ptrtoint).
    //
    // Example (x86-64: mov rax, 99; ret):
    //   var code = [0x48, 0xb8, 99, 0, 0, 0, 0, 0, 0, 0, 0xc3];
    //   var f: funcptr = funcptr_new(code, 11);
    if (bid == BuiltinId::FUNCPTR_NEW) {
        validateArgCount(expr, "funcptr_new", 2);
        llvm::Value* arrVal = generateExpression(expr->arguments[0].get());
        llvm::Value* nBytes = generateExpression(expr->arguments[1].get());
        // Only convert nBytes to i64; keep arrVal as a native pointer when possible
        // to avoid a ptrtoint+inttoptr round-trip in the array-pointer path.
        nBytes = toDefaultType(nBytes);

        // Declare or reuse the array-based runtime helper:
        //   void* omsc_funcptr_new_arr(const int64_t* arr, int64_t n)
        // The helper packs the low byte of each i64 element into executable memory.
        // Returns NULL on allocation failure — do not mark as nonnull.
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        auto* helperFnTy = llvm::FunctionType::get(ptrTy, {ptrTy, getDefaultType()}, /*isVarArg=*/false);
        auto* helperFn =
            llvm::cast<llvm::Function>(module->getOrInsertFunction("omsc_funcptr_new_arr", helperFnTy).getCallee());
        helperFn->setDoesNotThrow();

        // Advance past the 8-byte length header to element 0 of the array.
        // If arrVal is already a native pointer, use it directly (no ptrtoint).
        llvm::Value* arrPtr = arrVal->getType()->isPointerTy()
                                  ? arrVal
                                  : builder->CreateIntToPtr(toDefaultType(arrVal), ptrTy, "fnew.arrptr");
        llvm::Value* dataPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr,
                                                          llvm::ConstantInt::get(getDefaultType(), 1), "fnew.data");

        // Return the executable memory pointer directly (native ptr).
        // The funcptr alloca is ptr-typed, so no ptrtoint round-trip is needed.
        return builder->CreateCall(helperFn, {dataPtr, nBytes}, "funcptr.new");
    }

    // ── pslice_len(s) ── return the length stored in a pslice ────────────────
    if (bid == BuiltinId::PSLICE_LEN) {
        validateArgCount(expr, "pslice_len", 1);
        if (expr->arguments[0]->type != ASTNodeType::IDENTIFIER_EXPR)
            codegenError("pslice_len: argument must be a pslice variable name", expr);
        const std::string& varName = static_cast<IdentifierExpr*>(expr->arguments[0].get())->name;
        auto it = psliceLenAllocas_.find(varName);
        if (it == psliceLenAllocas_.end())
            codegenError("pslice_len: '" + varName + "' is not a pslice variable", expr);
        auto* lenLoad = builder->CreateAlignedLoad(getDefaultType(), it->second, llvm::MaybeAlign(8), "pslice.len");
        if (tbaaScalar_)
            lenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaScalar_);
        nonNegValues_.insert(lenLoad);
        return lenLoad;
    }

    // ── pslice_ptr(s) ── return the raw pointer address from a pslice ─────────
    // Returns the address as i64 (ptrtoint), the only place we cast pointer→int
    // for pslice; all internal accesses work with native LLVM pointer values.
    if (bid == BuiltinId::PSLICE_PTR) {
        validateArgCount(expr, "pslice_ptr", 1);
        if (expr->arguments[0]->type != ASTNodeType::IDENTIFIER_EXPR)
            codegenError("pslice_ptr: argument must be a pslice variable name", expr);
        const std::string& varName = static_cast<IdentifierExpr*>(expr->arguments[0].get())->name;
        if (!psliceVarNames_.count(varName))
            codegenError("pslice_ptr: '" + varName + "' is not a pslice variable", expr);
        auto it = namedValues.find(varName);
        if (it == namedValues.end())
            codegenError("pslice_ptr: undefined variable '" + varName + "'", expr);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        auto* ptrLoad = builder->CreateAlignedLoad(ptrTy, it->second, llvm::MaybeAlign(8), "pslice.ptr");
        if (tbaaScalar_)
            ptrLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaScalar_);
        // ptrtoint is only done here because the user explicitly requested the
        // integer address; internal pslice codegen never converts ptr→int.
        return builder->CreatePtrToInt(ptrLoad, getDefaultType(), "pslice.ptr.i64");
    }

    // ── store_ptr(value) ── box any value onto the stack, return ptr<typeof(value)>
    // Allocates an appropriately-typed slot in the function entry block,
    // stores the value, and returns a pointer to it.  The allocation is always
    // in the entry block so mem2reg / SROA can fully reason about it.
    //
    // Supported value types:
    //   • i64  (or any integer) → stores as i64
    //   • ptr                   → stores as ptr (pointer to pointer)
    //   • f64  (floating-point) → stores as f64
    //
    // Example:  var p: ptr<i64>  = store_ptr(42)
    //           var q: ptr<ptr>  = store_ptr(some_ptr_var)
    if (bid == BuiltinId::STORE_PTR) {
        validateArgCount(expr, "store_ptr", 1);
        llvm::Value* val = generateExpression(expr->arguments[0].get());

        // Determine the storage type from the value's LLVM type.
        llvm::Type* slotTy;
        llvm::Value* storeVal = val;
        if (val->getType()->isPointerTy()) {
            // Pointer value → store as ptr (pointer-to-pointer result).
            slotTy = llvm::PointerType::getUnqual(*context);
        } else if (val->getType()->isDoubleTy()) {
            // f64 value → store as f64.
            slotTy = llvm::Type::getDoubleTy(*context);
        } else {
            // Integer value (i64 or narrower) → widen to i64 and store.
            storeVal = toDefaultType(val);
            slotTy = getDefaultType();
        }

        // Place the alloca in the function entry block so mem2reg / SROA
        // can promote it to a register where possible.
        llvm::Function* fn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        auto* slot = entryB.CreateAlloca(slotTy, nullptr, "sptr.slot");
        const llvm::DataLayout& DL = module->getDataLayout();
        slot->setAlignment(DL.getABITypeAlign(slotTy));

        // Emit lifetime.start so DSE / LICM can bound the allocation.
        auto* lifeSz = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), DL.getTypeAllocSize(slotTy));
        auto* lifetimeStart =
            OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::lifetime_start, {llvm::PointerType::getUnqual(*context)});
        builder->CreateCall(lifetimeStart, {lifeSz, slot});

        // Store the value with proper alignment.
        auto* st = builder->CreateAlignedStore(storeVal, slot, DL.getABITypeAlign(slotTy));
        if (tbaaScalar_)
            st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaScalar_);

        // The alloca itself is provably nonnull.
        return slot;
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
            if (*iv >= 0)
                nonNegValues_.insert(ci);
            return ci;
        }
        if (auto sv = optCtx_->constStringReturn(expr->callee)) {
            llvm::GlobalVariable* gv = internString(*sv);
            stringReturningFunctions_.insert(expr->callee);
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                gv->getValueType(), gv,
                llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                                                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
    }

    // Multi-param constant folding: if all arguments are compile-time constants
    // and the function body is pure-foldable, evaluate entirely at compile time.
    if (!inOptMaxFunction && !expr->arguments.empty() && optimizationLevel >= OptimizationLevel::O1) {
        auto declIt2 = functionDecls_.find(expr->callee);
        if (declIt2 != functionDecls_.end() && declIt2->second->body &&
            expr->arguments.size() == declIt2->second->parameters.size()) {

            // Try CF-CTRE engine first (richer cross-function evaluation).
            if (optCtx_ && optCtx_->isCTPure(expr->callee)) {
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
                if (allConst || anyConst) {
                    auto ctResult = optCtx_->executeFunction(expr->callee, ctArgs);
                    if (ctResult) {
                        if (ctResult->isInt()) {
                            auto* ci = llvm::ConstantInt::get(getDefaultType(), ctResult->asI64());
                            if (ctResult->asI64() >= 0)
                                nonNegValues_.insert(ci);
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
                if (!cv) {
                    allConst = false;
                    break;
                }
                argEnv[declIt2->second->parameters[i].name] = *cv;
            }
            if (allConst) {
                auto result = tryConstEvalFull(declIt2->second, argEnv);
                if (result) {
                    if (result->kind == ConstValue::Kind::Integer) {
                        auto* ci = llvm::ConstantInt::get(getDefaultType(), result->intVal);
                        if (result->intVal >= 0)
                            nonNegValues_.insert(ci);
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
                    if (result->kind == ConstValue::Kind::Array) {
                        arrayReturningFunctions_.insert(expr->callee);
                        return emitComptimeArray(result->arrVal);
                    }
                }
            }
        }
    }

    // ── Integer / wide-integer type-cast syntax ────────────────────────────
    {
        // Resolve the canonical name: strip a leading "to_" prefix so that
        // to_i32(x), to_u8(x) etc. are equivalent to i32(x), u8(x).
        const std::string& rawCallee = expr->callee;
        const std::string& cn =
            (rawCallee.size() > 3 && rawCallee.rfind("to_", 0) == 0) ? rawCallee.substr(3) : rawCallee;
        unsigned castBits = 0;
        bool castUnsigned = false;
        // Parse "iN" / "uN" pattern
        if (cn.size() >= 2 && (cn[0] == 'i' || cn[0] == 'u')) {
            bool allDigits = true;
            int n = 0;
            for (size_t j = 1; j < cn.size(); ++j) {
                if (!std::isdigit(static_cast<unsigned char>(cn[j]))) {
                    allDigits = false;
                    break;
                }
                n = n * 10 + (cn[j] - '0');
                if (n > 256) {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits && n >= 1 && n <= 256) {
                castBits = static_cast<unsigned>(n);
                castUnsigned = (cn[0] == 'u');
            }
        }
        // Accept "int" / "uint" as aliases for i64/u64
        if (cn == "int") {
            castBits = 64;
            castUnsigned = false;
        }
        if (cn == "uint") {
            castBits = 64;
            castUnsigned = true;
        }
        if (cn == "bool") {
            castBits = 1;
            castUnsigned = true;
        }

        if (castBits >= 1 && expr->arguments.size() == 1) {
            llvm::Value* arg = generateExpression(expr->arguments[0].get());

            // Normalise input to a scalar integer (pointer/float → i64 first).
            if (arg->getType()->isPointerTy())
                arg = builder->CreatePtrToInt(arg, getDefaultType(), "cast.ptoi");
            else if (arg->getType()->isDoubleTy())
                arg = builder->CreateFPToSI(arg, getDefaultType(), "cast.ftoi");

            const unsigned srcBits = arg->getType()->isIntegerTy() ? arg->getType()->getIntegerBitWidth() : 64u;
            auto* destTy = llvm::IntegerType::get(*context, castBits);

            if (castBits == 1) {
                // bool(x): normalise to 0 or 1; return as i64 (conventional boolean width)
                auto* zero = llvm::ConstantInt::get(arg->getType(), 0);
                auto* cmp = builder->CreateICmpNE(arg, zero, "bool.cmp");
                return emitBoolZExt(cmp, "bool.zext");
            }

            if (castBits == srcBits) {
                // Same width: identity (includes i64(x), u64(x), int(x), uint(x)).
                // Tag as unsigned when cast was unsigned.
                if (castUnsigned)
                    unsignedExprs_.insert(arg);
                return arg;
            }

            if (castBits > srcBits) {
                // Widen: ZExt for unsigned, SExt for signed — produce destTy directly.
                llvm::Value* r;
                if (castUnsigned) {
                    r = builder->CreateZExt(arg, destTy, cn + ".zext",
                                            /*IsNonNeg=*/false);
                    unsignedExprs_.insert(r);
                    nonNegValues_.insert(r);
                } else {
                    r = builder->CreateSExt(arg, destTy, cn + ".sext");
                }
                return r;
            }

            // Narrow: castBits < srcBits — truncate to the requested width.
            // The narrow value carries its own sign semantics; downstream users
            // (convertTo, emitStoreArrayElem, print) consult unsignedExprs_ to
            // choose ZExt vs SExt when they need to widen back.
            auto* r = builder->CreateTrunc(arg, destTy, cn + ".trunc");
            if (castUnsigned) {
                unsignedExprs_.insert(r);
                nonNegValues_.insert(r); // all bit patterns are non-negative as unsigned
            }
            return r;
        }
    }

    // ── funcptr variable call: `fp(args)` ──────────────────────────────────
    // If the callee name is a known funcptr variable (declared as `funcptr` or
    // `fn(T...) -> R`), emit an indirect typed call through the stored pointer.
    if (funcptrVarNames_.count(expr->callee)) {
        auto nvIt = namedValues.find(expr->callee);
        if (nvIt != namedValues.end() && nvIt->second) {
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            auto* i64Ty = getDefaultType();
            // Load the function pointer from the variable's alloca.
            llvm::Value* fnPtr = builder->CreateAlignedLoad(ptrTy, nvIt->second,
                                                            llvm::MaybeAlign(8),
                                                            (expr->callee + ".fnptr").c_str());
            // Build a FunctionType using i64 for every argument + return value.
            // This is the conservative ABI-compatible approach.
            std::vector<llvm::Type*> paramTys(expr->arguments.size(), i64Ty);
            llvm::FunctionType* fnTy = llvm::FunctionType::get(i64Ty, paramTys, /*isVarArg=*/false);
            std::vector<llvm::Value*> callArgs;
            callArgs.reserve(expr->arguments.size());
            for (auto& argExpr : expr->arguments) {
                llvm::Value* av = generateExpression(argExpr.get());
                // Coerce to i64 for the indirect call.
                if (av->getType()->isPointerTy())
                    av = builder->CreatePtrToInt(av, i64Ty, "fpcall.ptoi");
                else if (av->getType()->isDoubleTy())
                    av = builder->CreateBitCast(av, i64Ty, "fpcall.ftoi");
                else if (av->getType() != i64Ty)
                    av = builder->CreateSExtOrBitCast(av, i64Ty, "fpcall.ext");
                callArgs.push_back(av);
            }
            auto* ci = builder->CreateCall(fnTy, fnPtr, callArgs, "fpcall.ret");
            ci->addFnAttr(llvm::Attribute::NoUnwind);
            return ci;
        }
    }

    auto calleeIt = functions.find(expr->callee);
    // Method-call desugaring: `obj.method(args)` emits CallExpr("method", {obj, args}).
    // If the bare name isn't found, try `StructName::method` for each known struct
    // whose name appears as a type annotation on the first argument's variable.
    // This resolves method calls without requiring a type-inference pass.
    if ((calleeIt == functions.end() || !calleeIt->second) && !expr->arguments.empty()) {
        // Check if there is an exact `SomeStruct::callee` function registered.
        const std::string& mname = expr->callee;
        // Scan all registered functions for a `*::mname` match.
        llvm::StringRef mnameRef(mname);
        for (auto it = functions.begin(); it != functions.end(); ++it) {
            llvm::StringRef key = it->getKey();
            // Accept if key ends with "::<mname>" AND the prefix is a known struct.
            if (key.ends_with("::" + mname)) {
                const std::string prefix = std::string(key.drop_back(mname.size() + 2));
                if (structDefs_.count(prefix)) {
                    calleeIt = it;
                    break;
                }
            }
        }
    }
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
    // Also try the qualified name (for method-call desugaring where calleeIt
    // may have been found under a different key than expr->callee).
    if (declIt == functionDecls_.end()) {
        const std::string qualKey = std::string(calleeIt->getKey());
        declIt = functionDecls_.find(qualKey);
    }

    // Expand spread arguments: `fn(a, ...arr, b)` → individual positional args.
    // Each spread `...arrExpr` is flattened at codegen time:
    //  - If the array length is a compile-time constant, emit that many element
    //    loads and insert them inline.
    //  - Otherwise emit a compile-time error (user-defined functions are
    //    fixed-arity, so the spread length must be statically known).
    //
    // Mixed entries: ArgEntry.expr != nullptr  → evaluate from AST at call time
    //                ArgEntry.val  != nullptr  → already-evaluated llvm::Value*
    struct ArgEntry {
        Expression* expr = nullptr; // raw AST pointer (non-owning)
        llvm::Value* val = nullptr; // pre-evaluated value (spread element)
    };
    std::vector<ArgEntry> flatArgEntries;

    bool hasSpread = false;
    for (auto& arg : expr->arguments) {
        if (arg->type == ASTNodeType::SPREAD_EXPR) {
            hasSpread = true;
            break;
        }
    }

    if (!hasSpread) {
        for (auto& arg : expr->arguments)
            flatArgEntries.push_back({arg.get(), nullptr});
    } else {
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        auto* i64Ty = getDefaultType();
        for (auto& arg : expr->arguments) {
            if (arg->type != ASTNodeType::SPREAD_EXPR) {
                flatArgEntries.push_back({arg.get(), nullptr});
                continue;
            }
            // Spread element: evaluate the source array, load each element.
            auto* se = static_cast<SpreadExpr*>(arg.get());
            llvm::Value* av = generateExpression(se->operand.get());
            llvm::Value* ap =
                av->getType()->isPointerTy() ? av : builder->CreateIntToPtr(av, ptrTy, "spread.call.arrptr");
            llvm::Value* al = emitLoadArrayLen(ap, "spread.call.len");
            auto* ci = llvm::dyn_cast<llvm::ConstantInt>(al);
            // Fallback: look up compile-time length from variable tracking.
            int64_t ctLen = -1;
            if (!ci && se->operand->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto& varName = static_cast<IdentifierExpr*>(se->operand.get())->name;
                auto lit = arrayCompTimeLens_.find(varName);
                if (lit != arrayCompTimeLens_.end())
                    ctLen = lit->second;
            } else if (se->operand->type == ASTNodeType::ARRAY_EXPR) {
                // Inline literal: count elements directly.
                auto* ae = static_cast<ArrayExpr*>(se->operand.get());
                bool hasNested = false;
                for (auto& el : ae->elements)
                    if (el->type == ASTNodeType::SPREAD_EXPR) {
                        hasNested = true;
                        break;
                    }
                if (!hasNested)
                    ctLen = static_cast<int64_t>(ae->elements.size());
            }
            uint64_t cnt = 0;
            if (ci) {
                cnt = ci->getZExtValue();
            } else if (ctLen >= 0) {
                cnt = static_cast<uint64_t>(ctLen);
            } else {
                codegenError("Spread '...' in a function call requires a compile-time-constant "
                             "array length (the callee has fixed arity). "
                             "Use a fixed-size array literal, e.g. [a, b, c].",
                             expr);
            }
            // Emit one element load per slot and stash as a pre-evaluated Value*.
            for (uint64_t k = 0; k < cnt; ++k) {
                llvm::Value* idx = llvm::ConstantInt::get(i64Ty, k + 1); // slot = k+1
                llvm::Value* elemPtr = builder->CreateInBoundsGEP(i64Ty, ap, idx, "spread.call.elemptr");
                llvm::Value* elem = builder->CreateAlignedLoad(i64Ty, elemPtr, llvm::MaybeAlign(8), "spread.call.elem");
                flatArgEntries.push_back({nullptr, elem});
            }
        }
    }

    size_t requiredArgs = callee->arg_size();
    if (declIt != functionDecls_.end()) {
        requiredArgs = declIt->second->requiredParameters();
    }
    if (flatArgEntries.size() < requiredArgs || flatArgEntries.size() > callee->arg_size()) {
        codegenError("Function '" + expr->callee + "' expects " +
                         (requiredArgs < callee->arg_size()
                              ? std::to_string(requiredArgs) + " to " + std::to_string(callee->arg_size())
                              : std::to_string(callee->arg_size())) +
                         " argument(s), but " + std::to_string(flatArgEntries.size()) + " provided",
                     expr);
    }

    // ── Clobber variables passed by address ──────────────────────────────────
    // When a variable's address is passed to a function (e.g. `f(&x)`), the
    // callee may write to it through the pointer.  Remove `x` from the
    // compile-time constant fold maps so that subsequent uses of `x` in the
    // same scope are not incorrectly constant-folded to the pre-call value.
    // This handles both BorrowExpr (`&x` explicit address-of) and
    // UnaryExpr with op "&" (alternate representation).
    for (auto& entry : flatArgEntries) {
        if (!entry.expr)
            continue;
        const Expression* arg = entry.expr;
        // Unwrap a single level of borrow/unary-& to get the variable name.
        std::string clobbered;
        if (arg->type == ASTNodeType::BORROW_EXPR) {
            const auto* borrow = static_cast<const BorrowExpr*>(arg);
            if (borrow->source && borrow->source->type == ASTNodeType::IDENTIFIER_EXPR)
                clobbered = static_cast<const IdentifierExpr*>(borrow->source.get())->name;
        } else if (arg->type == ASTNodeType::UNARY_EXPR) {
            const auto* u = static_cast<const UnaryExpr*>(arg);
            if (u->op == "&" && u->operand && u->operand->type == ASTNodeType::IDENTIFIER_EXPR)
                clobbered = static_cast<const IdentifierExpr*>(u->operand.get())->name;
        }
        if (!clobbered.empty()) {
            constIntFolds_.erase(llvm::StringRef(clobbered));
            constFloatFolds_.erase(llvm::StringRef(clobbered));
            constStringFolds_.erase(llvm::StringRef(clobbered));
            scopeComptimeInts_.erase(llvm::StringRef(clobbered));
        }
    }

    std::vector<llvm::Value*> args;
    args.reserve(callee->arg_size());
    for (size_t i = 0; i < callee->arg_size(); ++i) {
        llvm::Type* expectedTy = callee->getFunctionType()->getParamType(i);
        if (i < flatArgEntries.size()) {
            llvm::Value* argVal =
                flatArgEntries[i].val ? flatArgEntries[i].val : generateExpression(flatArgEntries[i].expr);
            // ── Array-to-pointer decay ────────────────────────────────────────
            // When the declared parameter type is a typed pointer (`ptr<T>` / `*T`)
            // and the supplied argument is an OmScript fat-pointer array, skip the
            // 16-byte {len, cap} header and pass the data pointer directly, matching
            // C array-decay semantics: `int arr[] → int *p`.
            if (argVal->getType()->isPointerTy() && declIt != functionDecls_.end() &&
                i < declIt->second->parameters.size()) {
                const std::string& paramAnn = declIt->second->parameters[i].typeName;
                // Detect a typed-pointer parameter (ptr<T> / *T → normalised to ptr<...>).
                bool isTypedPtrParam = paramAnn.size() > 4 && paramAnn.rfind("ptr<", 0) == 0 && paramAnn.back() == '>';
                if (isTypedPtrParam) {
                    // Detect an array argument: identifier in arrayVars_ or funcParamArrayTypes_.
                    bool isArrayArg = false;
                    if (flatArgEntries[i].expr &&
                        flatArgEntries[i].expr->type == ASTNodeType::IDENTIFIER_EXPR) {
                        const auto& argName = static_cast<IdentifierExpr*>(flatArgEntries[i].expr)->name;
                        if (arrayVars_.count(argName) ||
                            (funcParamArrayTypes_.count(expr->callee) &&
                             funcParamArrayTypes_.at(expr->callee).count(i)))
                            isArrayArg = true;
                    }
                    if (isArrayArg) {
                        // Skip the 8-byte length header to get the raw element data.
                        // OmScript array layout: [i64 len | i64 elem0 | i64 elem1 | ...]
                        // — only one header field, so element data starts at byte +8.
                        // (Strings have two fields {len, cap} = +16; arrays have one.)
                        auto* i8Ty = llvm::Type::getInt8Ty(*context);
                        auto* i64Ty = getDefaultType();
                        if (!argVal->getType()->isPointerTy())
                            argVal = builder->CreateIntToPtr(argVal,
                                         llvm::PointerType::getUnqual(*context), "arr.decay.itp");
                        argVal = builder->CreateInBoundsGEP(i8Ty, argVal,
                                     llvm::ConstantInt::get(i64Ty, 8), "arr.decay");
                    }
                }
            }
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
    if (callee->doesNotThrow()) {
        callResult->setDoesNotThrow();
    }
    // When the callee is @pure (speculatable + willreturn + readonly), mark the
    if (callee->hasFnAttribute(llvm::Attribute::Speculatable)) {
        callResult->addFnAttr(llvm::Attribute::Speculatable);
    }
    // Propagate callee function-level attributes to the call instruction so
    // LLVM's LICM, DSE, and alias-analysis passes can see them even before
    // inlining.  The attributes are already on the function definition, but
    // LLVM requires them on the CallInst too for interprocedural passes that
    // scan call uses rather than visiting the callee definition.
    if (callee->hasFnAttribute(llvm::Attribute::WillReturn))
        callResult->addFnAttr(llvm::Attribute::WillReturn);
    if (callee->hasFnAttribute(llvm::Attribute::NoSync))
        callResult->addFnAttr(llvm::Attribute::NoSync);
    if (callee->hasFnAttribute(llvm::Attribute::NoFree))
        callResult->addFnAttr(llvm::Attribute::NoFree);
    // Memory effects: copy the callee's memory attribute to the call so that
    // LICM can hoist calls with memory(none) or memory(read) out of loops.
    {
        llvm::MemoryEffects calleeMemFX = callee->getMemoryEffects();
        if (calleeMemFX != llvm::MemoryEffects::unknown())
            callResult->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, calleeMemFX));
    }
    // Emit !range metadata when a narrowed value range is known for this callee.
    if (optCtx_ && callee->getReturnType()->isIntegerTy(64)) {
        if (auto rng = optCtx_->returnRange(expr->callee)) {
            if (rng->isNarrowed() && !rng->isEmpty()) {
                llvm::Type* i64 = getDefaultType();
                // LLVM range metadata uses a half-open interval [lo, hi_excl).
                const int64_t hiExcl = (rng->hi == std::numeric_limits<int64_t>::max())
                                           ? std::numeric_limits<int64_t>::min()
                                           : rng->hi + 1;
                auto* rangeMD = llvm::MDNode::get(
                    *context, {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64, rng->lo, /*isSigned=*/true)),
                               llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64, hiExcl, /*isSigned=*/true))});
                callResult->setMetadata(llvm::LLVMContext::MD_range, rangeMD);
                // If the range lower bound is non-negative, mark the call
                if (rng->lo >= 0)
                    nonNegValues_.insert(callResult);
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
