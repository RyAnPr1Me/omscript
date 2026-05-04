#pragma once

#ifndef HGOE_EGRAPH_H
#define HGOE_EGRAPH_H

/// @file hgoe_egraph.h
/// @brief HGOE-Guided E-Graph Superoptimizer.
///
/// This pass fuses two previously separate systems — the equality-saturation
/// e-graph optimizer and the enumerative superoptimizer — into a single
/// cost-directed search engine.
///
/// Design Goals
/// ============
/// • Soundness:          all rewrites preserve semantics.
/// • Best-so-far:        every EClass continuously tracks its cheapest known
///                       representative via bestNode / bestCost.
/// • Budgeted search:    node and iteration growth stops under configurable
///                       limits (nodeLimit, iterLimit).
/// • HGOE-first pruning: each candidate is judged by the HGOE scorer
///                       immediately; branches whose lower bound already
///                       exceeds the global best are discarded early.
/// • Incremental costs:  cost changes propagate bottom-up without a full
///                       re-extraction sweep (only ancestors in the use-list
///                       need updating).
/// • Target sensitivity: MultiCost is scalarised via a hardware-specific
///                       weight vector derived from the MicroarchProfile.
/// • Deterministic mode: given identical inputs and HGOE weights, results
///                       are stable (no pointer-keyed containers in the hot
///                       path; tie-breaking uses ClassId / ENode ordering).
///
/// Pipeline
/// ========
///   AST expr
///     → normalise (constant fold, canonicalise children ordering)
///     → seed root e-class
///     → guided expansion loop:
///         for each round:
///           1. Apply rewrite rules in priority order (HGOE-scored rhs wins ties)
///           2. Score every modified class via HGOE
///           3. Propagate bestCost upward through use-lists
///           4. Prune classes whose lowerBound > globalBest
///           5. Stop when budget exhausted or fixpoint reached
///     → extract best representative from root class
///     → convert back to AST

#include "egraph.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace omscript {
// Forward declarations to avoid pulling in heavy headers.
struct Program;
class  OptimizationContext;
namespace egraph { class EGraph; }

namespace hgoe_egraph {

// ─────────────────────────────────────────────────────────────────────────────
// HGOEScorer — hardware-specific cost scalarisation
// ─────────────────────────────────────────────────────────────────────────────

/// Weights for combining the MultiCost dimensions into a single scalar.
/// Values are derived from the target MicroarchProfile (latency-heavy targets
/// weight `latency` more; bandwidth-heavy targets weight `memoryPressure`).
struct CostWeights {
    double cycles             = 1.0;
    double uops               = 0.5;
    double latency            = 2.0;
    double throughputPressure = 0.5;
    double registerPressure   = 0.3;
    double memoryPressure     = 0.8;
    double branchPenalty      = 1.5;
};

/// Compute a single scalar cost from a MultiCost and a CostWeights.
inline double scalarize(const egraph::MultiCost& mc, const CostWeights& w) noexcept {
    return mc.cycles             * w.cycles
         + mc.uops               * w.uops
         + mc.latency            * w.latency
         + mc.throughputPressure * w.throughputPressure
         + mc.registerPressure   * w.registerPressure
         + mc.memoryPressure     * w.memoryPressure
         + mc.branchPenalty      * w.branchPenalty;
}

// ─────────────────────────────────────────────────────────────────────────────
// NodeCostTable — maps ENode::op to a baseline MultiCost
// ─────────────────────────────────────────────────────────────────────────────

/// Returns a baseline MultiCost for a leaf e-node (no children).
/// Values are conservative estimates for a generic x86-64 out-of-order CPU;
/// they are overridden by per-target CostWeights during scalarisation.
egraph::MultiCost baselineCost(const egraph::ENode& node) noexcept;

/// Returns the NodeMeta for an e-node (instruction class, memory/branch flags).
egraph::NodeMeta nodeMetaFor(const egraph::ENode& node) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// HGOEGuidedConfig — tuning knobs for the guided optimizer
// ─────────────────────────────────────────────────────────────────────────────

struct HGOEGuidedConfig {
    /// Hard upper bound on the total number of e-nodes.  When reached, no new
    /// nodes are added but the current graph is still scored and extracted.
    size_t nodeLimit = 20'000;

