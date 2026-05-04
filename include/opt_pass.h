#pragma once
#ifndef OPT_PASS_H
#define OPT_PASS_H

/// @file opt_pass.h
/// @brief Declarative pass system for the OmScript optimization pipeline.
///
/// Every optimization pass — whether it performs analysis, AST transformation,
/// IR building, or backend tuning — is described by a PassMetadata struct.
/// The PassRegistry is a compile-time catalog of all passes; the
/// OptimizationOrchestrator uses it to enforce ordering, detect missing
/// prerequisites, and report which passes ran.
///
/// Design goals
/// ============
/// • Composable: passes declare their inputs and outputs as named AnalysisFact
///   tokens.  The registry can topologically sort any subset of passes.
/// • Deterministic: tie-breaking order is determined by the stable numeric
///   PassId, not by insertion order or pointer values.
/// • Extensible: new passes are registered via `PassRegistry::registerPass()`.
///   The `PassId::k*` constants defined in `opt_orchestrator.cpp` provide
///   stable, named references to each registered pass.
/// • Zero-overhead when disabled: all metadata lives in read-only data
///   sections; no virtual dispatch at runtime.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// PassPhase — the pipeline stage a pass belongs to
// ─────────────────────────────────────────────────────────────────────────────
enum class PassPhase : uint8_t {
    Preprocessing,        ///< Source-level analysis before semantic checks
    EvaluationAnalysis,   ///< Purity detection, effect inference, CF-CTRE
    ASTTransform,         ///< AST rewrites (e-graph, OPTMAX folder, loop fusion)
    IRPipeline,           ///< Reserved: LLVM pass-manager pipeline (O1/O2/O3 + OPTMAX)
    BackendTuning,        ///< Reserved: Superoptimizer, HGOE, post-pipeline cleanup
};

// ─────────────────────────────────────────────────────────────────────────────
// PassKind — the semantic role of a pass
// ─────────────────────────────────────────────────────────────────────────────
enum class PassKind : uint8_t {
    Analysis,            ///< Pure analysis — reads AST/IR but never modifies it
    SemanticTransform,   ///< Semantics-preserving rewrite (must be always-correct)
    CostTransform,       ///< Optional, cost-driven rewrite (may be skipped at O0)
};

// ─────────────────────────────────────────────────────────────────────────────
// AnalysisFact — stable identifiers for pieces of analysis state
// ─────────────────────────────────────────────────────────────────────────────
///
/// Each pass lists which facts it reads (requires), which it writes (provides),
/// and which it invalidates when it modifies the program representation.
/// These strings are cheap to compare (short, interned by the registry).
namespace AnalysisFact {
    inline constexpr const char* kPreflightCheck  = "preflight_check";
    inline constexpr const char* kStringTypes     = "string_types";
    inline constexpr const char* kArrayTypes      = "array_types";
    inline constexpr const char* kConstantReturns = "constant_returns";
    inline constexpr const char* kPurity          = "purity";
    inline constexpr const char* kEffects         = "effects";
    inline constexpr const char* kERSL            = "ersl";
    inline constexpr const char* kSynthesis       = "synthesis";
    inline constexpr const char* kCFCTRE          = "cfctre";
    inline constexpr const char* kEGraph          = "egraph";
    inline constexpr const char* kRangeAnalysis   = "range_analysis";
    inline constexpr const char* kRLC             = "rlc";
    inline constexpr const char* kDCE             = "dce";
    inline constexpr const char* kCSE             = "cse";
    inline constexpr const char* kAlgSimp         = "alg_simp";
    inline constexpr const char* kCopyProp        = "copy_prop";
    inline constexpr const char* kWidthLegalization = "width_legalization";
    inline constexpr const char* kWidthOpt           = "width_opt";
    inline constexpr const char* kUniqueness         = "uniqueness";
    inline constexpr const char* kBorrowCheck        = "borrow_check";
} // namespace AnalysisFact

// ─────────────────────────────────────────────────────────────────────────────
// Binary-operator predicates — shared utilities for analysis passes
// ─────────────────────────────────────────────────────────────────────────────
///
/// Centralised here so that CSE, AlgSimp, CopyProp, e-graph canonicalisation,
/// and any future passes can share a single definition instead of maintaining
/// per-pass local static sets.

