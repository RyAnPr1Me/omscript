#pragma once
#ifndef OPT_CONTEXT_H
#define OPT_CONTEXT_H

/// @file opt_context.h
/// @brief Unified Optimization Context for the OmScript compiler.
///
/// This header provides four foundational abstractions:
///
///  1. BuiltinEffectTable — the SINGLE canonical source of truth for the
///     purity and side-effect classification of every built-in function.
///     Replaces the previously scattered local tables:
///       • kPureBuiltins / kImpureBuiltins  in autoDetectConstEvalFunctions()
///       • kIOBuiltins / kMutatingBuiltins / kReadBuiltins  in inferFunctionEffects()
///     All queries go through this one table so a single edit propagates
///     everywhere.
///
///  2. FunctionFacts — per-function analysis results accumulated across
///     all pre-passes, stored in one place instead of spread across
///     separate maps inside CodeGenerator.
///
///  3. EGraphSubsystem — the e-graph equality-saturation optimizer as a
///     first-class subsystem of the optimization pipeline.  Owned by
///     OptimizationContext; configured before the egraph pre-pass runs;
///     exposes per-run statistics after the pass completes.  Calling code
///     (Orchestrator, CodeGenerator) accesses the e-graph exclusively
///     through this subsystem rather than the egraph:: free functions.
///
///  4. OptimizationContext — owns the FunctionFacts store, the EGraphSubsystem,
///     analysis-validity flags, and a non-owning pointer to the CTEngine.
///     Provides a single query surface for codegen to ask questions like
///     "is this function pure?" without knowing which analysis pass answered.

#include "ast.h"      // FunctionEffects, OptMaxConfig, etc.
#include "cfctre.h"   // CTValue, CTEngine
#include "egraph.h"   // EGraph, SaturationConfig, egraph::optimizeProgram, etc.
#include "opt_pass.h" // AnalysisKey, PassContract, IRInvariant
#include <any>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// BuiltinEffects — per-builtin effect descriptor
// ─────────────────────────────────────────────────────────────────────────────
///
/// Every built-in function in OmScript is described by this struct.
/// The four fields map directly onto the query predicates callers need:
///
///   constFoldable   → safe to evaluate at compile time when all arguments
///                     are known constants (replaces kPureBuiltins).
///   readsMemory     → may load from heap arrays, strings, or maps; safe for
///                     LLVM's readonly attribute (replaces kReadBuiltins).
///   writesMemory    → mutates heap memory: push/pop/sort/map_set/…
///                     (replaces kMutatingBuiltins).
///   hasIO           → performs observable I/O: print/input/file_*/sleep/…
///                     (replaces kIOBuiltins).
///
/// Derived predicates (implemented as free functions below):
///   isPureBuiltin(n)     ≡ table[n].constFoldable
///   isImpureBuiltin(n)   ≡ table[n].writesMemory || table[n].hasIO
///   isIOBuiltin(n)       ≡ table[n].hasIO
///   isMutatingBuiltin(n) ≡ table[n].writesMemory && !table[n].hasIO
///   isReadOnlyBuiltin(n) ≡ table[n].readsMemory && !table[n].writesMemory
///                             && !table[n].hasIO
struct BuiltinEffects {
    bool constFoldable = false; ///< Can be evaluated at compile time
    bool readsMemory   = false; ///< Accesses heap/array/string memory
    bool writesMemory  = false; ///< Mutates heap/array/string memory
    bool hasIO         = false; ///< Performs observable I/O
    bool mayThrow      = false; ///< May raise / panic / abort / exit
    bool noReturn      = false; ///< Provably never returns to the caller
    bool allocates     = false; ///< Performs heap allocation
    bool deallocates   = false; ///< Releases heap memory
};

// ─────────────────────────────────────────────────────────────────────────────
// BuiltinEffectTable — static singleton lookup table
// ─────────────────────────────────────────────────────────────────────────────
///
/// Provides O(1) lookup of the effect descriptor for any built-in name.
/// Returns a default-constructed BuiltinEffects{} (all false) for unknown
/// names so callers can treat unknowns conservatively without extra branches.
class BuiltinEffectTable {
public:
    /// Return the effect descriptor for built-in @p name.
    /// For unknown names all fields are false (conservatively impure).
    static const BuiltinEffects& get(const std::string& name) noexcept;

    // ── Convenience predicates ────────────────────────────────────────────