    /// Maximum number of expand-score-prune iterations.
    size_t iterLimit = 25;

    /// Maximum number of new nodes added per iteration (throttles expansion).
    size_t nodesPerIter = 2'000;

    /// Pruning threshold: a class is pruned when
    ///   scalarize(lowerBound, weights) > pruneRatio * scalarize(globalBest, weights)
    /// A value of 1.0 means "only prune if strictly worse than best so far";
    /// 1.2 means "prune if more than 20% worse than best so far".
    double pruneRatio = 1.25;

    /// Whether to enable constant folding during expansion.
    bool enableConstantFolding = true;

    /// Whether to try commutative orderings for rules with commutative LHS.
    bool enableCommutativity = true;

    /// When true, use a stable sort for tie-breaking so that outputs are
    /// deterministic across platforms regardless of memory layout.
    bool deterministicMode = true;

    /// Hardware-specific scalarisation weights.  Populated from the
    /// MicroarchProfile by the orchestrator when HGOE is active.
    CostWeights weights;
};

// ─────────────────────────────────────────────────────────────────────────────
// HGOEGuidedStats — diagnostics from one optimizer run
// ─────────────────────────────────────────────────────────────────────────────

struct HGOEGuidedStats {
    size_t iterations       = 0;  ///< Full expand-score-prune rounds executed
    size_t nodesAdded       = 0;  ///< Total new e-nodes created
    size_t classesPruned    = 0;  ///< Classes pruned by HGOE lower-bound check
    size_t merges           = 0;  ///< Successful e-class merges
    size_t rewrites         = 0;  ///< Rewrite rules applied (inc. duplicate LHS)
    double estimatedSpeedup = 0.0; ///< Scalar-cost ratio: original / best found
    bool   hitNodeLimit     = false; ///< True if nodeLimit was reached
    bool   hitIterLimit     = false; ///< True if iterLimit was reached
};

// ─────────────────────────────────────────────────────────────────────────────
// HGOEGuidedOptimizer — the core engine
// ─────────────────────────────────────────────────────────────────────────────

/// Fused HGOE-guided equality-saturation optimizer.
///
/// Unlike the plain EGraph which separates "saturate all rewrites" from
/// "extract cheapest node", this class interleaves scoring and pruning with
/// the rewrite expansion so that expensive branches are abandoned early.
///
/// Usage:
/// @code
///   HGOEGuidedConfig cfg;
///   cfg.weights.latency = 2.5;     // heavier latency weight for this target
///   HGOEGuidedOptimizer opt(cfg);
///
///   ClassId root = opt.seed(expr);                // populate initial class
///   HGOEGuidedStats stats = opt.optimize(rules);  // expand + score + prune
///   ENode best = opt.extractBest(root);           // retrieve winner
/// @endcode
class HGOEGuidedOptimizer {
public:
    explicit HGOEGuidedOptimizer(HGOEGuidedConfig config = {});

    // ── Graph construction ───────────────────────────────────────────────

    /// Seed the optimizer with a root expression; returns its ClassId.
    [[nodiscard]] egraph::ClassId seed(egraph::ENode node);

    /// Convenience: seed a constant integer.
    [[nodiscard]] egraph::ClassId seedConst(long long val);

    /// Convenience: seed a named variable.
    [[nodiscard]] egraph::ClassId seedVar(const std::string& name);

    /// Convenience: seed a binary operation.
    [[nodiscard]] egraph::ClassId seedBinOp(egraph::Op op,
                                            egraph::ClassId lhs,
                                            egraph::ClassId rhs);

    // ── Guided optimization loop ─────────────────────────────────────────

