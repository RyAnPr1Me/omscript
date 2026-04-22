#pragma once

#ifndef EGRAPH_H
#define EGRAPH_H

/// @file egraph.h
/// @brief E-Graph data structures for equality saturation optimization.
///
/// This module implements an e-graph (equivalence graph) that enables
/// equality saturation — a technique where rewrite rules are applied
/// exhaustively to discover all equivalent program representations,
/// then a cost model extracts the globally optimal version.
///
/// Architecture:
///   ENode   — a single operation (e.g., Add, Mul, Const) with children
///   EClass  — an equivalence class of ENodes representing the same value
///   EGraph  — the full graph with union-find for merging equivalences
///
/// The e-graph avoids exponential blowup through:
///   - Node deduplication (hash-consing)
///   - Configurable node limits
///   - Iteration limits on saturation
///   - Efficient union-find with path compression

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace omscript {
namespace egraph {

/// Unique identifier for an e-class within the e-graph.
using ClassId = uint32_t;

/// Sentinel value for an invalid / uninitialized class ID.
static constexpr ClassId INVALID_CLASS = std::numeric_limits<ClassId>::max();

// ─────────────────────────────────────────────────────────────────────────────
// ENode — a single operation in the e-graph
// ─────────────────────────────────────────────────────────────────────────────

/// Operation tag for e-nodes.
enum class Op : uint8_t {
    // Constants & variables
    Const,   ///< Integer constant (value stored in ENode::value)
    ConstF,  ///< Float constant (value stored in ENode::fvalue)
    Var,     ///< Named variable (name stored in ENode::name)

    // Arithmetic
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,     ///< Unary negation

    // Bitwise
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    Shl,
    Shr,

    // Comparison
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,

    // Logical
    LogAnd,
    LogOr,
    LogNot,

    // Power / math
    Pow,
    Sqrt,

    // Special
    Ternary, ///< condition ? then : else (3 children)
    Call,    ///< Function call (name + children as args)
    Nop,     ///< No-op / identity placeholder
};

/// A single node in the e-graph representing one way to compute a value.
struct ENode {
    Op op;
    long long value = 0;        ///< For Const nodes
    double fvalue = 0.0;        ///< For ConstF nodes
    std::string name;           ///< For Var / Call nodes
    std::vector<ClassId> children;

    ENode() : op(Op::Nop) {}
    explicit ENode(Op o) : op(o) {}
    ENode(Op o, long long v) : op(o), value(v) {}
    ENode(Op o, double v) : op(o), fvalue(v) {}
    ENode(Op o, const std::string& n) : op(o), name(n) {}
    ENode(Op o, std::vector<ClassId> c) : op(o), children(std::move(c)) {}
    ENode(Op o, const std::string& n, std::vector<ClassId> c)
        : op(o), name(n), children(std::move(c)) {}

    bool operator==(const ENode& other) const {
        if (op != other.op || value != other.value || name != other.name ||
            children != other.children)
            return false;
        // Use bit-pattern equality for doubles so that two NaN constants with
        // the same bit pattern compare equal (and two with different payloads
        // compare unequal).  Plain `fvalue == other.fvalue` fails for NaN
        // because IEEE 754 mandates NaN != NaN, which would prevent
        // deduplication of identical NaN e-nodes in the hashcons table.
        uint64_t fb = 0, ob = 0;
        static_assert(sizeof(double) == sizeof(uint64_t));
        __builtin_memcpy(&fb, &fvalue, sizeof(double));
        __builtin_memcpy(&ob, &other.fvalue, sizeof(double));
        return fb == ob;
    }
};

/// Hash function for ENode (used for deduplication / hash-consing).
struct ENodeHash {
    size_t operator()(const ENode& n) const {
        size_t h = std::hash<uint8_t>{}(static_cast<uint8_t>(n.op));
        h ^= std::hash<long long>{}(n.value) + 0x9e3779b9 + (h << 6) + (h >> 2);
        // Use bit_cast-style hashing for double to avoid UB
        uint64_t fbits = 0;
        static_assert(sizeof(double) == sizeof(uint64_t), "double must be 64 bits");
        std::memcpy(&fbits, &n.fvalue, sizeof(double));
        h ^= std::hash<uint64_t>{}(fbits) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(n.name) + 0x9e3779b9 + (h << 6) + (h >> 2);
        for (auto c : n.children) {
            h ^= std::hash<ClassId>{}(c) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// EClass — an equivalence class of ENodes
// ─────────────────────────────────────────────────────────────────────────────

/// An equivalence class containing all known representations of a value.
struct EClass {
    ClassId id;
    std::vector<ENode> nodes;    ///< All equivalent representations

    // ── Analysis data for richer relational reasoning ─────────────────────
    std::optional<long long> constVal;  ///< Cached constant value if known
    bool isZero = false;                ///< True if class contains constant 0
    bool isOne = false;                 ///< True if class contains constant 1
    bool isNonNeg = false;              ///< True if all values are >= 0 (by analysis)
    bool isPowerOfTwo = false;          ///< True if value is a power of 2 (1,2,4,8,...)
    bool isBoolean = false;             ///< True if value is 0 or 1 (comparison result)
    bool isFloat = false;               ///< Provably float-typed (contains ConstF or
                                        ///< derives from a float-typed operand).  Used
                                        ///< by `fp_*` rewrite rules to avoid unifying
                                        ///< integer and float constants via congruence.
    bool isInt = false;                 ///< Provably integer-typed (contains Const,
                                        ///< Shl/Shr/BitOp output, or derives from int
                                        ///< operands).  Used by integer-only rules to
                                        ///< avoid firing on float-typed operands.
};

// ─────────────────────────────────────────────────────────────────────────────
// Pattern — for matching e-graph sub-expressions in rewrite rules
// ─────────────────────────────────────────────────────────────────────────────

/// A pattern variable (wildcard) or concrete operation for matching.
struct Pattern {
    enum class Kind { Wildcard, OpMatch };
    Kind kind;
    std::string wildcard;             ///< Name of wildcard variable (e.g., "?a")
    Op op = Op::Nop;                  ///< Required operation
    long long constVal = 0;           ///< Required constant value (for Const match)
    double constFVal = 0.0;           ///< Required float constant (for ConstF match)
    bool matchConst = false;          ///< If true, match specific constant value
    bool matchConstF = false;         ///< If true, match specific float constant
    std::vector<Pattern> children;    ///< Sub-patterns for children

    /// Create a wildcard pattern.
    static Pattern Wild(const std::string& name) {
        Pattern p;
        p.kind = Kind::Wildcard;
        p.wildcard = name;
        return p;
    }

    /// Create an operation pattern with sub-patterns.
    static Pattern OpPat(Op o, std::vector<Pattern> kids = {}) {
        Pattern p;
        p.kind = Kind::OpMatch;
        p.op = o;
        p.children = std::move(kids);
        return p;
    }

    /// Create a constant-match pattern for a specific integer.
    static Pattern ConstPat(long long val) {
        Pattern p;
        p.kind = Kind::OpMatch;
        p.op = Op::Const;
        p.constVal = val;
        p.matchConst = true;
        return p;
    }

    /// Create a constant-match pattern for a specific float.
    static Pattern ConstFPat(double val) {
        Pattern p;
        p.kind = Kind::OpMatch;
        p.op = Op::ConstF;
        p.constFVal = val;
        p.matchConstF = true;
        return p;
    }

    /// Create a pattern that matches any constant (without value constraint).
    static Pattern AnyConst() {
        Pattern p;
        p.kind = Kind::OpMatch;
        p.op = Op::Const;
        return p;
    }

private:
    Pattern() : kind(Kind::Wildcard) {}
};

/// A substitution maps wildcard names to e-class IDs.
using Subst = std::unordered_map<std::string, ClassId>;

// ─────────────────────────────────────────────────────────────────────────────
// RewriteRule — a pattern-based rewrite for equality saturation
// ─────────────────────────────────────────────────────────────────────────────

/// Callback that builds the right-hand side of a rewrite given a substitution.
/// Returns the class ID of the newly created expression.
class EGraph; // forward declaration
using RhsBuilder = std::function<ClassId(EGraph&, const Subst&)>;

/// Guard predicate for relational e-graph rules.  When set, the rule is
/// only applied if the guard returns true for the matched substitution.
/// This enables relational pattern matching: structural patterns matched
/// by the LHS are refined by semantic predicates on bound values (e.g.,
/// "matched constant is a power of two").
using RuleGuard = std::function<bool(const EGraph&, const Subst&)>;

/// A single rewrite rule: when the LHS pattern matches, apply the RHS builder.
struct RewriteRule {
    std::string name;          ///< Human-readable rule name (for diagnostics)
    Pattern lhs;               ///< Left-hand side pattern to match
    RhsBuilder rhs;            ///< Builds the replacement expression
    RuleGuard guard;           ///< Optional relational guard predicate

    RewriteRule(const std::string& n, Pattern l, RhsBuilder r)
        : name(n), lhs(std::move(l)), rhs(std::move(r)) {}

    /// Construct a guarded (relational) rewrite rule.
    RewriteRule(const std::string& n, Pattern l, RhsBuilder r, RuleGuard g)
        : name(n), lhs(std::move(l)), rhs(std::move(r)), guard(std::move(g)) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CostModel — assigns costs to e-nodes for optimal extraction
// ─────────────────────────────────────────────────────────────────────────────

/// Cost of a single operation (lower is better).
using Cost = double;
static constexpr Cost INFINITE_COST = 1e18;

/// Cost model that considers instruction latency, memory access, and
/// instruction-level parallelism for x86-64 targets.
///
/// In addition to per-node latency, the model carries the **register
/// budget** (number of architectural GPRs available to hold simultaneous
/// live values) and the **spill penalty** (added cost per excess live
/// value beyond the budget).  These two parameters drive the
/// register-pressure-aware extraction pass: when an extracted DAG would
/// exceed the budget, the extractor pays a spill penalty per excess
/// register and may flip the selection toward a node that produces an
/// equivalent value with a shallower (lower-pressure) sub-tree.
///
/// Defaults model a generic x86-64 host:
///   - 13 callee-clobbered + caller-saved GPRs available to a leaf
///     function (16 architectural - RSP - RBP - 1 frame/scratch reserve).
///   - 5 cycles per spill (conservative L1 round-trip for a stack slot
///     reload).
///
/// HGOE may construct a `CostModel` with target-specific values from a
/// resolved `MicroarchProfile` (e.g. AArch64 has 31 GPRs → regBudget=27,
/// in-order Cortex-A55 has higher spill cost ~8, etc.).
struct CostModel {
    /// Returns the cost of a single e-node, not counting children.
    /// The extraction algorithm adds children costs to get total cost.
    Cost nodeCost(const ENode& node) const;

    /// Number of architectural registers the extractor may assume are
    /// available to hold simultaneously-live values.  When the
    /// pressure of a candidate sub-DAG exceeds this, each excess
    /// live value costs `spillPenalty` cycles.  Set to 0 to disable
    /// register-pressure-aware extraction (the extractor then uses
    /// pure latency cost, matching the historical behaviour).
    unsigned regBudget = 13;

    /// Cost added per excess simultaneously-live value beyond
    /// `regBudget`.  Models a stack-slot spill+reload round-trip.
    /// A value of 0 disables the pressure penalty.
    Cost spillPenalty = 5.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — the core e-graph with union-find and equality saturation
// ─────────────────────────────────────────────────────────────────────────────

/// Configuration for the e-graph saturation loop.
struct SaturationConfig {
    size_t maxNodes = 50000;        ///< Stop adding nodes beyond this limit
    size_t maxIterations = 30;      ///< Maximum saturation iterations
    bool enableConstantFolding = true;
};

/// The e-graph: stores equivalence classes of expressions and supports
/// equality saturation via rewrite rules.
class EGraph {
public:
    EGraph();
    explicit EGraph(SaturationConfig config);

    // ── Node creation ────────────────────────────────────────────────────

    /// Add a new node to the e-graph, returning its canonical class ID.
    /// If an equivalent node already exists, returns the existing class.
    [[nodiscard]] ClassId add(ENode node);

    /// Create a constant integer node.
    [[nodiscard]] ClassId addConst(long long val);

    /// Create a constant float node.
    [[nodiscard]] ClassId addConstF(double val);

    /// Create a variable reference node.
    [[nodiscard]] ClassId addVar(const std::string& name);

    /// Create a binary operation node.
    [[nodiscard]] ClassId addBinOp(Op op, ClassId lhs, ClassId rhs);

    /// Create a unary operation node.
    [[nodiscard]] ClassId addUnaryOp(Op op, ClassId operand);

    // ── Union-find ───────────────────────────────────────────────────────

    /// Find the canonical representative of an e-class.
    [[nodiscard]] ClassId find(ClassId id);

    /// Merge two e-classes, returning the new canonical ID.
    ClassId merge(ClassId a, ClassId b);

    // ── Equality saturation ──────────────────────────────────────────────

    /// Run equality saturation with the given rewrite rules.
    /// Returns the number of iterations performed.
    size_t saturate(const std::vector<RewriteRule>& rules);

    /// Apply all rules once and return the number of new merges.
    [[nodiscard]] size_t applyRules(const std::vector<RewriteRule>& rules);

    // ── Pattern matching ─────────────────────────────────────────────────

    /// Match a pattern against all e-classes, returning substitutions.
    [[nodiscard]] std::vector<std::pair<ClassId, Subst>> match(const Pattern& pat) const;

    // ── Extraction ───────────────────────────────────────────────────────

    /// Extract the lowest-cost expression tree from the given root class.
    /// Returns the reconstructed ENode tree (with children as nested nodes).
    [[nodiscard]] ENode extract(ClassId root, const CostModel& model);

    // ── Accessors ────────────────────────────────────────────────────────

    /// Get the e-class for a given canonical ID.
    [[nodiscard]] const EClass& getClass(ClassId id) const;

    /// Number of e-classes (after canonicalization).
    [[nodiscard]] size_t numClasses() const;

    /// Total number of e-nodes across all classes.
    [[nodiscard]] size_t numNodes() const noexcept;

    /// Check if the node limit has been reached.
    [[nodiscard]] bool atNodeLimit() const noexcept;

    // ── Relational helpers ───────────────────────────────────────────

    /// Extract the integer constant value from an e-class, if one exists.
    /// Returns std::nullopt if the class contains no Const node.
    [[nodiscard]] std::optional<long long> getConstValue(ClassId cls) const;

    /// Extract the float constant value from an e-class, if one exists.
    /// Returns std::nullopt if the class contains no ConstF node.
    std::optional<double> getConstFValue(ClassId cls) const;

    /// Check if a class is known to be a power of two (1, 2, 4, 8, ...).
    bool isClassPowerOfTwo(ClassId cls) const;

    /// Check if a class is known to be boolean (0 or 1 only).
    bool isClassBoolean(ClassId cls) const;

    /// Check if a class is known to be non-negative (>= 0).
    bool isClassNonNeg(ClassId cls) const;

    /// Check if two e-classes are equivalent (represent the same value).
    bool areEquivalent(ClassId a, ClassId b);

    /// Check if an e-class contains a variable with the given name.
    bool hasVariable(ClassId cls, const std::string& name) const;

    /// Get the operation type of the best (cheapest/first) node in an e-class.
    std::optional<Op> getClassOp(ClassId cls) const;

private:
    /// Union-find parent array.
    mutable std::vector<ClassId> parent_;

    /// All e-classes, indexed by class ID.
    std::vector<EClass> classes_;

    /// Hash-cons map: deduplicates nodes to their canonical class.
    std::unordered_map<ENode, ClassId, ENodeHash> hashcons_;

    /// Configuration for saturation limits.
    SaturationConfig config_;

    /// Total node count across all classes.
    size_t totalNodes_ = 0;

    /// Monotonically-increasing counter of successful merges (i.e. calls to
    /// `merge(a, b)` where `find(a) != find(b)` so a real union happened).
    /// Incremented by `merge()` for every union path — including merges
    /// triggered by `foldConstants()` and pattern rules — so callers can
    /// detect "no merges since the last checkpoint" without instrumenting
    /// every path.  `saturate()` uses it to skip the per-iteration rebuild
    /// when no merges occurred, eliminating O(N) wasted work on the final
    /// fixpoint iteration and on rule sets that are quickly satisfied.
    size_t mergeCount_ = 0;

    /// Per-class use-list: `parents_[c]` lists every class id whose nodes
    /// reference `c` as a child.  Populated by `add()` and updated by
    /// `merge()` so the dirty-class rebuild can locate every class that
    /// could be affected by a union without scanning the entire graph.
    std::vector<std::vector<ClassId>> parents_;

    /// Worklist of classes whose hashcons entries may be stale because
    /// either they themselves received a merge or one of their children's
    /// canonical class id changed.  Drained by `rebuild()`.
    std::vector<ClassId> dirty_;
    std::vector<bool>    dirtyMark_;

    /// Per-op index of classes whose node-list contains at least one node
    /// of that op.  Populated by `add()` (one entry per new class+op
    /// combination) and extended by `merge()` (when class `b` carries an
    /// op into class `a` that wasn't already present).  Used by `match()`
    /// to skip the O(N) scan over all classes when the rule's root is an
    /// `OpMatch` pattern — we walk only the bucket for that op.
    ///
    /// Buckets may contain stale (non-canonical) ids that have since been
    /// merged away; `match()` canonicalises with `find()` and de-duplicates
    /// per call.  Entries are append-only because removing on every merge
    /// would make `merge()` linear in bucket size.
    static constexpr size_t kNumOps = static_cast<size_t>(Op::Nop) + 1;
    std::array<std::vector<ClassId>, kNumOps> classesByOp_;

    /// Generation-stamped scratch buffer used by `match()` (and any other
    /// per-call de-dup over canonical class ids) to avoid the per-call
    /// `std::vector<bool>(classes_.size(), false)` allocate-and-fill.  The
    /// buffer grows monotonically; each call bumps `matchSeenGen_` and
    /// considers an entry "seen" iff it equals the current generation.
    /// On generation overflow we zero the buffer once and reset the
    /// counter.  Mutable because `match()` is `const`.
    mutable std::vector<uint32_t> matchSeen_;
    mutable uint32_t              matchSeenGen_ = 0;

    /// Cheap O(1) test for "does the e-graph contain at least one
    /// e-class with a node of op `op`?" — used by `applyRules` to skip
    /// rules whose LHS root op cannot possibly match.  Buckets are
    /// append-only so a non-empty bucket is sufficient even if some
    /// entries are stale (the appended id was the canonical class at
    /// add-time and is still reachable via `find()`).
    bool hasOp(Op op) const noexcept {
        const auto idx = static_cast<size_t>(op);
        return idx < classesByOp_.size() && !classesByOp_[idx].empty();
    }

    /// Canonicalize an ENode's children to use canonical class IDs.
    ENode canonicalize(ENode node) const;

    /// Internal: rebuild the hashcons table after merges.
    /// Restricts re-canonicalization to classes that could have been
    /// affected by recent merges (those in `dirty_` plus their use-list
    /// closure).  When `dirty_` is empty, returns immediately — this is
    /// the common case on the final saturation iteration where the
    /// previous round produced no merges.
    void rebuild();

    /// Mark a class (and transitively every class that uses it) as dirty
    /// so that the next `rebuild()` re-canonicalizes their nodes.
    void markDirty(ClassId id);

    /// Internal: match a pattern against a specific e-class.
    bool matchClass(const Pattern& pat, ClassId cls, Subst& subst) const;

    /// Internal: match a pattern with commutative awareness for binary ops.
    /// For commutative operations (Add, Mul, BitAnd, BitOr, BitXor, Eq, Ne,
    /// LogAnd, LogOr), tries both orderings of children automatically.
    bool matchClassCommutative(const Pattern& pat, ClassId cls, Subst& subst) const;

    /// Internal: constant folding pass on an e-class.
    void foldConstants(ClassId cls);

public:
    /// Extract helper returning cost + best node per class.
    ///
    /// The extractor selects, for each e-class, the e-node that minimises
    /// the **effective cost** — a linear combination of latency cost and
    /// spill penalty derived from estimated register pressure.  When the
    /// host cost model has `spillPenalty == 0` or `regBudget == 0`, the
    /// pressure terms drop out and selection is purely latency-driven.
    ///
    /// Made public so that the AST bridge in `egraph_optimizer.cpp` can
    /// call `extractAll(root, model)` exactly once and reuse the
    /// resulting per-class best-node map for the linear-time recursive
    /// AST conversion.  The previous flow re-ran extractAll for every
    /// visited class via `EGraph::extract`, yielding O(N²) behaviour.
    struct ExtractionResult {
        Cost cost;          ///< Pure latency cost (sum of node + children costs,
                            ///< with DAG-sharing discount applied).
        Cost effCost = 0;   ///< Effective cost = cost + spillPenalty *
                            ///< max(0, regPressure - regBudget).  This is
                            ///< the metric the selection refinement
                            ///< minimises after the initial latency-only
                            ///< extraction has produced an estimate of
                            ///< parent counts (= sharing structure).
        ENode bestNode;
        unsigned depth = 0; ///< Critical-path depth (0 = leaf).  Used as a
                            ///< secondary tie-breaker after effCost/cost.
        unsigned regPressure = 1; ///< Sethi-Ullman simultaneous-live-value
                            ///< estimate for evaluating this sub-DAG.  For
                            ///< a leaf this is 1; for an internal node
                            ///< with k unshared children sorted by
                            ///< descending pressure p_i, it is
                            ///< sharedCount + max_i (p_i + i - 1), capturing
                            ///< the registers held for already-evaluated
                            ///< siblings while later siblings are computed.
                            ///< Shared children (parentCount > 1 in the
                            ///< extracted DAG) contribute exactly one
                            ///< register each because their result is
                            ///< computed once and reused.
    };
    std::unordered_map<ClassId, ExtractionResult>
    extractAll(ClassId root, const CostModel& model);
};

// ─────────────────────────────────────────────────────────────────────────────
// Rule library — pre-built rewrite rules for common optimizations
// ─────────────────────────────────────────────────────────────────────────────

/// Returns a comprehensive set of rewrite rules for algebraic simplification,
/// constant folding, strength reduction, and expression normalization.
std::vector<RewriteRule> getAlgebraicRules();

/// Returns advanced algebraic rules including nested arithmetic and
/// division/modulo simplifications.
std::vector<RewriteRule> getAdvancedAlgebraicRules();

/// Returns rewrite rules for comparison and branch simplification.
std::vector<RewriteRule> getComparisonRules();

/// Returns advanced comparison rules including boolean algebra,
/// comparison merging, redundant comparison elimination, and ternary
/// common-subexpression factoring.
std::vector<RewriteRule> getAdvancedComparisonRules();

/// Returns rewrite rules for bitwise operation simplification.
std::vector<RewriteRule> getBitwiseRules();

/// Returns advanced bitwise rules including arithmetic-bitwise identities
/// and relational guard-predicate rules (power-of-2 strength reduction).
std::vector<RewriteRule> getAdvancedBitwiseRules();

/// Returns relational rewrite rules with multi-variable guards,
/// including division strength reduction, conditional move optimization,
/// and cross-operation algebraic identities.
std::vector<RewriteRule> getRelationalRules();

/// Returns IEEE-754 compliant floating-point optimization rules.
/// These rules are safe for strict FP semantics (no NaN/Inf concerns
/// for the patterns they match).
std::vector<RewriteRule> getFloatingPointRules();

/// Strength reduction rules: replace expensive ops (mul, div) with cheaper
/// ones (shift, add, sub) for common small-constant multiplications.
std::vector<RewriteRule> getStrengthReductionRules();

/// Returns all optimization rules combined.
std::vector<RewriteRule> getAllRules();

} // namespace egraph

// Forward declarations of AST types used by the e-graph optimizer.
class Expression;
class FunctionDecl;
class Program;

// ─────────────────────────────────────────────────────────────────────────────
// AST-level optimizer — uses e-graph equality saturation on AST expressions
// ─────────────────────────────────────────────────────────────────────────────
namespace egraph {

/// Context for the AST-level e-graph optimizer.
///
/// Carries:
///   config         — saturation parameters (node limit, iteration limit, …)
///   pureUserFuncs  — names of user-defined functions that are pure.
///                    Expressions containing calls to these functions are
///                    eligible for e-graph optimization because a pure call
///                    with the same arguments is always equivalent, enabling
///                    algebraic rules (e.g. distributivity) to fire across
///                    call boundaries.
struct EGraphOptContext {
    SaturationConfig                       config;
    const std::unordered_set<std::string>* pureUserFuncs = nullptr; // non-owning
};

/// Optimize a single AST expression with an explicit context.
std::unique_ptr<Expression> optimizeExpression(const Expression* expr,
                                                const EGraphOptContext& ctx);

/// Optimize all expressions in a function body with an explicit context.
void optimizeFunction(FunctionDecl* func, const EGraphOptContext& ctx);

/// Optimize all functions in a program with an explicit context.
void optimizeProgram(Program* program, const EGraphOptContext& ctx);

/// Optimize a single AST expression using e-graph equality saturation.
/// Uses default saturation parameters and no user-function purity context.
std::unique_ptr<Expression> optimizeExpression(const Expression* expr);

/// Optimize all expressions in a function body.
void optimizeFunction(FunctionDecl* func);

/// Optimize all functions in a program.
void optimizeProgram(Program* program);

} // namespace egraph
} // namespace omscript

#endif // EGRAPH_H