    /// True when calling @p name cannot prevent the containing function from
    /// being const-evaluated (replaces kPureBuiltins look-up).
    static bool isPure(const std::string& name) noexcept {
        return get(name).constFoldable;
    }

    /// True when calling @p name makes the containing function impure for
    /// const-eval purposes (replaces kImpureBuiltins look-up).
    static bool isImpure(const std::string& name) noexcept {
        const auto& e = get(name);
        return e.writesMemory || e.hasIO;
    }

    /// True when @p name performs I/O (replaces kIOBuiltins look-up).
    static bool isIO(const std::string& name) noexcept {
        return get(name).hasIO;
    }

    /// True when @p name mutates heap memory (replaces kMutatingBuiltins).
    static bool isMutating(const std::string& name) noexcept {
        const auto& e = get(name);
        return e.writesMemory && !e.hasIO;
    }

    /// True when @p name only reads memory (replaces kReadBuiltins).
    static bool isReadOnly(const std::string& name) noexcept {
        const auto& e = get(name);
        return e.readsMemory && !e.writesMemory && !e.hasIO;
    }

    /// True when @p name may throw, panic, abort, or otherwise unwind/terminate.
    static bool mayThrow(const std::string& name) noexcept {
        return get(name).mayThrow;
    }

    /// True when @p name provably never returns (panic/abort/exit).
    static bool noReturn(const std::string& name) noexcept {
        return get(name).noReturn;
    }

    /// True when @p name allocates heap memory.
    static bool allocates(const std::string& name) noexcept {
        return get(name).allocates;
    }

    /// True when @p name deallocates heap memory.
    static bool deallocates(const std::string& name) noexcept {
        return get(name).deallocates;
    }

private:
    BuiltinEffectTable() = delete;
    static const std::unordered_map<std::string, BuiltinEffects>& table() noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// ValueRange — inclusive interval for an integer expression
// ─────────────────────────────────────────────────────────────────────────────
///
/// Tracks a conservative closed interval [lo, hi] for an integer value.
/// Used to record known bounds on function return values and to guide
/// the backend in emitting tighter LLVM !range metadata.
struct ValueRange {
    int64_t lo = std::numeric_limits<int64_t>::min(); ///< Inclusive lower bound
    int64_t hi = std::numeric_limits<int64_t>::max(); ///< Inclusive upper bound

    /// True when the range has been narrowed below the full i64 range.
    bool isNarrowed() const noexcept {
        return lo > std::numeric_limits<int64_t>::min() ||
               hi < std::numeric_limits<int64_t>::max();
    }

    /// Intersect two ranges: the tightest bound satisfying both constraints.
    static ValueRange intersect(const ValueRange& a, const ValueRange& b) noexcept {
        return {std::max(a.lo, b.lo), std::min(a.hi, b.hi)};
    }

    /// Join two ranges: the widest bound covering both.
    static ValueRange join(const ValueRange& a, const ValueRange& b) noexcept {
        return {std::min(a.lo, b.lo), std::max(a.hi, b.hi)};
    }

    /// True when the range is empty (no value satisfies it).
    bool isEmpty() const noexcept { return lo > hi; }

    /// True when the value is always non-negative.
    bool isNonNeg() const noexcept { return lo >= 0; }

    /// True when the value is a single known constant.
    bool isConst() const noexcept { return lo == hi; }

    /// Return the constant value when isConst() is true.
    int64_t constVal() const noexcept { return lo; }
};

// ─────────────────────────────────────────────────────────────────────────────
// FunctionFacts — per-function analysis results
// ─────────────────────────────────────────────────────────────────────────────
///
/// All analysis results produced by the pre-pass sequence are stored here
/// rather than across half a dozen independent maps in CodeGenerator.
/// The Orchestrator populates this struct; CodeGenerator reads from it.
struct FunctionFacts {
    // ── Purity / effect ───────────────────────────────────────────────────
    bool isPure          = false; ///< No side effects; safe for LLVM readnone
    bool isConstFoldable = false; ///< Can be const-evaluated when args known
    FunctionEffects effects;      ///< Detailed effect summary from inferFunctionEffects

    // ── Constant return values ────────────────────────────────────────────
    std::optional<int64_t>     constIntReturn;    ///< Always-constant int return
    std::optional<std::string> constStringReturn; ///< Always-constant string return
    std::optional<CTValue>     uniformCTReturn;   ///< CF-CTRE uniform return value

