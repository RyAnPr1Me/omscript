#pragma once
#ifndef CFCTRE_H
#define CFCTRE_H

/// @file cfctre.h
/// @brief CF-CTRE — Cross-Function Compile-Time Reasoning Engine.
///
/// A deterministic SSA-semantics interpreter embedded in the OmScript compiler
/// that executes pure functions across call boundaries at compile time.
/// Preserves pipeline + SIMD tile execution semantics and produces reduced
/// constant values (integers, strings, arrays via heap handles).
///
/// Pipeline position:
///   Frontend → Parser → AST Pre-passes → CF-CTRE → Code Generator → LLVM
///
/// Guarantees:
///   - Deterministic: same inputs always produce the same output.
///   - Memoised: (function, args) hash → result cached across all call sites.
///   - Fuel-bounded: terminates within kMaxInstructions.
///   - Depth-bounded: terminates within kMaxDepth call frames.
///   - No OS interaction: no I/O, no randomness, no filesystem access.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript {

// Forward-declare AST types (defined in ast.h).
class FunctionDecl;
class BlockStmt;
class Statement;
class Expression;
class Program;
class ForStmt;

// ─── CTArrayHandle ────────────────────────────────────────────────────────────
using CTArrayHandle = uint64_t;
static constexpr CTArrayHandle CT_NULL_HANDLE = 0;

// ─── CTValueKind ─────────────────────────────────────────────────────────────
enum class CTValueKind : uint8_t {
    CONCRETE_U64,     ///< Unsigned 64-bit integer (same storage as i64)
    CONCRETE_I64,     ///< Signed 64-bit integer
    CONCRETE_F64,     ///< 64-bit floating point
    CONCRETE_BOOL,    ///< Boolean 0/1 (stored as uint8)
    CONCRETE_STRING,  ///< Heap-owned UTF-8 string
    CONCRETE_ARRAY,   ///< Array stored in CTHeap; value holds handle
    UNINITIALIZED,    ///< Placeholder / missing value
};

// ─── CTValue ─────────────────────────────────────────────────────────────────
/// A single compile-time value produced or consumed by CF-CTRE.
/// Arrays are not stored inline — they are allocated on CTHeap and
/// referenced by an opaque CTArrayHandle.  This provides proper mutation
/// semantics: two CTValues that share a handle observe each other's writes.
struct CTValue {
    CTValueKind kind{CTValueKind::UNINITIALIZED};

    // Scalar payload (only one member is valid at a time, selected by `kind`).
    union {
        uint64_t u64;
        int64_t  i64;
        double   f64;
        bool     b;
    } scalar{};

    std::string   str;                      ///< Valid when kind == CONCRETE_STRING
    CTArrayHandle arr{CT_NULL_HANDLE};      ///< Valid when kind == CONCRETE_ARRAY

    // ── Factory helpers ───────────────────────────────────────────────────
    static CTValue fromU64(uint64_t v)    noexcept;
    static CTValue fromI64(int64_t  v)    noexcept;
    static CTValue fromF64(double   v)    noexcept;
    static CTValue fromBool(bool    v)    noexcept;
    static CTValue fromString(std::string s);
    static CTValue fromArray(CTArrayHandle h) noexcept;
    static CTValue uninit()               noexcept { return CTValue{}; }

    // ── Kind predicates ───────────────────────────────────────────────────
    bool isKnown()  const noexcept { return kind != CTValueKind::UNINITIALIZED; }
    bool isInt()    const noexcept {
        return kind == CTValueKind::CONCRETE_I64 || kind == CTValueKind::CONCRETE_U64;
    }
    bool isFloat()  const noexcept { return kind == CTValueKind::CONCRETE_F64;    }
    bool isString() const noexcept { return kind == CTValueKind::CONCRETE_STRING; }
    bool isArray()  const noexcept { return kind == CTValueKind::CONCRETE_ARRAY;  }
    bool isBool()   const noexcept { return kind == CTValueKind::CONCRETE_BOOL;   }

    // ── Accessors (assert-checked) ────────────────────────────────────────
    int64_t       asI64()  const noexcept;
    uint64_t      asU64()  const noexcept;
    double        asF64()  const noexcept;
    bool          asBool() const noexcept;
    const std::string& asStr()  const;
    CTArrayHandle asArr()  const noexcept;

