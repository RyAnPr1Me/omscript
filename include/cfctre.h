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
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript {

// Forward-declare AST types (defined in ast.h).
class FunctionDecl;
class BlockStmt;
class IfStmt;
class Statement;
class Expression;
class Program;
class ForStmt;

// ─── CTArrayHandle ────────────────────────────────────────────────────────────
using CTArrayHandle = uint64_t;
static constexpr CTArrayHandle CT_NULL_HANDLE = 0;

// ═══════════════════════════════════════════════════════════════════════════════
// CTInterval — signed 64-bit integer interval lattice
// ═══════════════════════════════════════════════════════════════════════════════
//
// This is the abstract domain used by the CF-CTRE abstract interpreter.
// A CTInterval represents the set of all integer values a variable can hold
// at a given program point.  It is the foundation for answering:
//
//   Q1  "What values can this variable ever take?"
//   Q2  "Which paths are actually reachable?"
//   Q4  "Can this section be rewritten cheaper but equivalent?"
//   Q5  "Can I prove this bug cannot happen in any execution?"
//
// The domain is a complete lattice with:
//   BOTTOM  — no values (unreachable code / contradiction)
//   [lo,hi] — concrete interval (inclusive bounds)
//   TOP     — any 64-bit integer (unknown / not tracked)
//
// Transfer functions are SEMANTICALLY DERIVED from arithmetic definitions —
// not from pattern matching.  For example, the range of `a + b` when
// a ∈ [la, ha] and b ∈ [lb, hb] is [la+lb, ha+hb] (with overflow clamping);
// this follows from the definition of addition, not from recognizing the "+"
// node in the AST.
// ═══════════════════════════════════════════════════════════════════════════════
struct CTInterval {
    enum class Kind : uint8_t { BOTTOM, RANGE, TOP };
    Kind    kind{Kind::TOP};
    int64_t lo{std::numeric_limits<int64_t>::min()};
    int64_t hi{std::numeric_limits<int64_t>::max()};

    // ── Factory helpers ───────────────────────────────────────────────────────
    static CTInterval top()    noexcept { return {}; }
    static CTInterval bottom() noexcept { CTInterval a; a.kind = Kind::BOTTOM; return a; }
    static CTInterval exact(int64_t v) noexcept {
        CTInterval a; a.kind = Kind::RANGE; a.lo = a.hi = v; return a;
    }
    static CTInterval range(int64_t lo, int64_t hi) noexcept {
        if (lo > hi) return bottom();
        CTInterval a; a.kind = Kind::RANGE; a.lo = lo; a.hi = hi; return a;
    }

    // ── Kind predicates ───────────────────────────────────────────────────────
    bool isBottom()   const noexcept { return kind == Kind::BOTTOM; }
    bool isRange()    const noexcept { return kind == Kind::RANGE; }
    bool isTop()      const noexcept { return kind == Kind::TOP; }
    bool isConcrete() const noexcept { return kind == Kind::RANGE && lo == hi; }

    bool includes(int64_t v) const noexcept {
        if (isBottom()) return false;
        if (isTop())    return true;
        return lo <= v && v <= hi;
    }
    bool includesZero() const noexcept { return includes(0); }
    bool isNonNegative() const noexcept {
        if (isBottom()) return true;   // vacuously true
        if (isTop())    return false;
        return lo >= 0;
    }

    // ── Lattice operations ────────────────────────────────────────────────────

    /// Join (least upper bound): result contains all values from both.
    CTInterval join(const CTInterval& o) const noexcept {
        if (isBottom()) return o;
        if (o.isBottom()) return *this;
        if (isTop() || o.isTop()) return top();
        return range(std::min(lo, o.lo), std::max(hi, o.hi));
    }

    /// Meet (greatest lower bound, set intersection): result contains only
    /// values present in BOTH intervals.  Returns BOTTOM when ranges don't
    /// overlap.  Used to combine constraints from compound conditions like
    /// `A && B` where both narrowings apply on the then-branch.
    CTInterval intersect(const CTInterval& o) const noexcept {
        if (isBottom() || o.isBottom()) return bottom();
        if (isTop()) return o;
        if (o.isTop()) return *this;
        const int64_t newLo = std::max(lo, o.lo);
        const int64_t newHi = std::min(hi, o.hi);
        if (newLo > newHi) return bottom();
        return range(newLo, newHi);
    }

    /// Widening: called on loop back-edges to ensure convergence.
    /// If the new bound extends the old bound, widen it to ±∞.
    CTInterval widen(const CTInterval& prev) const noexcept {
        if (isBottom()) return prev;
        if (prev.isBottom()) return *this;
        if (isTop()) return top();
        if (prev.isTop()) return top();
        const int64_t newLo = (lo < prev.lo) ? std::numeric_limits<int64_t>::min() : prev.lo;
        const int64_t newHi = (hi > prev.hi) ? std::numeric_limits<int64_t>::max() : prev.hi;
        if (newLo == std::numeric_limits<int64_t>::min() &&
            newHi == std::numeric_limits<int64_t>::max()) return top();
        return range(newLo, newHi);
    }

    // ── Narrowing operators (for branch-condition specialisation) ─────────────
    //
    // These are applied AFTER a branch is taken to restrict the variable's
    // range to only the values consistent with the branch condition.
    // The result is the INTERSECTION of the current interval with the
    // constraint imposed by the condition — semantically derived from the
    // definition of the comparison operator.

    CTInterval narrowLT(int64_t bound) const noexcept {   // x < bound
        if (isBottom()) return *this;
        if (bound == std::numeric_limits<int64_t>::min()) return bottom();
        if (isTop()) return range(std::numeric_limits<int64_t>::min(), bound - 1);
        if (lo >= bound) return bottom();
        return range(lo, std::min(hi, bound - 1));
    }
    CTInterval narrowLE(int64_t bound) const noexcept {   // x <= bound
        if (isBottom()) return *this;
        if (isTop()) return range(std::numeric_limits<int64_t>::min(), bound);
        if (lo > bound) return bottom();
        return range(lo, std::min(hi, bound));
    }
    CTInterval narrowGT(int64_t bound) const noexcept {   // x > bound
        if (isBottom()) return *this;
        if (bound == std::numeric_limits<int64_t>::max()) return bottom();
        if (isTop()) return range(bound + 1, std::numeric_limits<int64_t>::max());
        if (hi <= bound) return bottom();
        return range(std::max(lo, bound + 1), hi);
    }
    CTInterval narrowGE(int64_t bound) const noexcept {   // x >= bound
        if (isBottom()) return *this;
        if (isTop()) return range(bound, std::numeric_limits<int64_t>::max());
        if (hi < bound) return bottom();
        return range(std::max(lo, bound), hi);
    }
    CTInterval narrowEQ(int64_t val) const noexcept {   // x == val
        if (!includes(val)) return bottom();
        return exact(val);
    }
    CTInterval narrowNE(int64_t val) const noexcept {   // x != val
        if (!includes(val)) return *this;
        if (isConcrete()) return bottom();        // only value, now excluded
        if (isRange() && lo == val) return range(lo + 1, hi);
        if (isRange() && hi == val) return range(lo, hi - 1);
        return *this;  // conservative — can't express non-contiguous sets
    }

    // ── Comparison results ────────────────────────────────────────────────────
    //
    // These derive whether a comparison of two intervals is always true,
    // always false, or indeterminate — used to answer Q2 (reachability).
    // Semantically: `[la,ha] op [lb,hb]` is ALWAYS_TRUE when every value
    // in A relates to every value in B by op, ALWAYS_FALSE when no pair
    // satisfies op, and UNKNOWN otherwise.

    enum class CmpResult { ALWAYS_TRUE, ALWAYS_FALSE, UNKNOWN };

    CmpResult cmpLT(const CTInterval& o) const noexcept;   // this < o
    CmpResult cmpLE(const CTInterval& o) const noexcept;   // this <= o
    CmpResult cmpGT(const CTInterval& o) const noexcept;   // this > o
    CmpResult cmpGE(const CTInterval& o) const noexcept;   // this >= o
    CmpResult cmpEQ(const CTInterval& o) const noexcept;   // this == o
    CmpResult cmpNE(const CTInterval& o) const noexcept;   // this != o

    // ── Arithmetic transfer functions ─────────────────────────────────────────
    //
    // Each function computes the interval that contains ALL possible results
    // of the operation when the operands range over their respective intervals.
    // Results are SEMANTICALLY DERIVED from the definitions of the operations,
    // not from pattern matching on AST node types.

    CTInterval opAdd(const CTInterval& o) const noexcept;
    CTInterval opSub(const CTInterval& o) const noexcept;
    CTInterval opMul(const CTInterval& o) const noexcept;
    CTInterval opDiv(const CTInterval& o) const noexcept;  // returns TOP if divisor ∋ 0
    CTInterval opMod(const CTInterval& o) const noexcept;
    CTInterval opShl(const CTInterval& o) const noexcept;
    CTInterval opShr(const CTInterval& o) const noexcept;
    CTInterval opBitAnd(const CTInterval& o) const noexcept;
    CTInterval opBitOr(const CTInterval& o) const noexcept;
    CTInterval opBitXor(const CTInterval& o) const noexcept;
    CTInterval opNeg() const noexcept;
    CTInterval opAbs() const noexcept;
};

// ─── CTAbstractEnv ────────────────────────────────────────────────────────────
/// Per-function abstract environment: maps variable name → CTInterval at the
/// current program point in the abstract interpretation.
using CTAbstractEnv = std::unordered_map<std::string, CTInterval>;

// ─── CTAnalysisResult ─────────────────────────────────────────────────────────
/// Complete per-function analysis result produced by CTAbstractInterpreter.
/// Answers all five reasoning questions for a single function.
struct CTAnalysisResult {
    /// Q1: variable ranges at every program point (stored as exit-state
    ///     for the whole function, and also locally within loops/branches).
    CTAbstractEnv exitEnv;

    /// Q2: if-statements where the then-branch is provably unreachable
    ///     (condition is ALWAYS_FALSE from interval analysis).
    std::unordered_set<const Statement*> deadThenBranches;

    /// Q2: if-statements where the else-branch is provably unreachable
    ///     (condition is ALWAYS_TRUE from interval analysis).
    std::unordered_set<const Statement*> deadElseBranches;

    /// Q3: expressions proven to produce the same value as an earlier
    ///     computation in this function — candidates for CSE.
    ///     Maps expression pointer → its canonical CTInterval result.
    std::unordered_map<const Expression*, CTInterval> redundantExprs;

    /// Q4: binary expressions whose operand ranges make a cheaper-but-equivalent
    ///     rewrite unconditionally safe (e.g. x/2 → x>>1 when x ≥ 0).
    ///     The value is the name of the safe alternative operator (">>", "&", etc.).
    std::unordered_map<const Expression*, std::string> cheaperRewrites;

    /// Q5: array index expressions proven always in-bounds.
    std::unordered_set<const Expression*> safeArrayAccesses;

    /// Q5: binary div/mod expressions proven safe (divisor never zero).
    std::unordered_set<const Expression*> safeDivisions;

    /// Q5: binary add/sub/mul expressions proven to never overflow int64.
    std::unordered_set<const Expression*> safeArithmetic;
};

// ─── CTValueKind ─────────────────────────────────────────────────────────────
enum class CTValueKind : uint8_t {
    CONCRETE_U64,     ///< Unsigned 64-bit integer (same storage as i64)
    CONCRETE_I64,     ///< Signed 64-bit integer
    CONCRETE_F64,     ///< 64-bit floating point
    CONCRETE_BOOL,    ///< Boolean 0/1 (stored as uint8)
    CONCRETE_STRING,  ///< Heap-owned UTF-8 string
    CONCRETE_ARRAY,   ///< Array stored in CTHeap; value holds handle
    UNINITIALIZED,    ///< Placeholder / missing value
    SYMBOLIC,         ///< Unknown value for partial evaluation (path-sensitive folding)
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
    uint32_t      symId{0};                 ///< Non-zero for SYMBOLIC values; unique per created symbolic

    // ── Factory helpers ───────────────────────────────────────────────────
    static CTValue fromU64(uint64_t v)    noexcept;
    static CTValue fromI64(int64_t  v)    noexcept;
    static CTValue fromF64(double   v)    noexcept;
    static CTValue fromBool(bool    v)    noexcept;
    static CTValue fromString(std::string s);
    static CTValue fromArray(CTArrayHandle h) noexcept;
    static CTValue uninit()    noexcept { return CTValue{}; }
    static CTValue symbolic()  noexcept; ///< Returns a fresh symbolic value with unique symId

    // ── Kind predicates ───────────────────────────────────────────────────
    bool isKnown()     const noexcept { return kind != CTValueKind::UNINITIALIZED; }
    bool isSymbolic()  const noexcept { return kind == CTValueKind::SYMBOLIC; }
    bool isConcrete()  const noexcept { return isKnown() && !isSymbolic(); }
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

    /// True if no store() or push() has been called on this handle since alloc().
    /// Immutable arrays can be safely aliased (snapshotArray is a no-op for them).
    bool isImmutable(CTArrayHandle h) const noexcept;

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
    /// Handles that have been mutated via store() or push() after alloc().
    std::unordered_set<CTArrayHandle> mutableHandles_;
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

    /// Constraint set: varName → narrow interval from the branch condition that
    /// led into this frame path.  Applied in IDENTIFIER_EXPR to concretise symbolic
    /// variables whose range has been narrowed to a single value.
    std::unordered_map<std::string, CTInterval> constraints;
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
        int64_t loopsReasoned{0};    ///< For-loops handled by closed-form symbolic analysis
        int64_t branchMerges{0};     ///< Symbolic-IF diamond eliminations (path-sensitive folding)
        int64_t ternaryMerges{0};    ///< Symbolic-ternary both-arm agreement folds
        int64_t uniformReturnFunctionsFound{0}; ///< Pure functions whose return is always the same constant
        int64_t deadFunctionsDetected{0};       ///< Functions unreachable from any entry point
        // Phase 9 (abstract interpretation) counters
        int64_t deadBranchesEliminated{0};      ///< Q2: branches proven always-dead
        int64_t safeArrayAccesses{0};           ///< Q5: array accesses proven in-bounds
        int64_t safeDivisions{0};               ///< Q5: divisions proven non-zero divisor
        int64_t cheaperRewritesFound{0};        ///< Q4: range-conditioned strength reductions
        // Phase A/B/C/D/F additional stats
        int64_t algebraicFolds{0};              ///< Algebraic identity simplifications (x+0, x-x, etc.)
        int64_t constraintFolds{0};             ///< Branch-constraint folds (symbolic → concrete from narrowing)
        int64_t partialEvalFolds{0};            ///< Partial specialisation cache hits
        int64_t callSitesFolded{0};             ///< Phase 10: literal call sites pre-evaluated
    };
    const Stats& stats()      const noexcept { return stats_; }
    void         resetStats()       noexcept { stats_ = {}; }

    /// Set of user functions that produced ≥1 concrete fold result (i.e., they
    /// were successfully CT-evaluated with concrete arguments).  The codegen
    /// uses this to apply InlineHint to these functions so LLVM will prefer to
    /// inline them, exposing the same constant-folding opportunities at
    /// remaining runtime call sites.
    const std::unordered_set<std::string>& foldableCallees() const noexcept {
        return foldableCallees_;
    }

    /// Pure functions that always return the same constant value regardless of
    /// their arguments (detected by evaluating with symbolic parameters in Phase 6).
    /// Key = function name; Value = the uniform return value.
    /// The codegen uses this to fold all call sites to the constant and to
    /// populate the OptimizationContext constant-return facts.
    const std::unordered_map<std::string, CTValue>& uniformReturnValues() const noexcept {
        return uniformReturnValues_;
    }

    /// Functions that are unreachable from any program entry point (detected by
    /// BFS over the call graph in Phase 7).  The codegen marks these `cold` so
    /// LLVM's DCE / GlobalDCE removes them, and they are excluded from inlining.
    const std::unordered_set<std::string>& deadFunctions() const noexcept {
        return deadFunctions_;
    }

    /// Interprocedural call graph built during runPass.
    const CTGraph& graph() const noexcept { return graph_; }

    // ── Abstract interpretation results (Phase 9) ─────────────────────────
    //
    // After runPass, the abstract interpreter has analysed every function.
    // Codegen queries these maps to:
    //   - Skip generating dead branches (Q2)
    //   - Skip bounds/overflow checks for proven-safe operations (Q5)
    //   - Apply range-conditioned rewrites the e-graph can't safely apply
    //     without range information (Q4)

    /// True if the then-branch of `ifStmt` is provably never executed.
    bool isThenBranchDead(const Statement* ifStmt) const noexcept {
        return analysisDeadThen_.count(ifStmt) > 0;
    }
    /// True if the else-branch of `ifStmt` is provably never executed.
    bool isElseBranchDead(const Statement* ifStmt) const noexcept {
        return analysisDeadElse_.count(ifStmt) > 0;
    }
    /// True if the array-index expression `indexExpr` is proven always in-bounds.
    bool isArrayAccessSafe(const Expression* indexExpr) const noexcept {
        return analysisSafeArrayAccess_.count(indexExpr) > 0;
    }
    /// True if the div/mod expression `divExpr` is proven to never divide by zero.
    bool isDivisionSafe(const Expression* divExpr) const noexcept {
        return analysisSafeDiv_.count(divExpr) > 0;
    }
    /// True if the add/sub/mul `arithExpr` is proven to never overflow int64.
    bool isArithmeticSafe(const Expression* arithExpr) const noexcept {
        return analysisSafeArith_.count(arithExpr) > 0;
    }
    /// Return the range-conditioned cheaper alternative operator for `binExpr`,
    /// or empty string if none applies.
    /// Example: for `x / 4` where x ≥ 0, returns ">>" (so codegen emits x >> 2).
    const std::string& cheaperRewrite(const Expression* binExpr) const noexcept {
        static const std::string kEmpty;
        auto it = analysisCheaperRewrites_.find(binExpr);
        return (it != analysisCheaperRewrites_.end()) ? it->second : kEmpty;
    }
    /// Return the abstract interval for variable `varName` in function `fnName`
    /// at function exit.  Returns CTInterval::top() if unknown.
    CTInterval getExitRange(const std::string& fnName,
                            const std::string& varName) const noexcept;

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

    /// Narrow variable ranges along the then/else branches of an `if`
    /// based on simple comparison constraints in the condition expression.
    /// Updates `thenF.constraints` / `elseF.constraints` in place.
    void narrowBranchConstraints(const Expression* cond,
                                 CTFrame& thenF,
                                 CTFrame& elseF) const;

    /// Build a partial-specialisation cache key for (fnName, args), supporting
    /// mixed concrete/symbolic argument vectors.
    std::string partialSpecKey(const std::string& fnName,
                               const std::vector<CTValue>& args) const;

    // ── Phase 9: Abstract interpretation (CTAbstractInterpreter) ─────────
    // Called from runPass; populates the analysis* maps below.
    void runAbstractInterpretation(const Program* program);

    // ── State ─────────────────────────────────────────────────────────────
    CTHeap heap_;

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, CTValue>             globalConsts_;
    std::unordered_map<std::string, int64_t>             enumConsts_;
    std::unordered_set<std::string>                      pureFunctions_;

    /// Functions that produced ≥1 concrete fold (memoised with concrete args).
    /// Populated in executeFunction; read by codegen to apply InlineHint.
    std::unordered_set<std::string>                      foldableCallees_;

    /// Phase 6: pure functions whose return value is always the same constant
    /// (proven by evaluating with symbolic arguments).
    std::unordered_map<std::string, CTValue>             uniformReturnValues_;

    /// Phase 7: functions unreachable from any entry point via BFS on the call graph.
    std::unordered_set<std::string>                      deadFunctions_;

    // ── Phase 9 analysis results ──────────────────────────────────────────
    /// Q2: if-statements where the then-branch is always-dead.
    std::unordered_set<const Statement*>  analysisDeadThen_;
    /// Q2: if-statements where the else-branch is always-dead.
    std::unordered_set<const Statement*>  analysisDeadElse_;
    /// Q5: array index expressions proven always in-bounds.
    std::unordered_set<const Expression*> analysisSafeArrayAccess_;
    /// Q5: div/mod expressions proven non-zero divisor.
    std::unordered_set<const Expression*> analysisSafeDiv_;
    /// Q5: arithmetic expressions proven no-overflow.
    std::unordered_set<const Expression*> analysisSafeArith_;
    /// Q4: range-conditioned cheaper rewrites.
    std::unordered_map<const Expression*, std::string> analysisCheaperRewrites_;
    /// Q1: per-function exit ranges  fnName → (varName → CTInterval).
    std::unordered_map<std::string, CTAbstractEnv>     analysisExitEnvs_;

    /// Memoisation cache: CTMemoKey → snapshot CTValue.
    std::unordered_map<CTMemoKey, CTValue, CTMemoKeyHash> memoCache_;

    /// Partial-specialization cache: partialSpecKey → concrete CTValue.
    /// Caches concrete results produced when some args are symbolic,
    /// keyed by fnName + per-arg pattern (concrete hash OR symId for symbolic args).
    std::unordered_map<std::string, CTValue> specCache_;

    /// Recursion depth guard.
    int currentDepth_{0};

    /// Per-evaluation instruction budget (reset at each top-level entry).
    int64_t fuel_{0};

    Stats   stats_;
    CTGraph graph_;
};