    // ── Reachability ──────────────────────────────────────────────────────
    bool isDead = false; ///< Unreachable from any entry point (CF-CTRE Phase 7)

    // ── CF-CTRE: was this function successfully CT-evaluated? ─────────────
    bool foldedByCFCTRE = false; ///< Had ≥1 successful compile-time call

    // ── Value range for the return value ─────────────────────────────────
    std::optional<ValueRange> returnRange; ///< Known bounds on the return value
};

// ─────────────────────────────────────────────────────────────────────────────
// AnalysisValidity — tracks which analyses are up-to-date
// ─────────────────────────────────────────────────────────────────────────────
///
/// Each bit corresponds to one analysis pass.  When a transform invalidates
/// an analysis (e.g., AST mutation from the e-graph invalidates purity facts),
/// the orchestrator clears the corresponding flag and re-runs the pass before
/// any consumer reads from it again.
///
/// **Adding a new analysis fact:**
///   1. Add a `bool` field below (follow the existing naming pattern).
///   2. Add one entry to `fieldTable()` in the cpp — that is the *only* place
///      the string→field mapping needs to appear.  All three dispatch methods
///      (`isValid`, `markValid`, `invalidate`) and `invalidateFunctionFacts`
///      derive their behaviour from the same table.
struct AnalysisValidity {
    bool stringTypes     = false; ///< preAnalyzeStringTypes() has run
    bool arrayTypes      = false; ///< preAnalyzeArrayTypes() has run
    bool constantReturns = false; ///< analyzeConstantReturnValues() has run
    bool purity          = false; ///< autoDetectConstEvalFunctions() has run
    bool effects         = false; ///< inferFunctionEffects() has run
    bool synthesis       = false; ///< runSynthesisPass() has run
    bool cfctre          = false; ///< runCFCTRE() has run
    bool egraph          = false; ///< egraph::optimizeProgram() has run
    bool rangeAnalysis   = false; ///< Value range analysis has run
    bool rlc             = false; ///< Region Lifetime Coalescing pass has run

    // ── Dispatch table ────────────────────────────────────────────────────
    //
    // Returns a pointer to the `bool` field for @p fact, or nullptr if the
    // fact name is not recognised.  Uses C++ pointer-to-member (PMF) so that
    // `isValid`, `markValid`, and `invalidate` all derive from one table
    // rather than three parallel if-chains.
    //
    // Adding a new analysis fact: add one Row here.
    bool* fieldFor(std::string_view fact) noexcept {
        struct Row { std::string_view name; bool AnalysisValidity::* field; };
        static constexpr Row kTable[] = {
            {"string_types",     &AnalysisValidity::stringTypes    },
            {"array_types",      &AnalysisValidity::arrayTypes     },
            {"constant_returns", &AnalysisValidity::constantReturns},
            {"purity",           &AnalysisValidity::purity         },
            {"effects",          &AnalysisValidity::effects        },
            {"synthesis",        &AnalysisValidity::synthesis      },
            {"cfctre",           &AnalysisValidity::cfctre         },
            {"egraph",           &AnalysisValidity::egraph         },
            {"range_analysis",   &AnalysisValidity::rangeAnalysis  },
            {"rlc",              &AnalysisValidity::rlc            },
        };
        for (const auto& row : kTable) {
            if (row.name == fact)
                return &(this->*(row.field));
        }
        return nullptr;
    }
    const bool* fieldFor(std::string_view fact) const noexcept {
        return const_cast<AnalysisValidity*>(this)->fieldFor(fact);
    }

    // ── Public interface ──────────────────────────────────────────────────

    /// Mark all facts invalid (call when the AST is modified).
    void invalidateAll() noexcept { *this = {}; }

    /// Mark only the facts that depend on function bodies (called when a
    /// single function's body changes, e.g., after synthesis or loop fusion).
    void invalidateFunctionFacts() noexcept {
        // These five facts are derived from the function body and must be
        // recomputed when any function is modified.  The list deliberately
        // excludes string_types, array_types, and rlc which are structural
        // facts that survive single-function edits.
        static constexpr std::string_view kBodyFacts[] = {
            "constant_returns", "purity", "effects", "cfctre", "range_analysis"
        };
        for (std::string_view f : kBodyFacts)
            if (bool* fp = fieldFor(f)) *fp = false;
    }