    /// Truthy: non-zero int/float, non-empty string, valid array handle.
    bool isTruthy() const noexcept;

    /// Produce a deterministic string key for memoisation.
    std::string memoHash() const;

    /// Append the memo hash to an existing string (avoids temporary allocation
    /// when building composite memo keys).
    void appendMemoHash(std::string& out) const;

    bool operator==(const CTValue& o) const noexcept;
    bool operator!=(const CTValue& o) const noexcept { return !(*this == o); }
};

// ─── CTArray / CTHeap ────────────────────────────────────────────────────────

/// A fixed-length array stored on the compile-time heap.
struct CTArray {
    uint64_t             len{0};
    std::vector<CTValue> data;

    explicit CTArray(uint64_t n, const CTValue& fill = CTValue{});
    CTArray() = default;
};

/// Deterministic compile-time heap.
/// Uses monotone handles (never 0) and std::map for deterministic iteration.
/// Provides mutation semantics required for array element assignment during
/// compile-time function evaluation.
class CTHeap {
public:
    CTHeap()  = default;
    ~CTHeap() = default;

    /// Allocate a new array of `n` elements, each initialised to `fill`.
    CTArrayHandle alloc(uint64_t n, const CTValue& fill = CTValue{});

    /// Load element at `idx`; returns uninit CTValue if out-of-bounds.
    CTValue load(CTArrayHandle h, int64_t idx) const;

    /// Store `val` at element index `idx` (no-op if out-of-bounds).
    void store(CTArrayHandle h, int64_t idx, CTValue val);

    /// Append `val` to the end of array `h`.  Returns false if handle invalid.
    bool push(CTArrayHandle h, CTValue val);

    /// Return the logical length of array `h` (0 if not found).
    uint64_t length(CTArrayHandle h) const;

    /// True if the handle is valid (was allocated and not freed).
    bool exists(CTArrayHandle h) const;

    /// Free array `h`; future accesses return uninit.
    void freeArray(CTArrayHandle h);

    /// Read-only access to the underlying CTArray (nullptr if not found).
    const CTArray* get(CTArrayHandle h)  const;
    CTArray*       getMut(CTArrayHandle h);

    /// Current handle counter (useful for stats / debugging).
    uint64_t nextHandle() const noexcept { return nextHandle_; }

private:
    /// `std::map` (not unordered_map) guarantees deterministic iteration order
    /// over handles, which matters for snapshot/serialisation of heap state.
    std::map<CTArrayHandle, CTArray> arrays_;
    uint64_t nextHandle_{1};
};

// ─── CTFrame ─────────────────────────────────────────────────────────────────
/// Execution context for a single function invocation.
/// Holds local variable bindings, a pointer to the shared heap,
/// and control-flow signals (return, break, continue).
struct CTFrame {
    const FunctionDecl* fn{nullptr};     ///< Executing function (non-owning)

    /// Local variable map: name → CTValue.
    /// Array variables are stored as CTValues with kind==CONCRETE_ARRAY.
    std::unordered_map<std::string, CTValue> locals;

    CTHeap* heap{nullptr};               ///< Shared heap (non-owning)
    int     ip{0};                       ///< Logical instruction pointer

    // ── Control-flow signals ──────────────────────────────────────────────
    CTValue returnValue;
    bool    hasReturned{false};
    bool    didBreak{false};
    bool    didContinue{false};

    /// Last expression value in a statement context.
    /// Used as implicit return for `comptime { expr; }` blocks.
    CTValue lastBareExpr;
    bool    hasLastBare{false};
};

// ─── Memoisation key ─────────────────────────────────────────────────────────
struct CTMemoKey {
    std::string fnName;
    std::string argsHash;   ///< Concatenation of CTValue::memoHash() per arg
    bool operator==(const CTMemoKey& o) const noexcept {
        return fnName == o.fnName && argsHash == o.argsHash;
    }
};

struct CTMemoKeyHash {
    std::size_t operator()(const CTMemoKey& k) const noexcept {
        const std::size_t h1 = std::hash<std::string>{}(k.fnName);
        const std::size_t h2 = std::hash<std::string>{}(k.argsHash);
        return h1 ^ (h2 * 0x9e3779b185ebca87ULL);
    }
};

