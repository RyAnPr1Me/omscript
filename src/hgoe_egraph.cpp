/// @file hgoe_egraph.cpp
/// @brief HGOE-Guided E-Graph Superoptimizer — implementation.
///
/// This module implements the fused equality-saturation + HGOE-scoring pass
/// described in hgoe_egraph.h.  The main entry point is runHGOEGuidedPass()
/// which is called from the orchestrator.
///
/// Implementation outline
/// ═════════════════════
/// 1. astToEGraph()       — shared helper (reimplemented inline here, same as
///                          egraph_optimizer.cpp but using HGOEGuidedOptimizer)
/// 2. baselineCost()      — per-Op MultiCost defaults for generic x86-64
/// 3. nodeMetaFor()       — per-Op NodeMeta classification
/// 4. HGOEGuidedOptimizer — guided expand-score-prune loop
/// 5. eNodeToAST()        — back-conversion to AST using bestNode map
/// 6. runHGOEGuidedPass() — top-level driver called from the orchestrator

#include "hgoe_egraph.h"
#include "ast.h"
#include "egraph.h"
#include "opt_context.h"
#include "pass_utils.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript {
namespace hgoe_egraph {

using namespace egraph;

// ─────────────────────────────────────────────────────────────────────────────
// baselineCost — per-Op MultiCost defaults
// ─────────────────────────────────────────────────────────────────────────────

// instrClass encoding (matches NodeMeta::instrClass)
static constexpr uint8_t kIntArith   = 0;
static constexpr uint8_t kIntMul     = 1;
static constexpr uint8_t kIntDiv     = 2;
static constexpr uint8_t kFPArith    = 3;
static constexpr uint8_t kFPMul      = 4;
static constexpr uint8_t kFPDiv      = 5;
static constexpr uint8_t kVectorOp   = 6;
static constexpr uint8_t kLoad       = 7;
static constexpr uint8_t kStore      = 8;
static constexpr uint8_t kBranch     = 9;
static constexpr uint8_t kShift      = 10;
static constexpr uint8_t kComparison = 11;
static constexpr uint8_t kOther      = 12;

egraph::MultiCost baselineCost(const ENode& node) noexcept {
    MultiCost mc;
    switch (node.op) {
    // Constants and variables: essentially free (register rename / immediate)
    case Op::Const:
    case Op::ConstF:
    case Op::Var:
    case Op::Nop:
        mc.cycles = 0.0; mc.uops = 0.0; mc.latency = 0.0;
        mc.registerPressure = 1.0;
        break;

    // Integer arithmetic: 1 cycle latency, 0.25 cycle throughput on modern OOO
    case Op::Add:
    case Op::Sub:
    case Op::Neg:
        mc.cycles = 0.25; mc.uops = 1.0; mc.latency = 1.0;
        mc.throughputPressure = 0.25; mc.registerPressure = 1.0;
        break;

    // Integer multiply: 3 cycle latency, 1 cycle throughput
    case Op::Mul:
        mc.cycles = 1.0; mc.uops = 1.0; mc.latency = 3.0;
        mc.throughputPressure = 1.0; mc.registerPressure = 1.0;
        break;

    // Integer divide/mod: expensive — 20-30 cycle latency on x86-64
    case Op::Div:
    case Op::Mod:
        mc.cycles = 25.0; mc.uops = 10.0; mc.latency = 25.0;
        mc.throughputPressure = 25.0; mc.registerPressure = 1.0;
        break;

    // Power: depends on implementation; model as a call (expensive)
    case Op::Pow:
    case Op::Sqrt:
        mc.cycles = 20.0; mc.uops = 6.0; mc.latency = 20.0;
        mc.throughputPressure = 20.0; mc.registerPressure = 1.0;
        break;

    // Bitwise ops: same as add (ALU, 1 cycle)
    case Op::BitAnd:
    case Op::BitOr:
    case Op::BitXor:
    case Op::BitNot:
    case Op::LogAnd:
    case Op::LogOr:
    case Op::LogNot:
        mc.cycles = 0.25; mc.uops = 1.0; mc.latency = 1.0;
        mc.throughputPressure = 0.25; mc.registerPressure = 1.0;
        break;

    // Shifts: 1 cycle latency
    case Op::Shl:
    case Op::Shr:
        mc.cycles = 0.5; mc.uops = 1.0; mc.latency = 1.0;
        mc.throughputPressure = 0.5; mc.registerPressure = 1.0;
        break;

    // Comparisons: 1 cycle (flag-setting is free on x86 as part of ALU op)
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
        mc.cycles = 0.5; mc.uops = 1.0; mc.latency = 1.0;
        mc.throughputPressure = 0.5; mc.registerPressure = 1.0;
        break;

    // Ternary (select): 1 cycle cmov
    case Op::Ternary:
        mc.cycles = 0.5; mc.uops = 1.0; mc.latency = 1.0;
        mc.branchPenalty = 0.0; mc.registerPressure = 1.0;
        break;

    // Call: very expensive (call overhead + unknown callee)
    case Op::Call:
        mc.cycles = 10.0; mc.uops = 4.0; mc.latency = 10.0;
        mc.throughputPressure = 5.0; mc.registerPressure = 4.0;
        mc.memoryPressure = 1.0; // may touch stack
        break;
    }
    return mc;
}

// ─────────────────────────────────────────────────────────────────────────────
// nodeMetaFor — per-Op NodeMeta classification
// ─────────────────────────────────────────────────────────────────────────────

egraph::NodeMeta nodeMetaFor(const ENode& node) noexcept {
    NodeMeta m;
    switch (node.op) {
    case Op::Const:
    case Op::ConstF:
    case Op::Var:
    case Op::Nop:
        m.instrClass = kOther;
        break;

    case Op::Add:
    case Op::Sub:
    case Op::Neg:
    case Op::BitAnd:
    case Op::BitOr:
    case Op::BitXor:
    case Op::BitNot:
    case Op::LogAnd:
    case Op::LogOr:
    case Op::LogNot:
        m.instrClass = kIntArith;
        break;

    case Op::Mul:
        m.instrClass = kIntMul;
        break;

    case Op::Div:
    case Op::Mod:
        m.instrClass = kIntDiv;
        break;

    case Op::Shl:
    case Op::Shr:
        m.instrClass = kShift;
        break;

    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
        m.instrClass = kComparison;
        break;

    case Op::Pow:
    case Op::Sqrt:
        m.instrClass = kFPMul; // approximate
        break;

    case Op::Ternary:
        m.instrClass = kBranch;
        m.hasBranch = true;
        break;

    case Op::Call:
        m.instrClass = kOther;
        m.readsMemory = true; // conservative: unknown callee
        break;
    }
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// HGOEGuidedOptimizer — implementation
// ─────────────────────────────────────────────────────────────────────────────

HGOEGuidedOptimizer::HGOEGuidedOptimizer(HGOEGuidedConfig config)
    : graph_(SaturationConfig{config.nodeLimit,
                               config.iterLimit,
                               config.enableConstantFolding}),
      cfg_(std::move(config)) {}

egraph::ClassId HGOEGuidedOptimizer::seed(ENode node) {
    return graph_.add(std::move(node));
}
egraph::ClassId HGOEGuidedOptimizer::seedConst(long long val) {
    return graph_.addConst(val);
}
egraph::ClassId HGOEGuidedOptimizer::seedVar(const std::string& name) {
    return graph_.addVar(name);
}
egraph::ClassId HGOEGuidedOptimizer::seedBinOp(Op op, ClassId lhs, ClassId rhs) {
    return graph_.addBinOp(op, lhs, rhs);
}

size_t HGOEGuidedOptimizer::numClasses() const { return graph_.numClasses(); }
size_t HGOEGuidedOptimizer::numNodes()   const { return graph_.numNodes(); }

double HGOEGuidedOptimizer::scalar(const MultiCost& mc) const noexcept {
    return scalarize(mc, cfg_.weights);
}

// ── computeNodeCost ──────────────────────────────────────────────────────────

MultiCost HGOEGuidedOptimizer::computeNodeCost(const ENode& node,
                                                ClassId /*cls*/) {
    MultiCost mc = baselineCost(node);
    // Add children's best-known costs (critical-path max for latency,
    // sum for throughput and register pressure).
    double maxChildLatency = 0.0;
    double sumChildCycles  = 0.0;
    double maxChildReg     = 0.0;
    double sumChildMem     = 0.0;

    for (ClassId childId : node.children) {
        ClassId canonical = graph_.find(childId);
        const EClass& child = graph_.getClass(canonical);
        const MultiCost& cc = child.bestCost;
        maxChildLatency = std::max(maxChildLatency, cc.latency);
        sumChildCycles  += cc.cycles;
        maxChildReg     = std::max(maxChildReg, cc.registerPressure);
        sumChildMem     += cc.memoryPressure;
    }

    mc.latency            += maxChildLatency;
    mc.cycles             += sumChildCycles;
    mc.registerPressure   += maxChildReg;
    mc.memoryPressure     += sumChildMem;
    mc.throughputPressure += mc.cycles;  // simple model: throughput tracks cycles
    return mc;
}

// ── scoreClass ───────────────────────────────────────────────────────────────

bool HGOEGuidedOptimizer::scoreClass(ClassId id) {
    ClassId canonical = graph_.find(id);
    EClass& cls = const_cast<EClass&>(graph_.getClass(canonical));

    bool improved = false;
    for (const ENode& node : cls.nodes) {
        if (cls.hgoePruned) break;
        // Skip self-referential nodes: rules like `x * 1 → x` merge the Mul
        // class into the Var class, creating nodes whose canonicalized children
        // point back to the containing class.  Such nodes have infinite cost
        // regardless, but skipping them early prevents them from being
        // selected as bestNode via the nodes.front() fallback in extractBest.
        bool selfRef = false;
        for (ClassId child : node.children) {
            if (graph_.find(child) == canonical) { selfRef = true; break; }
        }
        if (selfRef) continue;
        MultiCost cost = computeNodeCost(node, canonical);
        if (scalar(cost) < scalar(cls.bestCost)) {
            cls.bestCost = cost;
            cls.bestNode = node;
            cls.upperBound = cost;
            cls.hgoeState.valid = false; // invalidate cached features
            improved = true;
        }
    }

    // Rebuild features if stale
    if (!cls.hgoeState.valid) {
        cls.hgoeState.features = buildFeatureVec(canonical);
        cls.hgoeState.scoredCost = cls.bestCost;
        cls.hgoeState.valid = true;
    }

    return improved;
}

// ── buildFeatureVec ──────────────────────────────────────────────────────────

FeatureVec HGOEGuidedOptimizer::buildFeatureVec(ClassId id) {
    ClassId canonical = graph_.find(id);
    const EClass& cls = graph_.getClass(canonical);
    FeatureVec fv;
    for (const ENode& node : cls.nodes) {
        NodeMeta m = nodeMetaFor(node);
        fv.addNode(m);
    }
    if (cls.constVal.has_value()) fv.isConstant = true;
    if (cls.bestNode.has_value()) {
        fv.depth = static_cast<uint16_t>(cls.bestCost.latency);
        fv.regPressure = static_cast<uint16_t>(
            std::min(cls.bestCost.registerPressure, 65535.0));
    }
    return fv;
}

// ── updateLowerBound ─────────────────────────────────────────────────────────

void HGOEGuidedOptimizer::updateLowerBound(ClassId id) {
    ClassId canonical = graph_.find(id);
    EClass& cls = const_cast<EClass&>(graph_.getClass(canonical));

    // Lower bound: node's own baseline cost (children's lower bounds summed)
    MultiCost lb;
    lb.cycles = lb.uops = lb.latency = lb.throughputPressure =
        lb.registerPressure = lb.memoryPressure = lb.branchPenalty = 0.0;

    for (const ENode& node : cls.nodes) {
        MultiCost nodeLB = baselineCost(node); // leaf cost only
        for (ClassId childId : node.children) {
            ClassId cc = graph_.find(childId);
            const EClass& child = graph_.getClass(cc);
            const MultiCost& clb = child.lowerBound;
            // Critical-path lower bound for latency
            nodeLB.latency += clb.latency;
            // Sum for the rest
            nodeLB.cycles             += clb.cycles;
            nodeLB.registerPressure   += clb.registerPressure;
            nodeLB.memoryPressure     += clb.memoryPressure;
            nodeLB.throughputPressure += clb.throughputPressure;
            nodeLB.branchPenalty      += clb.branchPenalty;
        }
        // The best possible lower bound across all nodes in this class
        if (scalar(nodeLB) < scalar(lb) || cls.nodes.size() == 1) {
            lb = nodeLB;
        }
    }
    cls.lowerBound = lb;
}

// ── propagateCosts ───────────────────────────────────────────────────────────

void HGOEGuidedOptimizer::propagateCosts(
        const std::vector<ClassId>& dirtyClasses) {
    // Simple BFS upward through the parent use-list exposed by EGraph.
    // We use the graph's getClass + nodes to find parents implicitly by
    // scanning all classes (for simplicity given the existing EGraph API).
    // This is bounded by the total node count which is capped by nodeLimit.
    if (dirtyClasses.empty()) return;

    std::unordered_set<ClassId> todo(dirtyClasses.begin(), dirtyClasses.end());
    std::unordered_set<ClassId> nextTodo;

    // BFS: at most O(depth) rounds; each round is O(totalNodes)
    for (int round = 0; round < 16 && !todo.empty(); ++round) {
        nextTodo.clear();
        for (size_t i = 0; i < graph_.numClasses(); ++i) {
            // Walk all classes; EGraph doesn't expose an iterator by index,
            // so we iterate ClassId range [0, numClasses()).
            // Note: ClassId is uint32_t; valid IDs are 0..numClasses()-1 but
            // some may be merged away. find() canonicalises them.
            ClassId cid = static_cast<ClassId>(i);
            ClassId canonical = graph_.find(cid);
            if (canonical != cid) continue; // merged into another class

            const EClass& cls = graph_.getClass(canonical);
            for (const ENode& node : cls.nodes) {
                for (ClassId child : node.children) {
                    if (todo.count(graph_.find(child))) {
                        // This class references a dirty child — re-score it.
                        if (scoreClass(canonical)) {
                            nextTodo.insert(canonical);
                        }
                        break;
                    }
                }
            }
        }
        todo = nextTodo;
    }
}

// ── pruneBelow ───────────────────────────────────────────────────────────────

size_t HGOEGuidedOptimizer::pruneBelow(double threshold) {
    size_t count = 0;
    for (size_t i = 0; i < graph_.numClasses(); ++i) {
        ClassId cid = static_cast<ClassId>(i);
        ClassId canonical = graph_.find(cid);
        if (canonical != cid) continue;
        EClass& cls = const_cast<EClass&>(graph_.getClass(canonical));
        if (cls.hgoePruned) continue;
        if (scalar(cls.lowerBound) > threshold) {
            cls.hgoePruned = true;
            ++count;
        }
    }
    return count;
}

// ── optimize ─────────────────────────────────────────────────────────────────

HGOEGuidedStats HGOEGuidedOptimizer::optimize(
        const std::vector<RewriteRule>& rules,
        ClassId root) {
    HGOEGuidedStats stats;
    const bool haveRoot = (root != static_cast<ClassId>(-1));

    // Phase 0: initial scoring pass to populate bestNode/bestCost for all
    // classes seeded before this call.
    for (size_t i = 0; i < graph_.numClasses(); ++i) {
        ClassId cid = static_cast<ClassId>(i);
        if (graph_.find(cid) == cid) {
            scoreClass(cid);
            updateLowerBound(cid);
        }
    }

    // Anchor globalBest_ to the ROOT expression, not to the cheapest class
    // in the graph.  Leaf nodes (Var, Const) have very low costs and would
    // otherwise make the pruning threshold extremely tight, causing all
    // non-trivial intermediate classes to be pruned immediately.
    if (haveRoot) {
        ClassId canonical = graph_.find(root);
        const EClass& rootCls = graph_.getClass(canonical);
        globalBest_  = scalar(rootCls.bestCost);
        initialCost_ = globalBest_;
    } else {
        // Fallback: use min across all classes (may over-prune, acceptable
        // when the caller has not provided a root).
        for (size_t i = 0; i < graph_.numClasses(); ++i) {
            ClassId cid = static_cast<ClassId>(i);
            if (graph_.find(cid) != cid) continue;
            const EClass& cls = graph_.getClass(cid);
            double s = scalar(cls.bestCost);
            if (s < globalBest_) globalBest_ = s;
        }
        initialCost_ = globalBest_;
    }

    // Phase 1: main guided expand-score-prune loop
    for (size_t iter = 0; iter < cfg_.iterLimit; ++iter) {
        if (graph_.atNodeLimit()) {
            stats.hitNodeLimit = true;
            break;
        }

        // 1a. Apply rewrite rules (throttled to nodesPerIter new nodes)
        size_t nodesBefore = graph_.numNodes();
        size_t appliedThisIter = graph_.applyRules(rules);
        stats.rewrites += appliedThisIter;
        size_t nodesAdded = graph_.numNodes() - nodesBefore;
        stats.nodesAdded += nodesAdded;

        if (appliedThisIter == 0 && nodesAdded == 0) {
            // Fixpoint reached
            break;
        }

        // 1b. Score all modified/new classes and collect dirty set
        std::vector<ClassId> dirty;
        for (size_t i = 0; i < graph_.numClasses(); ++i) {
            ClassId cid = static_cast<ClassId>(i);
            if (graph_.find(cid) != cid) continue;
            const EClass& cls = graph_.getClass(cid);
            if (!cls.hgoePruned && !cls.hgoeState.valid) {
                if (scoreClass(cid)) {
                    dirty.push_back(cid);
                }
            }
        }
        // Update globalBest_ from the ROOT class only (root-anchored pruning).
        if (haveRoot) {
            ClassId canonical = graph_.find(root);
            const EClass& rootCls = graph_.getClass(canonical);
            double s = scalar(rootCls.bestCost);
            if (s < globalBest_) globalBest_ = s;
        } else {
            for (ClassId cid : dirty) {
                const EClass& cls = graph_.getClass(graph_.find(cid));
                double s = scalar(cls.bestCost);
                if (s < globalBest_) globalBest_ = s;
            }
        }

        // 1c. Propagate cost improvements upward
        propagateCosts(dirty);

        // 1d. Update lower bounds
        for (size_t i = 0; i < graph_.numClasses(); ++i) {
            ClassId cid = static_cast<ClassId>(i);
            if (graph_.find(cid) == cid) updateLowerBound(cid);
        }

        // 1e. Prune dominated branches
        double pruneThreshold = globalBest_ * cfg_.pruneRatio;
        stats.classesPruned += pruneBelow(pruneThreshold);

        ++stats.iterations;

        // Throttle: stop this round if we already added enough nodes
        if (nodesAdded >= cfg_.nodesPerIter) break;
    }

    if (stats.iterations >= cfg_.iterLimit) stats.hitIterLimit = true;

    // Phase 2: final scoring sweep to ensure all best-nodes are up to date
    for (size_t i = 0; i < graph_.numClasses(); ++i) {
        ClassId cid = static_cast<ClassId>(i);
        if (graph_.find(cid) == cid) scoreClass(cid);
    }

    // Compute estimated speedup
    if (globalBest_ > 0 && initialCost_ > 0)
        stats.estimatedSpeedup = (initialCost_ - globalBest_) / initialCost_ * 100.0;

    return stats;
}

// ── extractBest / bestScalarCost ─────────────────────────────────────────────

ENode HGOEGuidedOptimizer::extractBest(ClassId root) {
    ClassId canonical = graph_.find(root);
    const EClass& cls = graph_.getClass(canonical);
    if (cls.bestNode.has_value()) return *cls.bestNode;
    // Fallback: return first node
    if (!cls.nodes.empty()) return cls.nodes.front();
    return ENode(Op::Const, 0LL);
}

double HGOEGuidedOptimizer::bestScalarCost(ClassId root) {
    ClassId canonical = graph_.find(root);
    const EClass& cls = graph_.getClass(canonical);
    return scalar(cls.bestCost);
}

// ─────────────────────────────────────────────────────────────────────────────
// AST ↔ EGraph conversion helpers
// ─────────────────────────────────────────────────────────────────────────────

static ClassId astToEGraph(HGOEGuidedOptimizer& opt, const Expression* expr) {
    if (!expr) return opt.seedConst(0);

    switch (expr->type) {
    case ASTNodeType::LITERAL_EXPR: {
        auto* lit = static_cast<const LiteralExpr*>(expr);
        switch (lit->literalType) {
        case LiteralExpr::LiteralType::INTEGER:
            return opt.seedConst(lit->intValue);
        case LiteralExpr::LiteralType::FLOAT: {
            ENode n(Op::ConstF, lit->floatValue);
            return opt.seed(std::move(n));
        }
        case LiteralExpr::LiteralType::STRING:
            return opt.seedVar("__str_" + lit->stringValue);
        }
        break;
    }

    case ASTNodeType::IDENTIFIER_EXPR: {
        auto* id = static_cast<const IdentifierExpr*>(expr);
        return opt.seedVar(id->name);
    }

    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(expr);
        ClassId lhs = astToEGraph(opt, bin->left.get());
        ClassId rhs = astToEGraph(opt, bin->right.get());

        Op op = Op::Nop;
        if      (bin->op == "+")  op = Op::Add;
        else if (bin->op == "-")  op = Op::Sub;
        else if (bin->op == "*")  op = Op::Mul;
        else if (bin->op == "/")  op = Op::Div;
        else if (bin->op == "%")  op = Op::Mod;
        else if (bin->op == "**") op = Op::Pow;
        else if (bin->op == "&")  op = Op::BitAnd;
        else if (bin->op == "|")  op = Op::BitOr;
        else if (bin->op == "^")  op = Op::BitXor;
        else if (bin->op == "<<") op = Op::Shl;
        else if (bin->op == ">>") op = Op::Shr;
        else if (bin->op == "==") op = Op::Eq;
        else if (bin->op == "!=") op = Op::Ne;
        else if (bin->op == "<")  op = Op::Lt;
        else if (bin->op == "<=") op = Op::Le;
        else if (bin->op == ">")  op = Op::Gt;
        else if (bin->op == ">=") op = Op::Ge;
        else if (bin->op == "&&") op = Op::LogAnd;
        else if (bin->op == "||") op = Op::LogOr;
        else return opt.seedVar("__binop_" + bin->op);

        return opt.seedBinOp(op, lhs, rhs);
    }

    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<const UnaryExpr*>(expr);
        ClassId operand = astToEGraph(opt, un->operand.get());
        Op op = Op::Nop;
        if      (un->op == "-") op = Op::Neg;
        else if (un->op == "~") op = Op::BitNot;
        else if (un->op == "!") op = Op::LogNot;
        else return opt.seedVar("__unop_" + un->op);
        ENode n(op, std::vector<ClassId>{operand});
        return opt.seed(std::move(n));
    }

    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const TernaryExpr*>(expr);
        ClassId cond  = astToEGraph(opt, tern->condition.get());
        ClassId thenE = astToEGraph(opt, tern->thenExpr.get());
        ClassId elseE = astToEGraph(opt, tern->elseExpr.get());
        ENode n(Op::Ternary);
        n.children = {cond, thenE, elseE};
        return opt.seed(std::move(n));
    }

    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(expr);
        std::vector<ClassId> argIds;
        argIds.reserve(call->arguments.size());
        for (const auto& arg : call->arguments)
            argIds.push_back(astToEGraph(opt, arg.get()));
        ENode n(Op::Call, call->callee, std::move(argIds));
        return opt.seed(std::move(n));
    }

    default:
        return opt.seedVar("__opaque");
    }

    return opt.seedConst(0);
}