// ═══════════════════════════════════════════════════════════════════════════════
// CTAbstractInterpreter — per-function abstract interpretation engine
// ═══════════════════════════════════════════════════════════════════════════════
//
// This class performs flow-sensitive abstract interpretation of a single
// OmScript function body using the CTInterval lattice as the abstract domain.
//
// The analysis is SEMANTICALLY DRIVEN — it computes abstract values by
// applying transfer functions to each operation, not by pattern-matching
// on the AST structure.  The same analysis that handles:
//   for i in 0..n  → i ∈ [0, n-1]
// also handles:
//   let start = f()  →  start = CT-value of f()
//   for i in start..end  →  i ∈ [start, end-1]  (if both are concrete)
// because both cases go through the same `analyzeExpr` → `CTInterval` path.
//
// Outputs per function:
//   - Exit abstract environment (Q1)
//   - Dead branch annotations (Q2)
//   - Redundant expression candidates (Q3)
//   - Range-conditioned cheaper rewrites (Q4)
//   - Proven-safe operation sets (Q5)
// ═══════════════════════════════════════════════════════════════════════════════
class CTAbstractInterpreter {
public:
    explicit CTAbstractInterpreter(CTEngine& eng,
                                   const std::unordered_map<std::string, CTValue>& globals,
                                   const std::unordered_map<std::string, int64_t>& enums)
        : engine_(eng), globals_(globals), enums_(enums) {}