// ─── CTExecutionGraph ─────────────────────────────────────────────────────────
struct CTCallEdge {
    std::string callerName;
    std::string calleeName;
};

/// Interprocedural call graph of functions that participated in CT evaluation.
struct CTGraph {
    std::vector<std::string> nodes;   ///< Function names that were CT-evaluated
    std::vector<CTCallEdge>  edges;   ///< Discovered call-graph edges
};

// ─── CTEngine ─────────────────────────────────────────────────────────────────
/// CF-CTRE main engine.
///
/// Maintains a shared CTHeap and memoisation cache across the entire
/// compilation unit.  Each executeFunction call creates an isolated CTFrame
/// but shares the heap (so array results allocated by a callee remain valid
/// after the call returns).
///
/// SIMD / pipeline semantics:
///   When a PipelineStmt is encountered during CT evaluation the engine
///   partitions the iteration range into tiles of kSIMDLaneWidth elements
///   and executes each tile sequentially with a lane-mask for the last
///   (potentially partial) tile.  This matches the spec's VECTOR MODEL
///   requirement without introducing real parallelism.
class CTEngine {
public:
    // ── Compile-time limits ───────────────────────────────────────────────
    static constexpr int     kMaxDepth        = 128;
    static constexpr int64_t kMaxInstructions = 10'000'000LL;
    static constexpr int     kSIMDLaneWidth   = 8;   ///< Pipeline tile width

    explicit CTEngine();
    ~CTEngine() = default;

    // Non-copyable, non-movable (holds heap state).
    CTEngine(const CTEngine&)            = delete;
    CTEngine& operator=(const CTEngine&) = delete;

    // ── Function / constant registry ─────────────────────────────────────
    void registerFunction(const std::string& name, const FunctionDecl* fn);
    void registerGlobalConst(const std::string& name, CTValue val);
    void registerEnumConst(const std::string& name, int64_t val);
    void markPure(const std::string& name);
    bool isPure(const std::string& name) const;

    // ── Primary execution entry points ───────────────────────────────────

    /// Execute a named function with CT-known arguments.
    std::optional<CTValue> executeFunction(
        const std::string&          fnName,
        const std::vector<CTValue>& args);

    /// Execute a FunctionDecl* directly.
    std::optional<CTValue> executeFunction(
        const FunctionDecl*         fn,
        const std::vector<CTValue>& args);

    /// Evaluate a comptime block (BlockStmt).
    std::optional<CTValue> evalComptimeBlock(
        const BlockStmt*            body,
        const std::unordered_map<std::string, CTValue>& env = {});

    // ── Builtin evaluator (exposed for bridge with existing ConstValue) ───
    /// Evaluate a named built-in function with fully-resolved arguments.
    /// Returns std::nullopt for unknown builtins, I/O functions, or when
    /// argument types don't match expected signatures.
    ///
    /// Supported categories (~100 builtins):
    ///   • Math: abs, min, max, sign, clamp, pow, sqrt, gcd, lcm, log2, exp2
    ///   • Bitwise: popcount, clz, ctz, bitreverse, bswap, rotate_left/right
    ///   • Saturating: saturating_add, saturating_sub
    ///   • Character: is_alpha, is_digit, is_upper, is_lower, is_space, is_alnum,
    ///                is_even, is_odd, to_char, char_code
    ///   • String: len, str_len, str_eq, str_concat, str_find, str_substr,
    ///             str_upper, str_lower, str_contains, str_index_of, str_replace,
    ///             str_trim, str_starts_with, str_ends_with, str_repeat,
    ///             str_reverse, str_count, str_pad_left, str_pad_right,
    ///             str_chars, str_to_int, str_split, str_join,
    ///             to_string, number_to_string, string_to_number
    ///   • Array: push, pop, array_fill, array_concat, array_slice, array_copy,
    ///            array_contains, index_of, array_find, array_min, array_max,
    ///            array_last, array_product, sum, range, range_step,
    ///            reverse, sort, array_remove, array_insert,
    ///            array_any, array_every, array_count
    ///   • Float math: sin, cos, tan, asin, acos, atan, atan2, exp, cbrt,
    ///                  hypot, fma, copysign, min_float, max_float
    ///   • Fast/precise arithmetic: fast_add/sub/mul/div, precise_add/sub/mul/div
    ///   • Type casts: u64, i64, int, uint, u32, i32, u16, i16, u8, i8, bool
    std::optional<CTValue> evalBuiltin(
        const std::string&          name,
        const std::vector<CTValue>& args);