/// Map enode Op → OmScript operator string (binary).
static std::string opToString(Op op) {
    switch (op) {
    case Op::Add: return "+";    case Op::Sub: return "-";
    case Op::Mul: return "*";    case Op::Div: return "/";
    case Op::Mod: return "%";    case Op::Pow: return "**";
    case Op::BitAnd: return "&"; case Op::BitOr: return "|";
    case Op::BitXor: return "^"; case Op::Shl: return "<<";
    case Op::Shr: return ">>";   case Op::Eq: return "==";
    case Op::Ne: return "!=";    case Op::Lt: return "<";
    case Op::Le: return "<=";    case Op::Gt: return ">";
    case Op::Ge: return ">=";    case Op::LogAnd: return "&&";
    case Op::LogOr: return "||";
    default: return "";
    }
}

static bool isBinaryOp(Op op) {
    switch (op) {
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
    case Op::BitAnd: case Op::BitOr: case Op::BitXor:
    case Op::Shl: case Op::Shr:
    case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
    case Op::LogAnd: case Op::LogOr: case Op::Pow:
        return true;
    default: return false;
    }
}

static bool isUnaryOp(Op op) {
    switch (op) {
    case Op::Neg: case Op::BitNot: case Op::LogNot: return true;
    default: return false;
    }
}