    /// Return true if the analysis fact identified by @p fact is currently valid.
    /// @p fact should be one of the AnalysisFact::k* string literals defined in
    /// opt_pass.h (e.g. "purity", "effects").  Unknown fact names return false.
    bool isValid(std::string_view fact) const noexcept {
        const bool* fp = fieldFor(fact);
        return fp && *fp;
    }

    /// Mark the analysis fact identified by @p fact as invalid (stale).
    /// Called by PassScheduler::applyInvalidation() after a transformation pass.
    /// If a dependency graph is attached, all transitive dependents of @p fact
    /// are also marked invalid (cascading invalidation).
    /// Unknown fact names are silently ignored.
    void invalidate(std::string_view fact) noexcept {
        auto markInvalid = [this](std::string_view f) noexcept {
            if (bool* fp = fieldFor(f)) *fp = false;
        };
        if (depGraph_) {
            for (const auto& d : depGraph_->getAllDependents(std::string(fact)))
                markInvalid(d);
        } else {
            markInvalid(fact);
        }
    }

    /// Mark the analysis fact identified by @p fact as valid (freshly produced).
    /// Used by PassScheduler::applyInvalidation() to re-validate the facts a
    /// pass just produced after the dependency-graph cascade — the cascade may
    /// have transitively invalidated a fact that the pass itself provides
    /// (e.g. synthesis depends on purity and also invalidates purity).
    /// Unknown fact names are silently ignored.
    void markValid(std::string_view fact) noexcept {
        if (bool* fp = fieldFor(fact)) *fp = true;
    }

    /// Attach a dependency graph so that invalidating a fact also invalidates
    /// all transitively dependent facts.  Non-owning pointer.
    void setDependencyGraph(const AnalysisDependencyGraph* graph) noexcept {
        depGraph_ = graph;
    }
    const AnalysisDependencyGraph* dependencyGraph() const noexcept {
        return depGraph_;
    }

private:
    const AnalysisDependencyGraph* depGraph_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// AnalysisCache — typed analysis result store with invalidation
// ─────────────────────────────────────────────────────────────────────────────
///
/// A typed, key-value cache for analysis results.  Keys are AnalysisKey strings
/// (the same tokens used in AnalysisFact and AnalysisValidity).  Values are
/// stored as std::any so heterogeneous analysis results (dependence summaries,
/// alias sets, SCEV facts, etc.) can be cached without a common base class.
///
/// **Usage**
/// ```cpp
/// cache.put<DependenceSummary>("dep_summary", std::move(ds));
/// const auto* ds = cache.get<DependenceSummary>("dep_summary");
/// cache.invalidate("dep_summary");
/// cache.invalidateByContract(passContract);
/// ```
///
/// **Thread safety**: NOT thread-safe.  Owned by OptimizationContext and
/// accessed single-threaded during the pre-pass pipeline.
class AnalysisCache {
public:
    AnalysisCache()  = default;
    ~AnalysisCache() = default;

    // Non-copyable (large state).
    AnalysisCache(const AnalysisCache&)            = delete;
    AnalysisCache& operator=(const AnalysisCache&) = delete;

    // Movable.
    AnalysisCache(AnalysisCache&&)            = default;
    AnalysisCache& operator=(AnalysisCache&&) = default;

    /// Store a value of type T under key @p k.
    /// Replaces any existing value for that key.
    template<class T>
    void put(const AnalysisKey& k, T value) {
        store_[k] = std::move(value);
    }

    /// Retrieve a const pointer to the stored value of type T for key @p k.
    /// Returns nullptr if the key is not found or the stored type is not T.
    template<class T>
    const T* get(const AnalysisKey& k) const noexcept {
        auto it = store_.find(k);
        if (it == store_.end()) return nullptr;
        return std::any_cast<T>(&it->second);
    }

    /// Retrieve a mutable pointer to the stored value of type T for key @p k.
    /// Returns nullptr if the key is not found or the stored type is not T.
    template<class T>
    T* get(const AnalysisKey& k) noexcept {
        auto it = store_.find(k);
        if (it == store_.end()) return nullptr;
        return std::any_cast<T>(&it->second);
    }

    /// Return true if the cache contains an entry for key @p k.
    bool has(const AnalysisKey& k) const noexcept {
        return store_.count(k) > 0;
    }

    /// Remove the cached value for key @p k (if present).
    /// If a dependency graph has been attached (via setDependencyGraph()),
    /// all transitive dependents of @p k are also evicted.
    void invalidate(const AnalysisKey& k) noexcept {
        if (depGraph_) {
            for (const auto& dep : depGraph_->getAllDependents(k))
                store_.erase(dep);
        } else {
            store_.erase(k);
        }
    }

