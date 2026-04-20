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
/// • Extensible: new passes are registered with a single
///   OMSC_REGISTER_PASS() call; no global arrays to update manually.
/// • Zero-overhead when disabled: all metadata lives in read-only data
///   sections; no virtual dispatch at runtime.

#include <cstdint>
#include <string>
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
    IRPipeline,           ///< LLVM pass-manager pipeline (O1/O2/O3 + OPTMAX)
    BackendTuning,        ///< Superoptimizer, HGOE, post-pipeline cleanup
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
    inline constexpr const char* kStringTypes     = "string_types";
    inline constexpr const char* kArrayTypes      = "array_types";
    inline constexpr const char* kConstantReturns = "constant_returns";
    inline constexpr const char* kPurity          = "purity";
    inline constexpr const char* kEffects         = "effects";
    inline constexpr const char* kSynthesis       = "synthesis";
    inline constexpr const char* kCFCTRE          = "cfctre";
    inline constexpr const char* kEGraph          = "egraph";
} // namespace AnalysisFact

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

    /// Look up a pass by ID.  Returns nullptr if not found.
    const PassMetadata* find(uint32_t id) const noexcept;

    /// Look up a pass by name.  Returns nullptr if not found.
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
    uint32_t nextId_ = 1;
};

// ─────────────────────────────────────────────────────────────────────────────
// IPass — common interface for AST-level passes
// ─────────────────────────────────────────────────────────────────────────────
///
/// Passes that operate on the AST implement this interface so the Orchestrator
/// can run them polymorphically without knowing their concrete type.
/// IR-level passes (which live inside LLVM's PassManager) do not implement
/// this interface; they are described only by their metadata.
class Program; // forward declaration (defined in ast.h)
class OptimizationContext; // forward declaration

class IPass {
public:
    virtual ~IPass() = default;

    /// Return the static metadata for this pass.
    virtual const PassMetadata& metadata() const noexcept = 0;

    /// Execute the pass.
    /// @param program  The AST being compiled (may be modified for transforms).
    /// @param ctx      The optimization context (read facts before, write after).
    virtual void run(Program* program, OptimizationContext& ctx) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Well-known pass IDs (assigned during static init in opt_orchestrator.cpp)
// ─────────────────────────────────────────────────────────────────────────────
///
/// These IDs are used by the Orchestrator and tests to reference passes
/// without hard-coding their numeric values.  They are populated once by
/// the registration macros at program startup; after that they are const.
namespace PassId {
    extern uint32_t kStringTypes;
    extern uint32_t kArrayTypes;
    extern uint32_t kConstantReturns;
    extern uint32_t kPurity;
    extern uint32_t kEffects;
    extern uint32_t kSynthesis;
    extern uint32_t kCFCTRE;
    extern uint32_t kEGraph;
} // namespace PassId

} // namespace omscript

#endif // OPT_PASS_H