/// Recursively convert the best ENode in `cls` back to an AST Expression.
/// Uses a flat bestNode map built from each class's bestNode field.
static std::unique_ptr<Expression> eNodeToAST(
        HGOEGuidedOptimizer& opt,
        ClassId cls,
        std::unordered_map<ClassId, ENode>& visited) {

    ClassId canonical = opt.graph().find(cls);
    // Cycle guard / DAG-sharing: if we have already started extracting this
    // class (visited), reconstruct a leaf expression from the cached node to
    // avoid infinite recursion.  With the self-referential node fix in
    // scoreClass this should be rare, but guard defensively.
    auto it = visited.find(canonical);
    if (it != visited.end()) {
        const ENode& cached = it->second;
        switch (cached.op) {
        case Op::Var:
            return std::make_unique<IdentifierExpr>(cached.name);
        case Op::Const:
            return std::make_unique<LiteralExpr>(cached.value);
        case Op::ConstF:
            return std::make_unique<LiteralExpr>(cached.fvalue);
        default:
            // True structural cycle — should not happen after self-ref fix;
            // return a safe placeholder to avoid UB.
            return std::make_unique<LiteralExpr>(0LL);
        }
    }

    ENode best = opt.extractBest(canonical);
    visited[canonical] = best; // mark as in-progress

    switch (best.op) {
    case Op::Const: {
        auto lit = std::make_unique<LiteralExpr>(best.value);
        return lit;
    }
    case Op::ConstF: {
        auto lit = std::make_unique<LiteralExpr>(best.fvalue);
        return lit;
    }
    case Op::Var: {
        // Strip compiler-internal prefixes
        if (best.name.substr(0, 7) == "__str_")
            return std::make_unique<LiteralExpr>(best.name.substr(7));
        return std::make_unique<IdentifierExpr>(best.name);
    }
    case Op::Nop:
        return std::make_unique<LiteralExpr>(0LL);

    default:
        if (isBinaryOp(best.op) && best.children.size() == 2) {
            auto lhs = eNodeToAST(opt, best.children[0], visited);
            auto rhs = eNodeToAST(opt, best.children[1], visited);
            if (!lhs || !rhs) return std::make_unique<LiteralExpr>(0LL);
            return std::make_unique<BinaryExpr>(
                    opToString(best.op), std::move(lhs), std::move(rhs));
        }
        if (isUnaryOp(best.op) && best.children.size() == 1) {
            auto operand = eNodeToAST(opt, best.children[0], visited);
            if (!operand) return std::make_unique<LiteralExpr>(0LL);
            std::string uop = (best.op == Op::Neg ? "-" :
                               best.op == Op::BitNot ? "~" : "!");
            return std::make_unique<UnaryExpr>(uop, std::move(operand));
        }
        if (best.op == Op::Ternary && best.children.size() == 3) {
            auto cond  = eNodeToAST(opt, best.children[0], visited);
            auto thenE = eNodeToAST(opt, best.children[1], visited);
            auto elseE = eNodeToAST(opt, best.children[2], visited);
            if (!cond || !thenE || !elseE)
                return std::make_unique<LiteralExpr>(0LL);
            return std::make_unique<TernaryExpr>(
                    std::move(cond), std::move(thenE), std::move(elseE));
        }
        if (best.op == Op::Call) {
            std::vector<std::unique_ptr<Expression>> args;
            args.reserve(best.children.size());
            for (ClassId child : best.children) {
                auto arg = eNodeToAST(opt, child, visited);
                if (!arg) arg = std::make_unique<LiteralExpr>(0LL);
                args.push_back(std::move(arg));
            }
            return std::make_unique<CallExpr>(best.name, std::move(args));
        }
        // Fallback: return an identifier named after the op
        return std::make_unique<LiteralExpr>(0LL);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Collect all rules for the guided optimizer
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<RewriteRule> getAllGuidedRules() {
    auto rules = getAlgebraicRules();
    auto adv   = getAdvancedAlgebraicRules();
    auto cmp   = getComparisonRules();
    auto advC  = getAdvancedComparisonRules();
    auto bw    = getBitwiseRules();
    auto advB  = getAdvancedBitwiseRules();
    rules.insert(rules.end(), std::make_move_iterator(adv.begin()),
                               std::make_move_iterator(adv.end()));
    rules.insert(rules.end(), std::make_move_iterator(cmp.begin()),
                               std::make_move_iterator(cmp.end()));
    rules.insert(rules.end(), std::make_move_iterator(advC.begin()),
                               std::make_move_iterator(advC.end()));
    rules.insert(rules.end(), std::make_move_iterator(bw.begin()),
                               std::make_move_iterator(bw.end()));
    rules.insert(rules.end(), std::make_move_iterator(advB.begin()),
                               std::make_move_iterator(advB.end()));
    return rules;
}

// ─────────────────────────────────────────────────────────────────────────────
// Recursive expression rewriter
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Round-trip guard — only optimise expressions we can faithfully reconstruct
// ─────────────────────────────────────────────────────────────────────────────

/// Returns true if the expression subtree can be safely seeded into an e-graph
/// and reconstructed back to semantically equivalent AST.  Types that cannot
/// be represented (IndexExpr, FieldExpr, etc.) map to the opaque sentinel and
/// would produce an unknown-variable error after extraction.
static bool isRoundTrippable(const Expression* expr) {
    if (!expr) return false;
    switch (expr->type) {
    case ASTNodeType::LITERAL_EXPR: {
        // String literals map to a Var by prefix convention — acceptable only
        // if the content contains no characters that would corrupt the prefix
        // (this is conservative but safe).
        auto* lit = static_cast<const LiteralExpr*>(expr);
        return lit->literalType != LiteralExpr::LiteralType::STRING;
    }
    case ASTNodeType::IDENTIFIER_EXPR:
        return true;
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(expr);
        return isRoundTrippable(bin->left.get()) &&
               isRoundTrippable(bin->right.get());
    }
    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<const UnaryExpr*>(expr);
        return isRoundTrippable(un->operand.get());
    }
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const TernaryExpr*>(expr);
        return isRoundTrippable(tern->condition.get()) &&
               isRoundTrippable(tern->thenExpr.get()) &&
               isRoundTrippable(tern->elseExpr.get());
    }
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(expr);
        for (const auto& arg : call->arguments)
            if (!isRoundTrippable(arg.get())) return false;
        return true;
    }
    default:
        // IndexExpr, FieldExpr, AssignExpr, PostfixExpr, PrefixExpr, etc.
        // all fall through to false — we cannot reconstruct them from e-nodes.
        return false;
    }
}