    /// Remove all cached values whose keys appear in @p contract.invalidates_facts.
    /// Cascades through the dependency graph (if attached) for each key.
    void invalidateByContract(const PassContract& contract) noexcept {
        for (const auto& key : contract.invalidates_facts)
            invalidate(key); // uses graph-aware invalidate
    }

    /// Attach a dependency graph so that invalidating a fact also invalidates
    /// all facts that were computed using it.  Non-owning pointer: the graph
    /// must outlive the cache.  Passing nullptr detaches any existing graph.
    void setDependencyGraph(const AnalysisDependencyGraph* graph) noexcept {
        depGraph_ = graph;
    }

    const AnalysisDependencyGraph* dependencyGraph() const noexcept {
        return depGraph_;
    }

    /// Remove all cached values.
    void clear() noexcept { store_.clear(); }

    /// Return the number of entries currently in the cache.
    size_t size() const noexcept { return store_.size(); }

private:
    std::unordered_map<AnalysisKey, std::any> store_;
    const AnalysisDependencyGraph*            depGraph_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// EGraphConfig — configuration for one EGraphSubsystem run
// ─────────────────────────────────────────────────────────────────────────────
///
/// Mirrors the fields of `egraph::SaturationConfig` but lives in the public
/// optimization-system surface so callers (CodeGenerator, tests) can adjust
/// them without knowing the internal `SaturationConfig` type.
///
/// Default values match the existing behaviour of `egraph::optimizeExpression`.
struct EGraphConfig {
    size_t maxNodes            = 10000; ///< Node limit per expression graph
    size_t maxIterations       = 15;    ///< Saturation iteration cap
    bool   enableConstFolding  = true;  ///< Enable constant-folding during saturation
};

// ─────────────────────────────────────────────────────────────────────────────
// EGraphStats — per-run statistics produced by EGraphSubsystem
// ─────────────────────────────────────────────────────────────────────────────
///
/// Reset at the start of each call to optimizeProgram/optimizeFunction.
/// Available for query (e.g., verbose logging) after the pass completes.
struct EGraphStats {
    unsigned expressionsAttempted  = 0; ///< Expressions submitted to the e-graph
    unsigned expressionsSimplified = 0; ///< Expressions that changed after extraction
    unsigned expressionsSkipped    = 0; ///< Expressions not representable in the e-graph
    unsigned functionsChanged      = 0; ///< Functions with ≥1 simplified expression
    size_t   rulesApplied          = 0; ///< Total rewrites across all saturations

    void reset() noexcept { *this = {}; }
};

// ─────────────────────────────────────────────────────────────────────────────
// EGraphSubsystem — e-graph equality-saturation as an optimization subsystem
// ─────────────────────────────────────────────────────────────────────────────
///
/// This is the single entry point for all e-graph equality-saturation
/// optimization.  It is owned by `OptimizationContext` and accessed via
/// `ctx.egraph()`.
///
/// **Why a subsystem rather than free functions?**
///
///   Free functions (`egraph::optimizeProgram`) cannot carry per-run state,
///   configuration, or statistics without global variables.  Making the e-graph
///   a subsystem allows:
///     • Configuration injection per compilation (e.g., tighter node limits at
///       lower optimization levels).
///     • Per-run statistics aggregation for verbose output and future profiling.
///     • A stable interface point for future changes (e.g., per-function rule
///       sets, incremental saturation, external rule plugins).
///
/// **Usage**
/// ```cpp
/// ctx.egraph().setConfig({.maxNodes = 20000, .maxIterations = 20});
/// ctx.egraph().optimizeProgram(program);
/// if (verbose) print(ctx.egraph().stats());
/// ```
class EGraphSubsystem {
public:
    EGraphSubsystem()  = default;
    ~EGraphSubsystem() = default;

    // ── Configuration ─────────────────────────────────────────────────────

    /// Replace the current configuration.  Must be called before any
    /// optimize* call; ignored after the pass has started running.
    void setConfig(EGraphConfig cfg) noexcept { config_ = cfg; }
    const EGraphConfig& config() const noexcept { return config_; }