/// True when the operator is commutative: `a OP b` ≡ `b OP a`.
/// Used by CSE to normalise expression keys and by the e-graph's pattern
/// matcher to try swapped operands.
inline bool isCommutativeOp(const std::string& op) noexcept {
    return op == "+"  || op == "*"  || op == "&"  ||
           op == "|"  || op == "^"  || op == "==" || op == "!=";
}

/// True when applying OP to integer/bitwise operands has no observable side
/// effect (no I/O, no heap writes).  Used by CSE to decide whether a
/// subexpression is safe to hoist and by AlgSimp to decide what can be
/// reordered.
///
/// Note: floating-point operators are NOT listed here because of IEEE-754
/// side-effects (NaN propagation, signed-zero, rounding modes).  The CSE
/// and AlgSimp passes conservatively refuse to hoist expressions involving
/// float literals.
inline bool isPureBinaryOp(const std::string& op) noexcept {
    return op == "+"  || op == "-"  || op == "*"  || op == "/"  ||
           op == "%"  || op == "&"  || op == "|"  || op == "^"  ||
           op == "<<" || op == ">>" || op == "==" || op == "!=" ||
           op == "<"  || op == "<=" || op == ">"  || op == ">=";
}

// ─────────────────────────────────────────────────────────────────────────────
// PassMetadata — static descriptor for one pass
// ─────────────────────────────────────────────────────────────────────────────
///
/// Instances are stored in the PassRegistry's internal vector.
/// All strings are string-view style; they must outlive the registry.
struct PassMetadata {
    uint32_t     id;          ///< Unique, stable numeric identifier
    const char*  name;        ///< Short human-readable identifier (e.g. "purity")
    const char*  description; ///< One-line description for verbose output
    PassPhase    phase;       ///< Which pipeline stage this pass belongs to
    PassKind     kind;        ///< Role: analysis / semantic transform / cost transform

    /// Analysis facts this pass REQUIRES to be valid before it runs.
    std::vector<const char*> requires_;

    /// Analysis facts this pass PRODUCES (marks valid after it runs).
    std::vector<const char*> provides_;

    /// Analysis facts this pass INVALIDATES (marks stale when it transforms).
    std::vector<const char*> invalidates_;
};

// ─────────────────────────────────────────────────────────────────────────────
// PassRegistry — compile-time catalog of all passes
// ─────────────────────────────────────────────────────────────────────────────
class PassRegistry {
public:
    /// Return the global singleton registry.
    static PassRegistry& instance();

    /// Register a pass.  Called during static initialization via the
    /// OMSC_REGISTER_PASS macro.  Returns the assigned ID.
    uint32_t registerPass(PassMetadata meta);

    /// Look up a pass by ID.  Returns nullptr if not found.  O(1).
    const PassMetadata* find(uint32_t id) const noexcept;

    /// Look up a pass by name.  Returns nullptr if not found.  O(1).
    const PassMetadata* find(const std::string& name) const noexcept;

    /// All registered passes in stable ID order.
    const std::vector<PassMetadata>& all() const noexcept { return passes_; }

    /// Compute a valid run order for @p subset of pass IDs, respecting
    /// requires→provides dependencies.  Throws std::logic_error on cycles.
    /// If @p subset is empty, returns an ordering for ALL passes.
    std::vector<uint32_t> topologicalOrder(
        const std::vector<uint32_t>& subset = {}) const;

private:
    PassRegistry()  = default;
    ~PassRegistry() = default;

    PassRegistry(const PassRegistry&)            = delete;
    PassRegistry& operator=(const PassRegistry&) = delete;

    std::vector<PassMetadata> passes_;
    /// O(1) index: stable pass ID → index into passes_.
    std::unordered_map<uint32_t, size_t> byId_;
    /// O(1) index: pass name → index into passes_.
    std::unordered_map<std::string, size_t> byName_;
    uint32_t nextId_ = 1;
};