    /// Analyse a single function and return the complete result.
    /// If the function body is nullptr, returns a default (all-top) result.
    CTAnalysisResult analyzeFunction(const FunctionDecl* fn);

private:
    CTEngine& engine_;
    const std::unordered_map<std::string, CTValue>& globals_;
    const std::unordered_map<std::string, int64_t>& enums_;

    // ── Expression analysis ───────────────────────────────────────────────
    /// Compute the abstract interval for expression `e` in environment `env`.
    /// Returns CTInterval::top() when the value cannot be determined.
    CTInterval analyzeExpr(const Expression* e, const CTAbstractEnv& env,
                           CTAnalysisResult& result);

    // ── Statement analysis ────────────────────────────────────────────────
    /// Analyse `s` and update `env` to the post-state.
    /// Returns false if the rest of the block is unreachable (return/break).
    bool analyzeStmt(const Statement* s, CTAbstractEnv& env,
                     CTAnalysisResult& result);

    void analyzeBlock(const BlockStmt* b, CTAbstractEnv& env,
                      CTAnalysisResult& result);

    // ── Condition narrowing ───────────────────────────────────────────────
    /// Given a branch condition, narrow the two environments (then/else) to
    /// reflect the constraint implied by the condition being true / false.
    /// Both environments start as copies of the pre-condition environment.
    void narrowCondition(const Expression* cond,
                         CTAbstractEnv& thenEnv, CTAbstractEnv& elseEnv);

    // ── Environment join ──────────────────────────────────────────────────
    /// Join two environments at a control-flow merge point.
    static CTAbstractEnv joinEnvs(const CTAbstractEnv& a, const CTAbstractEnv& b);

    // ── Array length tracking ─────────────────────────────────────────────
    /// Return the known compile-time length of an array variable, or -1 if unknown.
    int64_t getArrayLength(const std::string& varName, const CTAbstractEnv& env) const;

    /// Map from variable name to known array length (populated by VarDecl analysis).
    std::unordered_map<std::string, int64_t> arrayLengths_;
};

} // namespace omscript

#endif // CFCTRE_H