    // ── Heap access for result extraction ────────────────────────────────
    /// Extract all elements of an array into a std::vector.
    std::vector<CTValue> extractArray(CTArrayHandle h) const;
    /// Return the length of a heap array.
    uint64_t arrayLength(CTArrayHandle h) const;
    /// Direct heap access (read-only).
    const CTHeap& heap() const noexcept { return heap_; }

    // ── Specialisation key ────────────────────────────────────────────────
    std::string specializationKey(
        const std::string&          fnName,
        const std::vector<CTValue>& args) const;

    // ── Whole-program CF-CTRE pass ─────────────────────────────────────────
    /// Run the CF-CTRE analysis pass over an entire parsed Program.
    /// Populates the memoisation cache and pure-function registry.
    /// Called once by the compiler driver before code generation.
    void runPass(const Program* program);

    // ── Statistics ────────────────────────────────────────────────────────
    struct Stats {
        int64_t instructionsExecuted{0};
        int64_t functionCallsMemoized{0};
        int64_t arraysAllocated{0};
        int64_t specializationsHit{0};
        int64_t pipelineTilesExecuted{0};
        int64_t functionsRegistered{0};
        int64_t pureFunctionsDetected{0};
        int64_t loopsReasoned{0};  ///< For-loops handled by closed-form symbolic analysis
    };
    const Stats& stats()      const noexcept { return stats_; }
    void         resetStats()       noexcept { stats_ = {}; }

    /// Interprocedural call graph built during runPass.
    const CTGraph& graph() const noexcept { return graph_; }

private:
    // ── Internal evaluation ───────────────────────────────────────────────
    CTValue evalExpr(CTFrame& frame, const Expression* expr);
    bool    evalStmt(CTFrame& frame, const Statement*  stmt);
    bool    evalPipelineStmt(CTFrame& frame, const Statement* stmt);
    void    executeTile(CTFrame& frame,
                        const std::vector<const Statement*>& stageStmts,
                        int64_t baseIdx, int64_t n);
    bool    executeBody(CTFrame& frame, const BlockStmt* body);

    CTValue evalBinaryOp(const std::string& op, const CTValue& lhs, const CTValue& rhs);
    CTValue evalUnaryOp(const std::string& op, const CTValue& val);
    CTValue evalTypeCast(const std::string& name, const CTValue& val);
    CTValue evalCall(CTFrame& callerFrame,
                     const std::string& fnName,
                     const std::vector<CTValue>& args);

    /// Attempt to symbolically reduce a for-range loop body using closed-form
    /// arithmetic instead of iteration.  Returns true (and updates frame) if
    /// the body was fully handled; returns false to let the caller fall back
    /// to direct iteration.
    bool tryReasonForLoop(CTFrame& frame, const ForStmt* fs,
                          int64_t start, int64_t end, int64_t step, int64_t N);

    /// Snapshot an array from the heap into a new handle (for memoisation).
    CTArrayHandle snapshotArray(CTArrayHandle src);

    // ── State ─────────────────────────────────────────────────────────────
    CTHeap heap_;

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, CTValue>             globalConsts_;
    std::unordered_map<std::string, int64_t>             enumConsts_;
    std::unordered_set<std::string>                      pureFunctions_;

    /// Memoisation cache: CTMemoKey → snapshot CTValue.
    std::unordered_map<CTMemoKey, CTValue, CTMemoKeyHash> memoCache_;

    /// Recursion depth guard.
    int currentDepth_{0};

    /// Per-evaluation instruction budget (reset at each top-level entry).
    int64_t fuel_{0};

    Stats   stats_;
    CTGraph graph_;
};

} // namespace omscript

#endif // CFCTRE_H