// ─────────────────────────────────────────────────────────────────────────────
// Well-known pass IDs (assigned during static init in opt_orchestrator.cpp)
// ─────────────────────────────────────────────────────────────────────────────
///
/// These IDs are used by the Orchestrator and tests to reference passes
/// without hard-coding their numeric values.  They are populated once by
/// the registration macros at program startup; after that they are const.
namespace PassId {
    extern uint32_t kPreflightCheck;
    extern uint32_t kStringTypes;
    extern uint32_t kArrayTypes;
    extern uint32_t kConstantReturns;
    extern uint32_t kPurity;
    extern uint32_t kEffects;
    extern uint32_t kERSL;
    extern uint32_t kSynthesis;
    extern uint32_t kCFCTRE;
    extern uint32_t kEGraph;
    extern uint32_t kRangeAnalysis;
    extern uint32_t kRLC;
    extern uint32_t kWidthLegalization;
    extern uint32_t kWidthOpt;
    extern uint32_t kUniqueness;
    extern uint32_t kBorrowCheck;
} // namespace PassId

// ─────────────────────────────────────────────────────────────────────────────
// AnalysisKey — type alias for analysis fact identifier strings
// ─────────────────────────────────────────────────────────────────────────────
using AnalysisKey = std::string;

// ─────────────────────────────────────────────────────────────────────────────
// AnalysisDependencyGraph — tracks which analyses depend on which
// ─────────────────────────────────────────────────────────────────────────────
///
/// The dependency graph records "A depends on B" edges, meaning that if fact B
/// is invalidated, fact A must also be invalidated (because A was computed using
/// B as an input).  This enables automatic cascading invalidation: callers only
/// need to invalidate the direct target; the graph propagates to all transitive
/// dependents.
///
/// ## OmScript standard dependencies (see createDefault()):
///   constant_returns → (none)
///   purity           → constant_returns
///   effects          → purity
///   synthesis        → purity, effects          (and itself invalidates them)
///   cfctre           → purity, effects, synthesis
///   egraph           → cfctre
///   range_analysis   → purity, effects, cfctre
///
/// ## Invalidation semantics
/// When fact B is invalidated:
///   1. All direct dependents of B (facts A where A→B edge exists) are
///      transitively invalidated.
///   2. Invalidation stops at facts with no registered dependents.
///
/// ## Thread safety
/// Mutation (addDependency) is NOT thread-safe.  The graph is expected to be
/// built once (in a single-threaded static-init context) and thereafter used
/// read-only from multiple threads.
class AnalysisDependencyGraph {
public:
    AnalysisDependencyGraph() = default;

    /// Register that @p dependent requires @p dependency to be valid.
    /// When @p dependency is invalidated, @p dependent will also be
    /// invalidated via cascading.
    void addDependency(const std::string& dependent,
                       const std::string& dependency) {
        // dependents_[dependency] = {all facts that directly depend on it}
        dependents_[dependency].insert(dependent);
    }

    /// Return every fact that must be invalidated when @p key is invalidated.
    /// Includes @p key itself, plus all transitive dependents (BFS).
    std::vector<std::string> getAllDependents(const std::string& key) const {
        std::vector<std::string> result;
        std::unordered_set<std::string> visited;
        std::vector<std::string> queue;
        queue.push_back(key);
        while (!queue.empty()) {
            std::string cur = std::move(queue.back());
            queue.pop_back();
            if (!visited.insert(cur).second) continue;
            result.push_back(cur);
            auto it = dependents_.find(cur);
            if (it == dependents_.end()) continue;
            for (const auto& dep : it->second) {
                if (!visited.count(dep)) queue.push_back(dep);
            }
        }
        return result;
    }

    /// Return the direct dependents of @p key (not transitive).
    const std::unordered_set<std::string>* directDependents(
        const std::string& key) const noexcept {
        auto it = dependents_.find(key);
        return (it != dependents_.end()) ? &it->second : nullptr;
    }

    /// True if any dependency edge involves @p key (as a dependency or as a
    /// dependent).
    bool contains(const std::string& key) const noexcept {
        if (dependents_.count(key)) return true;
        for (const auto& [k, deps] : dependents_) {
            if (deps.count(key)) return true;
        }
        return false;
    }

    /// Create the standard OmScript analysis dependency graph.
    /// Must be called after static initialisation of AnalysisFact constants.
    static AnalysisDependencyGraph createDefault();

private:
    /// Key   = analysis fact that was invalidated
    /// Value = set of analysis facts that directly depend on it (and must
    ///         therefore also be invalidated when the key is invalidated)
    std::unordered_map<std::string, std::unordered_set<std::string>> dependents_;
};

} // namespace omscript

#endif // OPT_PASS_H
