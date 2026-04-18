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
struct CostModel {
    /// Returns the cost of a single e-node, not counting children.
    /// The extraction algorithm adds children costs to get total cost.
    Cost nodeCost(const ENode& node) const;
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

    /// Canonicalize an ENode's children to use canonical class IDs.
    ENode canonicalize(ENode node) const;

    /// Internal: rebuild the hashcons table after merges.
    void rebuild();

    /// Internal: match a pattern against a specific e-class.
    bool matchClass(const Pattern& pat, ClassId cls, Subst& subst) const;

    /// Internal: match a pattern with commutative awareness for binary ops.
    /// For commutative operations (Add, Mul, BitAnd, BitOr, BitXor, Eq, Ne,
    /// LogAnd, LogOr), tries both orderings of children automatically.
    bool matchClassCommutative(const Pattern& pat, ClassId cls, Subst& subst) const;

    /// Internal: constant folding pass on an e-class.
    void foldConstants(ClassId cls);

    /// Internal: extract helper returning cost + best node per class.
    struct ExtractionResult {
        Cost cost;
        ENode bestNode;
        unsigned depth = 0; ///< Critical-path depth (0 = leaf). Used as tie-breaker:
                             ///< when two options have equal cost, prefer the shallower
                             ///< one to reduce register pressure and expose more ILP.
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

/// Optimize a single AST expression using e-graph equality saturation.
std::unique_ptr<Expression> optimizeExpression(const Expression* expr);

/// Optimize all expressions in a function body.
void optimizeFunction(FunctionDecl* func);

/// Optimize all functions in a program.
void optimizeProgram(Program* program);

} // namespace egraph
} // namespace omscript

#endif // EGRAPH_H