    /// Build the egraph::SaturationConfig from our config for internal use.
    egraph::SaturationConfig toSaturationConfig() const noexcept {
        egraph::SaturationConfig sc;
        sc.maxNodes            = config_.maxNodes;
        sc.maxIterations       = config_.maxIterations;
        sc.enableConstantFolding = config_.enableConstFolding;
        return sc;
    }

    /// Build the egraph::EGraphOptContext from our config and pure-user-funcs.
    /// This is what all internal optimize* calls use.
    egraph::EGraphOptContext toOptContext() const noexcept {
        egraph::EGraphOptContext ctx;
        ctx.config        = toSaturationConfig();
        ctx.pureUserFuncs = pureUserFuncs_.empty() ? nullptr : &pureUserFuncs_;
        return ctx;
    }

    // ── Pure-user-function registry ───────────────────────────────────────

    /// Replace the set of known-pure user-defined function names.
    /// Called by the Orchestrator after the purity pass so that the e-graph
    /// can include expressions involving these functions.
    void setPureUserFuncs(std::unordered_set<std::string> funcs) {
        pureUserFuncs_ = std::move(funcs);
    }

    const std::unordered_set<std::string>& pureUserFuncs() const noexcept {
        return pureUserFuncs_;
    }

    // ── Entry points ──────────────────────────────────────────────────────

    /// Optimize a single AST expression.  Returns the simplified expression
    /// (or the original if no improvement was found / not representable).
    std::unique_ptr<Expression> optimizeExpression(const Expression* expr);

    /// Optimize all expressions in one function's body.
    void optimizeFunction(FunctionDecl* func);

    /// Optimize all functions in a program.  Resets stats before running.
    void optimizeProgram(Program* program);

    // ── Statistics ────────────────────────────────────────────────────────

    const EGraphStats& stats() const noexcept { return stats_; }

    /// Reset statistics (called automatically at the start of optimizeProgram).
    void resetStats() noexcept { stats_.reset(); }

private:
    EGraphConfig                    config_;
    EGraphStats                     stats_;
    std::unordered_set<std::string> pureUserFuncs_;
};

// ─────────────────────────────────────────────────────────────────────────────
// OptimizationContext — the unified analysis hub
// ─────────────────────────────────────────────────────────────────────────────
///
/// Owns the per-function facts table, analysis-validity flags, the
/// EGraphSubsystem, and a non-owning pointer to the CTEngine (owned by
/// CodeGenerator since the engine contains the memoisation cache that
/// outlives the context).
///
/// **Lifetime**: created at the top of CodeGenerator::generate() and lives
/// for the duration of that call.  CodeGenerator passes a pointer to it into
/// the Orchestrator; the Orchestrator populates it during the pre-pass
/// sequence; and CodeGenerator queries it during IR emission.
class OptimizationContext {
public:
    explicit OptimizationContext() = default;
    ~OptimizationContext()         = default;

    // Non-copyable (large state, always passed by pointer or reference).
    OptimizationContext(const OptimizationContext&)            = delete;
    OptimizationContext& operator=(const OptimizationContext&) = delete;

    // ── Fact registration (called by analysis passes) ─────────────────────

    /// Register or update all facts for @p funcName.
    void setFacts(const std::string& funcName, FunctionFacts facts) {
        facts_[funcName] = std::move(facts);
    }

    /// Return the facts for @p funcName.  If the function has not been
    /// analysed, a default-constructed FunctionFacts is returned.
    const FunctionFacts& getFacts(const std::string& funcName) const noexcept {
        static const FunctionFacts kDefault;
        auto it = facts_.find(funcName);
        return (it != facts_.end()) ? it->second : kDefault;
    }

    /// Mutable access — used by the Orchestrator to incrementally populate.
    FunctionFacts& mutableFacts(const std::string& funcName) {
        return facts_[funcName];
    }

    // ── High-level query API (used by CodeGenerator during IR emission) ───

    /// True when @p funcName is pure (no side effects of any kind).
    bool isPure(const std::string& funcName) const noexcept {
        auto it = facts_.find(funcName);
        return (it != facts_.end()) && it->second.isPure;
    }

    /// True when @p funcName can be const-evaluated with constant args.
    bool isConstFoldable(const std::string& funcName) const noexcept {
        auto it = facts_.find(funcName);
        return (it != facts_.end()) && it->second.isConstFoldable;
    }

    /// Return the always-constant integer return value, if known.
    std::optional<int64_t> constIntReturn(const std::string& funcName) const noexcept {
        auto it = facts_.find(funcName);
        if (it == facts_.end()) return std::nullopt;
        return it->second.constIntReturn;
    }

