#pragma once
#ifndef OPT_CONTEXT_H
#define OPT_CONTEXT_H

/// @file opt_context.h
/// @brief Unified Optimization Context for the OmScript compiler.
///
/// This header provides three foundational abstractions:
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
///  3. OptimizationContext — owns the FunctionFacts store, exposes the
///     analysis-validity flags that let the orchestrator decide which
///     passes need to be (re-)run, and provides a single query surface for
///     codegen to ask questions like "is this function pure?" without knowing
///     which analysis pass produced the answer.

#include "ast.h"     // FunctionEffects, OptMaxConfig, etc.
#include "cfctre.h"  // CTValue, CTEngine
#include <optional>
#include <string>
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

private:
    BuiltinEffectTable() = delete;
    static const std::unordered_map<std::string, BuiltinEffects>& table() noexcept;
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
};

// ─────────────────────────────────────────────────────────────────────────────
// AnalysisValidity — tracks which analyses are up-to-date
// ─────────────────────────────────────────────────────────────────────────────
///
/// Each bit corresponds to one analysis pass.  When a transform invalidates
/// an analysis (e.g., AST mutation from the e-graph invalidates purity facts),
/// the orchestrator clears the corresponding flag and re-runs the pass before
/// any consumer reads from it again.
struct AnalysisValidity {
    bool stringTypes    = false; ///< preAnalyzeStringTypes() has run
    bool arrayTypes     = false; ///< preAnalyzeArrayTypes() has run
    bool constantReturns = false; ///< analyzeConstantReturnValues() has run
    bool purity         = false; ///< autoDetectConstEvalFunctions() has run
    bool effects        = false; ///< inferFunctionEffects() has run
    bool synthesis      = false; ///< runSynthesisPass() has run
    bool cfctre         = false; ///< runCFCTRE() has run
    bool egraph         = false; ///< egraph::optimizeProgram() has run

    /// Mark all facts invalid (call when the AST is modified).
    void invalidateAll() noexcept { *this = {}; }

    /// Mark only the facts that depend on function bodies (called when a
    /// single function's body changes, e.g., after synthesis or loop fusion).
    void invalidateFunctionFacts() noexcept {
        constantReturns = false;
        purity          = false;
        effects         = false;
        cfctre          = false;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// OptimizationContext — the unified analysis hub
// ─────────────────────────────────────────────────────────────────────────────
///
/// Owns the per-function facts table, analysis-validity flags, and a
/// non-owning pointer to the CTEngine (owned by CodeGenerator since the
/// engine contains the memoisation cache that outlives the context).
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

private:
    std::unordered_map<std::string, FunctionFacts> facts_;
    AnalysisValidity validity_;
    CTEngine*        ctEngine_ = nullptr;
};

} // namespace omscript

#endif // OPT_CONTEXT_H