/// Optimise a single expression in-place.  Returns the new (possibly
/// different) AST expression, or nullptr if no improvement found.
static std::unique_ptr<Expression> optimiseExpr(
        Expression* expr,
        const std::vector<RewriteRule>& rules,
        const HGOEGuidedConfig& cfg,
        HGOEGuidedStats& stats) {
    if (!expr) return nullptr;

    // Guard: only optimise expressions whose entire subtree can be faithfully
    // round-tripped through the e-graph.  Complex sub-trees (IndexExpr, etc.)
    // would be lost and replaced with an undefined __opaque variable.
    if (!isRoundTrippable(expr)) return nullptr;

    HGOEGuidedOptimizer opt(cfg);

    // Seed
    ClassId root = astToEGraph(opt, expr);

    // Capture original cost before rewrites (root-anchored: pass root so
    // the pruning threshold is set relative to this expression's cost).
    double origCost = 0.0;
    {
        HGOEGuidedStats pre = opt.optimize({}, root);  // no rules → just scoring
        origCost = opt.bestScalarCost(root);
        (void)pre;
    }

    // Rebuild with rules, anchoring pruning to the same root expression.
    HGOEGuidedOptimizer opt2(cfg);
    ClassId root2 = astToEGraph(opt2, expr);
    HGOEGuidedStats s = opt2.optimize(rules, root2);
    double bestCost = opt2.bestScalarCost(root2);

    stats.iterations    += s.iterations;
    stats.nodesAdded    += s.nodesAdded;
    stats.classesPruned += s.classesPruned;
    stats.rewrites      += s.rewrites;
    stats.merges        += s.merges;

    if (bestCost >= origCost * 0.99) {
        // Not meaningfully cheaper — skip
        return nullptr;
    }

    // Convert best back to AST
    std::unordered_map<ClassId, ENode> visited;
    auto result = eNodeToAST(opt2, root2, visited);
    if (!result) return nullptr;

    stats.estimatedSpeedup += (origCost > 0)
        ? (origCost - bestCost) / origCost * 100.0
        : 0.0;

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression visitor — walks every Expression* in the program
// ─────────────────────────────────────────────────────────────────────────────

/// Replace `*exprPtr` in-place with the HGOE-optimised form.
/// The statement/declaration owning the expression holds a unique_ptr<Expression>;
/// we receive a pointer-to-that-pointer so we can reassign it.
static void visitExpr(std::unique_ptr<Expression>& exprPtr,
                      const std::vector<RewriteRule>& rules,
                      const HGOEGuidedConfig& cfg,
                      HGOEGuidedStats& stats) {
    if (!exprPtr) return;
    Expression* expr = exprPtr.get();

    // Recurse into children first (bottom-up)
    switch (expr->type) {
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<BinaryExpr*>(expr);
        visitExpr(bin->left,  rules, cfg, stats);
        visitExpr(bin->right, rules, cfg, stats);
        break;
    }
    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<UnaryExpr*>(expr);
        visitExpr(un->operand, rules, cfg, stats);
        break;
    }
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<TernaryExpr*>(expr);
        visitExpr(tern->condition, rules, cfg, stats);
        visitExpr(tern->thenExpr,  rules, cfg, stats);
        visitExpr(tern->elseExpr,  rules, cfg, stats);
        break;
    }
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<CallExpr*>(expr);
        for (auto& arg : call->arguments)
            visitExpr(arg, rules, cfg, stats);
        break;
    }
    case ASTNodeType::ASSIGN_EXPR: {
        auto* asgn = static_cast<AssignExpr*>(expr);
        visitExpr(asgn->value, rules, cfg, stats);
        break;
    }
    case ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<IndexExpr*>(expr);
        visitExpr(idx->array, rules, cfg, stats);
        visitExpr(idx->index, rules, cfg, stats);
        break;
    }
    default:
        break;
    }

    // Try to replace current expression with a cheaper form
    if (auto improved = optimiseExpr(exprPtr.get(), rules, cfg, stats)) {
        exprPtr = std::move(improved);
    }
}