    /// Return the always-constant string return value, if known.
    std::optional<std::string> constStringReturn(const std::string& funcName) const noexcept {
        auto it = facts_.find(funcName);
        if (it == facts_.end()) return std::nullopt;
        return it->second.constStringReturn;
    }

    /// Return the CF-CTRE uniform return value (may be any CTValue), if known.
    std::optional<CTValue> uniformCTReturn(const std::string& funcName) const noexcept {
        auto it = facts_.find(funcName);
        if (it == facts_.end()) return std::nullopt;
        return it->second.uniformCTReturn;
    }

    /// Return the known integer return range for @p funcName, if available.
    std::optional<ValueRange> returnRange(const std::string& funcName) const noexcept {
        auto it = facts_.find(funcName);
        if (it == facts_.end()) return std::nullopt;
        return it->second.returnRange;
    }

    /// True when @p funcName is unreachable from any entry point.
    bool isDead(const std::string& funcName) const noexcept {
        auto it = facts_.find(funcName);
        return (it != facts_.end()) && it->second.isDead;
    }

    /// Return the effect summary for @p funcName.
    const FunctionEffects& effects(const std::string& funcName) const noexcept {
        static const FunctionEffects kDefault;
        auto it = facts_.find(funcName);
        return (it != facts_.end()) ? it->second.effects : kDefault;
    }

    // ── Validity tracking ─────────────────────────────────────────────────

    AnalysisValidity&       validity()       noexcept { return validity_; }
    const AnalysisValidity& validity() const noexcept { return validity_; }

    // ── CTEngine reference ────────────────────────────────────────────────
    /// Non-owning pointer to the CTEngine that owns the memoisation cache.
    /// Set once by CodeGenerator::generate() before the Orchestrator runs.
    void setCTEngine(CTEngine* engine) noexcept { ctEngine_ = engine; }
    CTEngine* ctEngine() const noexcept { return ctEngine_; }

    // ── Iteration helpers ─────────────────────────────────────────────────
    const std::unordered_map<std::string, FunctionFacts>& allFacts() const noexcept {
        return facts_;
    }

    // ── E-Graph subsystem ─────────────────────────────────────────────────
    ///
    /// The e-graph subsystem is the canonical entry point for equality-saturation
    /// optimization.  It is owned by the context, configured before the egraph
    /// pre-pass, and carries per-run statistics after the pass completes.
    ///
    /// Access via: ctx.egraph().*
    EGraphSubsystem&       egraph()       noexcept { return egraph_; }
    const EGraphSubsystem& egraph() const noexcept { return egraph_; }

    // ── Analysis cache ────────────────────────────────────────────────────
    ///
    /// The typed analysis cache stores heterogeneous analysis results keyed by
    /// AnalysisKey strings.  PassScheduler::applyInvalidation() evicts entries
    /// for any fact that a transformation pass declares as invalidated, ensuring
    /// cached results are never used after the underlying analysis becomes stale.
    AnalysisCache&       cache()       noexcept { return cache_; }
    const AnalysisCache& cache() const noexcept { return cache_; }

    // ── Analysis dependency graph ─────────────────────────────────────────
    ///
    /// When attached, the dependency graph is shared between AnalysisValidity
    /// and AnalysisCache so that invalidating a single fact cascades to all
    /// facts that were computed from it — without callers needing to enumerate
    /// the transitive closure manually.
    ///
    /// The graph is typically set once at construction time (from the default
    /// graph created by AnalysisDependencyGraph::createDefault()) and remains
    /// constant for the lifetime of the context.
    ///
    /// Non-owning: the caller is responsible for ensuring the graph outlives
    /// this context.  Pass nullptr to detach.
    void setDependencyGraph(const AnalysisDependencyGraph* graph) noexcept {
        validity_.setDependencyGraph(graph);
        cache_.setDependencyGraph(graph);
    }
    const AnalysisDependencyGraph* dependencyGraph() const noexcept {
        return cache_.dependencyGraph();
    }

private:
    std::unordered_map<std::string, FunctionFacts> facts_;
    AnalysisValidity validity_;
    CTEngine*        ctEngine_ = nullptr;
    EGraphSubsystem  egraph_;
    AnalysisCache    cache_;
};

} // namespace omscript

#endif // OPT_CONTEXT_H