    /// Run the guided expand-score-prune loop.
    /// Applies all rules, scores new classes via HGOE, prunes dominated
    /// branches, and propagates best costs upward.
    /// @param rules  Rewrite rules to apply.
    /// @param root   ClassId of the root expression being optimised.
    ///               The global-best pruning threshold is anchored to the root's
    ///               cost, not to the minimum cost across all classes (leaf nodes
    ///               such as Var/Const are naturally cheap and must not pull the
    ///               pruning threshold below the root's actual minimum).
    ///               Pass ClassId(-1) to disable root-anchored pruning.
    /// Returns statistics from the run.
    [[nodiscard]] HGOEGuidedStats
    optimize(const std::vector<egraph::RewriteRule>& rules,
             egraph::ClassId root = static_cast<egraph::ClassId>(-1));

    // ── Extraction ───────────────────────────────────────────────────────

    /// Extract the best-known ENode for the given class.
    /// Returns the bestNode field directly — O(1).
    [[nodiscard]] egraph::ENode extractBest(egraph::ClassId root);

    /// Get the scalar cost of the best representative for a class.
    [[nodiscard]] double bestScalarCost(egraph::ClassId root);

    // ── Accessors ────────────────────────────────────────────────────────

    [[nodiscard]] const egraph::EGraph& graph() const noexcept { return graph_; }
    [[nodiscard]]       egraph::EGraph& graph()       noexcept { return graph_; }
    [[nodiscard]] const HGOEGuidedConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] size_t numClasses() const;
    [[nodiscard]] size_t numNodes()   const;

private:
    egraph::EGraph      graph_;
    HGOEGuidedConfig    cfg_;

    // ── Internal helpers ─────────────────────────────────────────────────

    /// Score a single class via HGOE, update bestNode/bestCost/hgoeState.
    /// Returns true if bestCost improved (triggers upward propagation).
    bool scoreClass(egraph::ClassId id);

    /// Compute the MultiCost of an e-node given its children's bestCost.
    egraph::MultiCost computeNodeCost(const egraph::ENode& node,
                                      egraph::ClassId      cls);

    /// Propagate updated costs bottom-up through the use-list.
    /// Starts from `dirtyClasses` and walks the parent_ links in the EGraph.
    void propagateCosts(const std::vector<egraph::ClassId>& dirtyClasses);

    /// Update lowerBound for a class based on children's lowerBounds.
    void updateLowerBound(egraph::ClassId id);

    /// Prune all classes whose lowerBound scalar exceeds `threshold`.
    /// Returns count of newly pruned classes.
    size_t pruneBelow(double threshold);

    /// Build FeatureVec for a class from its nodes' NodeMeta.
    egraph::FeatureVec buildFeatureVec(egraph::ClassId id);

    /// Compute scalar cost for a MultiCost using cfg_.weights.
    double scalar(const egraph::MultiCost& mc) const noexcept;

    /// Global best scalar cost seen so far (used for pruning threshold).
    double globalBest_ = 1e18;

    /// Initial (unseeded) scalar cost of the root expression.
    double initialCost_ = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Top-level AST optimisation entry point
// ─────────────────────────────────────────────────────────────────────────────

/// Run the HGOE-guided optimizer on all expression trees in `program`.
/// This is the single entry point called from the orchestrator's runHGOEEGraph.
///
/// The function:
///   1. Iterates all functions in the program (via forEachFunction).
///   2. For each expression, seeds the HGOEGuidedOptimizer.
///   3. Runs the guided loop with the full algebraic + advanced rule set.
///   4. Extracts the best node and converts it back to an AST expression.
///   5. Replaces the original expression in-place if a cheaper form was found.
///
/// Returns aggregate statistics summed across all expressions optimized.
HGOEGuidedStats runHGOEGuidedPass(Program& program,
                                   const HGOEGuidedConfig& config,
                                   bool verbose = false);

} // namespace hgoe_egraph
} // namespace omscript

#endif // HGOE_EGRAPH_H