/// Walk all statements in a block, calling visitExpr for each expression.
static void visitStmt(Statement* stmt,
                      const std::vector<RewriteRule>& rules,
                      const HGOEGuidedConfig& cfg,
                      HGOEGuidedStats& stats);

static void visitBlock(BlockStmt* block,
                       const std::vector<RewriteRule>& rules,
                       const HGOEGuidedConfig& cfg,
                       HGOEGuidedStats& stats) {
    if (!block) return;
    for (auto& s : block->statements)
        visitStmt(s.get(), rules, cfg, stats);
}

static void visitStmt(Statement* stmt,
                      const std::vector<RewriteRule>& rules,
                      const HGOEGuidedConfig& cfg,
                      HGOEGuidedStats& stats) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::VAR_DECL: {
        auto* vd = static_cast<VarDecl*>(stmt);
        if (vd->initializer) visitExpr(vd->initializer, rules, cfg, stats);
        break;
    }
    case ASTNodeType::EXPR_STMT: {
        auto* es = static_cast<ExprStmt*>(stmt);
        if (es->expression) visitExpr(es->expression, rules, cfg, stats);
        break;
    }
    case ASTNodeType::RETURN_STMT: {
        auto* rs = static_cast<ReturnStmt*>(stmt);
        if (rs->value) visitExpr(rs->value, rules, cfg, stats);
        break;
    }
    case ASTNodeType::IF_STMT: {
        auto* is = static_cast<IfStmt*>(stmt);
        if (is->condition) visitExpr(is->condition, rules, cfg, stats);
        visitStmt(is->thenBranch.get(), rules, cfg, stats);
        if (is->elseBranch) visitStmt(is->elseBranch.get(), rules, cfg, stats);
        break;
    }
    case ASTNodeType::WHILE_STMT: {
        auto* ws = static_cast<WhileStmt*>(stmt);
        if (ws->condition) visitExpr(ws->condition, rules, cfg, stats);
        visitStmt(ws->body.get(), rules, cfg, stats);
        break;
    }
    case ASTNodeType::FOR_STMT: {
        auto* fs = static_cast<ForStmt*>(stmt);
        if (fs->start) visitExpr(fs->start, rules, cfg, stats);
        if (fs->end)   visitExpr(fs->end,   rules, cfg, stats);
        if (fs->step)  visitExpr(fs->step,  rules, cfg, stats);
        visitStmt(fs->body.get(), rules, cfg, stats);
        break;
    }
    case ASTNodeType::FOR_EACH_STMT: {
        auto* fes = static_cast<ForEachStmt*>(stmt);
        if (fes->collection) visitExpr(fes->collection, rules, cfg, stats);
        visitStmt(fes->body.get(), rules, cfg, stats);
        break;
    }
    case ASTNodeType::BLOCK: {
        visitBlock(static_cast<BlockStmt*>(stmt), rules, cfg, stats);
        break;
    }
    case ASTNodeType::THROW_STMT: {
        auto* ts = static_cast<ThrowStmt*>(stmt);
        if (ts->value) visitExpr(ts->value, rules, cfg, stats);
        break;
    }
    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runHGOEGuidedPass — top-level driver
// ─────────────────────────────────────────────────────────────────────────────

HGOEGuidedStats runHGOEGuidedPass(Program& program,
                                   const HGOEGuidedConfig& config,
                                   bool verbose) {
    HGOEGuidedStats total;

    // Collect all rules once (expensive: builds 200+ rule objects)
    const std::vector<RewriteRule> rules = getAllGuidedRules();

    forEachFunction(&program, [&](FunctionDecl* fn) {
        if (!fn || !fn->body) return;
        visitBlock(fn->body.get(), rules, config, total);
    });

    if (verbose) {
        // Diagnostic output intentionally omitted from hot path;
        // callers can inspect the returned stats struct instead.
        (void)total;
    }

    return total;
}

} // namespace hgoe_egraph
} // namespace omscript
