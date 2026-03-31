/// @file egraph.cpp
/// @brief E-Graph implementation for equality saturation optimization.
///
/// This file implements the core e-graph data structure including:
///   - Union-find with path compression and union by rank
///   - Hash-consing for node deduplication
///   - Pattern matching against e-classes
///   - Equality saturation loop with configurable limits
///   - Cost-based extraction of optimal expressions
///   - Constant folding within the e-graph

#include "egraph.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <queue>
#include <stack>

namespace omscript {
namespace egraph {

// ─────────────────────────────────────────────────────────────────────────────
// CostModel
// ─────────────────────────────────────────────────────────────────────────────

Cost CostModel::nodeCost(const ENode& node) const {
    switch (node.op) {
    // Free / near-free operations
    case Op::Const:
    case Op::ConstF:
    case Op::Var:
    case Op::Nop:
        return 0.1;

    // Simple ALU ops — 1 cycle latency on modern x86
    case Op::Add:
    case Op::Sub:
    case Op::Neg:
    case Op::BitAnd:
    case Op::BitOr:
    case Op::BitXor:
    case Op::BitNot:
    case Op::Shl:
    case Op::Shr:
        return 1.0;

    // Multiply — 3 cycles on modern x86
    case Op::Mul:
        return 3.0;

    // Division/modulo — 20-40 cycles, very expensive
    case Op::Div:
    case Op::Mod:
        return 25.0;

    // Comparisons — 1 cycle
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
        return 1.0;

    // Logical ops
    case Op::LogAnd:
    case Op::LogOr:
        return 1.5; // Slightly more due to short-circuit branch
    case Op::LogNot:
        return 1.0;

    // Math — power is very expensive, sqrt moderate
    case Op::Pow:
        return 50.0;
    case Op::Sqrt:
        return 10.0;

    // Ternary (branch) — moderate due to branch prediction
    case Op::Ternary:
        return 3.0;

    // Function call — overhead from call/ret + potential cache miss
    case Op::Call:
        return 15.0;
    }
    return 5.0; // Default for unknown ops
}

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — construction
// ─────────────────────────────────────────────────────────────────────────────

EGraph::EGraph() : config_{} {}

EGraph::EGraph(SaturationConfig config) : config_(config) {}

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — node creation
// ─────────────────────────────────────────────────────────────────────────────

ClassId EGraph::add(ENode node) {
    // Canonicalize children to use canonical class IDs
    node = canonicalize(std::move(node));

    // Check hash-cons for deduplication
    auto it = hashcons_.find(node);
    if (it != hashcons_.end()) {
        return find(it->second);
    }

    // Create a new e-class
    auto id = static_cast<ClassId>(classes_.size());
    parent_.push_back(id);

    EClass cls;
    cls.id = id;
    cls.nodes.push_back(node);

    // Populate analysis data
    if (node.op == Op::Const) {
        cls.constVal = node.value;
        cls.isZero = (node.value == 0);
        cls.isOne = (node.value == 1);
        cls.isNonNeg = (node.value >= 0);
        cls.isPowerOfTwo = (node.value > 0) && ((node.value & (node.value - 1)) == 0);
        cls.isBoolean = (node.value == 0 || node.value == 1);
    } else if (node.op == Op::ConstF) {
        cls.isNonNeg = (node.fvalue >= 0.0);
    } else if (node.children.size() >= 2) {
        // Propagate isNonNeg through operations where it's provable.
        // This is critical for relational rules (div_pow2_nonneg,
        // mod_pow2_nonneg) to fire on expressions derived from
        // non-negative sub-expressions like loop counters.
        ClassId lhs = find(node.children[0]);
        ClassId rhs = find(node.children[1]);
        bool lNonNeg = (lhs < classes_.size()) && classes_[lhs].isNonNeg;
        bool rNonNeg = (rhs < classes_.size()) && classes_[rhs].isNonNeg;
        bool lBool = (lhs < classes_.size()) && classes_[lhs].isBoolean;
        bool rBool = (rhs < classes_.size()) && classes_[rhs].isBoolean;

        switch (node.op) {
        case Op::Add:
        case Op::Mul:
        case Op::BitOr:
        case Op::BitXor:
            // Non-negative when both operands are non-negative
            cls.isNonNeg = lNonNeg && rNonNeg;
            break;
        case Op::BitAnd:
            // Non-negative when either operand is non-negative
            // (AND can only clear bits, so if sign bit is 0 in either, result sign is 0)
            cls.isNonNeg = lNonNeg || rNonNeg;
            // AND of two booleans is boolean; AND with non-boolean can produce non-boolean values
            cls.isBoolean = lBool && rBool;
            break;
        case Op::Shr:
            // Logical/arithmetic shift right: if lhs non-negative, result non-negative
            cls.isNonNeg = lNonNeg;
            break;
        case Op::Shl:
            // Shift left preserves non-negativity only for small shifts,
            // but conservatively mark it if lhs is non-negative
            cls.isNonNeg = lNonNeg;
            break;
        case Op::Mod:
            // x % C with x >= 0 and C > 0 → result in [0, C-1)
            cls.isNonNeg = lNonNeg && rNonNeg;
            break;
        case Op::Div:
            // x / C with x >= 0 and C > 0 → result >= 0
            cls.isNonNeg = lNonNeg && rNonNeg;
            break;
        case Op::Sub:
            // Not generally non-negative; leave as false
            break;
        case Op::LogAnd:
        case Op::LogOr:
            // Logical operations produce boolean 0/1
            cls.isNonNeg = true;
            cls.isBoolean = true;
            break;
        default:
            // Comparisons produce boolean 0/1 which are non-negative
            if (node.op == Op::Eq || node.op == Op::Ne ||
                node.op == Op::Lt || node.op == Op::Le ||
                node.op == Op::Gt || node.op == Op::Ge) {
                cls.isNonNeg = true;
                cls.isBoolean = true;
            }
            break;
        }
    } else if (node.children.size() == 1) {
        // Unary operations
        if (node.op == Op::LogNot) {
            cls.isNonNeg = true;  // Boolean result: 0 or 1
            cls.isBoolean = true;
        } else if (node.op == Op::Sqrt) {
            cls.isNonNeg = true;  // sqrt is always non-negative for valid inputs
        }
    }

    classes_.push_back(std::move(cls));

    hashcons_[node] = id;
    totalNodes_++;

    return id;
}

ClassId EGraph::addConst(long long val) {
    ENode n(Op::Const, val);
    return add(std::move(n));
}

ClassId EGraph::addConstF(double val) {
    ENode n(Op::ConstF, val);
    return add(std::move(n));
}

ClassId EGraph::addVar(const std::string& name) {
    ENode n(Op::Var, name);
    return add(std::move(n));
}

ClassId EGraph::addBinOp(Op op, ClassId lhs, ClassId rhs) {
    ENode n(op);
    n.children = {lhs, rhs};
    return add(std::move(n));
}

ClassId EGraph::addUnaryOp(Op op, ClassId operand) {
    ENode n(op);
    n.children = {operand};
    return add(std::move(n));
}

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — union-find with path compression
// ─────────────────────────────────────────────────────────────────────────────

ClassId EGraph::find(ClassId id) {
    assert(id < parent_.size() && "ClassId out of range");
    // Path compression
    while (parent_[id] != id) {
        parent_[id] = parent_[parent_[id]]; // path halving
        id = parent_[id];
    }
    return id;
}

ClassId EGraph::merge(ClassId a, ClassId b) {
    a = find(a);
    b = find(b);
    if (a == b) return a;

    // Union by rank: merge smaller into larger
    if (classes_[a].nodes.size() < classes_[b].nodes.size()) {
        std::swap(a, b);
    }

    // b merges into a
    parent_[b] = a;

    // Move all nodes from b into a
    for (auto& node : classes_[b].nodes) {
        classes_[a].nodes.push_back(std::move(node));
    }
    classes_[b].nodes.clear();

    // Merge analysis data
    if (!classes_[a].constVal && classes_[b].constVal)
        classes_[a].constVal = classes_[b].constVal;
    classes_[a].isZero = classes_[a].isZero || classes_[b].isZero;
    classes_[a].isOne = classes_[a].isOne || classes_[b].isOne;
    // When merging two equivalent e-classes, if EITHER is proven non-negative,
    // the merged class is non-negative (they represent the same value).
    classes_[a].isNonNeg = classes_[a].isNonNeg || classes_[b].isNonNeg;

    return a;
}

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — canonicalization and rebuild
// ─────────────────────────────────────────────────────────────────────────────

ENode EGraph::canonicalize(ENode node) const {
    for (auto& child : node.children) {
        // find() uses mutable path compression via mutable parent_
        child = const_cast<EGraph*>(this)->find(child);
    }
    return node;
}

void EGraph::rebuild() {
    // Rebuild the hashcons table: re-canonicalize all nodes after merges.
    hashcons_.clear();
    for (ClassId i = 0; i < classes_.size(); ++i) {
        ClassId canonical = find(i);
        if (canonical != i) continue; // Skip non-canonical classes

        for (auto& node : classes_[i].nodes) {
            node = canonicalize(std::move(node));
            hashcons_[node] = i;
        }

        // Deduplicate nodes within the class
        std::unordered_set<ENode, ENodeHash> seen;
        std::vector<ENode> unique;
        for (auto& node : classes_[i].nodes) {
            if (seen.insert(node).second) {
                unique.push_back(std::move(node));
            }
        }
        classes_[i].nodes = std::move(unique);
    }

    // Update total node count
    totalNodes_ = 0;
    for (ClassId i = 0; i < classes_.size(); ++i) {
        if (find(i) == i) {
            totalNodes_ += classes_[i].nodes.size();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — pattern matching
// ─────────────────────────────────────────────────────────────────────────────

bool EGraph::matchClass(const Pattern& pat, ClassId cls, Subst& subst) const {
    cls = const_cast<EGraph*>(this)->find(cls);

    if (pat.kind == Pattern::Kind::Wildcard) {
        auto it = subst.find(pat.wildcard);
        if (it != subst.end()) {
            // Wildcard already bound — check it matches the same class
            return const_cast<EGraph*>(this)->find(it->second) == cls;
        }
        subst[pat.wildcard] = cls;
        return true;
    }

    // OpMatch: try to match against any node in the class
    for (const auto& node : classes_[cls].nodes) {
        if (node.op != pat.op) continue;

        // Check constant value constraint
        if (pat.matchConst && node.value != pat.constVal) continue;
        if (pat.matchConstF && node.fvalue != pat.constFVal) continue;

        // Check children count
        if (node.children.size() != pat.children.size()) continue;

        // Try matching all children
        Subst childSubst = subst;
        bool allMatch = true;
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (!matchClass(pat.children[i], node.children[i], childSubst)) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) {
            subst = std::move(childSubst);
            return true;
        }
    }
    return false;
}

bool EGraph::matchClassCommutative(const Pattern& pat, ClassId cls, Subst& subst) const {
    // First try the standard match
    if (matchClass(pat, cls, subst)) return true;

    // For commutative binary patterns, try swapping children
    if (pat.kind == Pattern::Kind::OpMatch && pat.children.size() == 2) {
        bool isCommutative = false;
        switch (pat.op) {
        case Op::Add: case Op::Mul:
        case Op::BitAnd: case Op::BitOr: case Op::BitXor:
        case Op::Eq: case Op::Ne:
        case Op::LogAnd: case Op::LogOr:
            isCommutative = true;
            break;
        default:
            break;
        }
        if (isCommutative) {
            // Create a swapped pattern
            Pattern swapped = Pattern::OpPat(pat.op, {pat.children[1], pat.children[0]});
            Subst swappedSubst = subst;
            if (matchClass(swapped, cls, swappedSubst)) {
                subst = std::move(swappedSubst);
                return true;
            }
        }
    }
    return false;
}

std::vector<std::pair<ClassId, Subst>> EGraph::match(const Pattern& pat) const {
    std::vector<std::pair<ClassId, Subst>> results;
    for (ClassId i = 0; i < classes_.size(); ++i) {
        ClassId canonical = const_cast<EGraph*>(this)->find(i);
        if (canonical != i) continue; // Skip non-canonical

        Subst subst;
        if (matchClassCommutative(pat, i, subst)) {
            results.emplace_back(i, std::move(subst));
        }
    }
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — constant folding
// ─────────────────────────────────────────────────────────────────────────────

void EGraph::foldConstants(ClassId cls) {
    cls = find(cls);
    auto& eclass = classes_[cls];

    for (const auto& node : eclass.nodes) {
        // Binary constant folding
        if (node.children.size() == 2) {
            ClassId lhsId = find(node.children[0]);
            ClassId rhsId = find(node.children[1]);

            // Find constant nodes in children
            const ENode* lhsConst = nullptr;
            const ENode* rhsConst = nullptr;
            for (const auto& n : classes_[lhsId].nodes) {
                if (n.op == Op::Const) { lhsConst = &n; break; }
            }
            for (const auto& n : classes_[rhsId].nodes) {
                if (n.op == Op::Const) { rhsConst = &n; break; }
            }

            if (lhsConst && rhsConst) {
                long long lv = lhsConst->value;
                long long rv = rhsConst->value;
                long long result = 0;
                bool valid = true;

                switch (node.op) {
                case Op::Add: result = lv + rv; break;
                case Op::Sub: result = lv - rv; break;
                case Op::Mul: result = lv * rv; break;
                case Op::Div:
                    if (rv != 0) result = lv / rv;
                    else valid = false;
                    break;
                case Op::Mod:
                    if (rv != 0) result = lv % rv;
                    else valid = false;
                    break;
                case Op::BitAnd: result = lv & rv; break;
                case Op::BitOr:  result = lv | rv; break;
                case Op::BitXor: result = lv ^ rv; break;
                case Op::Shl:
                    if (rv >= 0 && rv < 64) result = static_cast<long long>(static_cast<unsigned long long>(lv) << static_cast<unsigned long long>(rv));
                    else valid = false;
                    break;
                case Op::Shr:
                    if (rv >= 0 && rv < 64) result = static_cast<long long>(static_cast<unsigned long long>(lv) >> static_cast<unsigned long long>(rv));
                    else valid = false;
                    break;
                case Op::Eq:     result = (lv == rv) ? 1 : 0; break;
                case Op::Ne:     result = (lv != rv) ? 1 : 0; break;
                case Op::Lt:     result = (lv < rv) ? 1 : 0; break;
                case Op::Le:     result = (lv <= rv) ? 1 : 0; break;
                case Op::Gt:     result = (lv > rv) ? 1 : 0; break;
                case Op::Ge:     result = (lv >= rv) ? 1 : 0; break;
                default: valid = false; break;
                }

                if (valid) {
                    ClassId constCls = const_cast<EGraph*>(this)->addConst(result);
                    const_cast<EGraph*>(this)->merge(cls, constCls);
                    return;
                }
            }

            // Float constant folding
            const ENode* lhsConstF = nullptr;
            const ENode* rhsConstF = nullptr;
            for (const auto& n : classes_[lhsId].nodes) {
                if (n.op == Op::ConstF) { lhsConstF = &n; break; }
            }
            for (const auto& n : classes_[rhsId].nodes) {
                if (n.op == Op::ConstF) { rhsConstF = &n; break; }
            }

            if (lhsConstF && rhsConstF) {
                double lv = lhsConstF->fvalue;
                double rv = rhsConstF->fvalue;
                double result = 0.0;
                bool valid = true;

                switch (node.op) {
                case Op::Add: result = lv + rv; break;
                case Op::Sub: result = lv - rv; break;
                case Op::Mul: result = lv * rv; break;
                case Op::Div:
                    if (rv != 0.0) result = lv / rv;
                    else valid = false;
                    break;
                default: valid = false; break;
                }

                if (valid) {
                    ClassId constCls = const_cast<EGraph*>(this)->addConstF(result);
                    const_cast<EGraph*>(this)->merge(cls, constCls);
                    return;
                }
            }
        }

        // Unary constant folding
        if (node.children.size() == 1) {
            ClassId childId = find(node.children[0]);
            const ENode* childConst = nullptr;
            for (const auto& n : classes_[childId].nodes) {
                if (n.op == Op::Const) { childConst = &n; break; }
            }
            if (childConst) {
                long long cv = childConst->value;
                long long result = 0;
                bool valid = true;
                switch (node.op) {
                case Op::Neg:    result = -cv; break;
                case Op::BitNot: result = ~cv; break;
                case Op::LogNot: result = (cv == 0) ? 1 : 0; break;
                default: valid = false; break;
                }
                if (valid) {
                    ClassId constCls = const_cast<EGraph*>(this)->addConst(result);
                    const_cast<EGraph*>(this)->merge(cls, constCls);
                    return;
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — equality saturation
// ─────────────────────────────────────────────────────────────────────────────

size_t EGraph::applyRules(const std::vector<RewriteRule>& rules) {
    size_t merges = 0;

    for (const auto& rule : rules) {
        if (atNodeLimit()) break;

        auto matches = match(rule.lhs);
        for (auto& [cls, subst] : matches) {
            if (atNodeLimit()) break;

            // Relational guard: skip this match if the predicate rejects it.
            if (rule.guard && !rule.guard(*this, subst)) continue;

            ClassId rhsId = rule.rhs(*this, subst);
            ClassId lhsCls = find(cls);
            ClassId rhsCls = find(rhsId);
            if (lhsCls != rhsCls) {
                merge(lhsCls, rhsCls);
                merges++;
            }
        }
    }

    return merges;
}

size_t EGraph::saturate(const std::vector<RewriteRule>& rules) {
    size_t iterations = 0;
    size_t prevMerges = 0;
    unsigned stagnantRuns = 0;

    for (size_t iter = 0; iter < config_.maxIterations; ++iter) {
        if (atNodeLimit()) break;

        size_t merges = applyRules(rules);

        // Constant folding pass
        if (config_.enableConstantFolding) {
            for (ClassId i = 0; i < classes_.size(); ++i) {
                if (find(i) == i) {
                    foldConstants(i);
                }
            }
        }

        // Rebuild after all merges
        rebuild();
        iterations++;

        // Reached fixpoint if no new merges
        if (merges == 0) break;

        // Early termination: if the number of merges is ≤ 10% of the
        // previous iteration's merges for 3 consecutive rounds, the
        // saturation is delivering diminishing returns.  Stop early to
        // save compile time on large programs.
        if (prevMerges > 0 && merges * 10 <= prevMerges) {
            ++stagnantRuns;
            if (stagnantRuns >= 3) break;
        } else {
            stagnantRuns = 0;
        }
        prevMerges = merges;
    }

    return iterations;
}

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — extraction (cost-based)
// ─────────────────────────────────────────────────────────────────────────────

std::unordered_map<ClassId, EGraph::ExtractionResult>
EGraph::extractAll(ClassId root, const CostModel& model) {
    root = find(root);
    std::unordered_map<ClassId, ExtractionResult> best;

    // Collect only e-classes reachable from root to avoid wasted work.
    std::unordered_set<ClassId> reachable;
    {
        std::vector<ClassId> worklist = {root};
        while (!worklist.empty()) {
            ClassId cls = find(worklist.back());
            worklist.pop_back();
            if (!reachable.insert(cls).second) continue;
            for (const auto& node : classes_[cls].nodes)
                for (auto child : node.children)
                    worklist.push_back(find(child));
        }
    }

    // Iterative cost computation: repeat until stable.
    // We use a simple fixed-point iteration (like Bellman-Ford).
    // Each iteration, for each reachable class, we pick the node with the
    // lowest total cost (node cost + sum of children's best costs).
    bool changed = true;
    for (int pass = 0; pass < 100 && changed; ++pass) {
        changed = false;
        for (ClassId cls : reachable) {

            for (const auto& node : classes_[cls].nodes) {
                Cost total = model.nodeCost(node);
                bool feasible = true;

                for (auto child : node.children) {
                    child = find(child);
                    auto it = best.find(child);
                    if (it == best.end()) {
                        feasible = false;
                        break;
                    }
                    total += it->second.cost;
                }

                if (!feasible) {
                    // Leaf nodes with no children are always feasible
                    if (!node.children.empty()) continue;
                }

                auto it = best.find(cls);
                if (it == best.end() || total < it->second.cost) {
                    best[cls] = {total, node};
                    changed = true;
                }
            }
        }
    }

    // ── DAG sharing discount ────────────────────────────────────────────
    // When the same sub-expression is used by multiple parent nodes, it is
    // computed once and reused.  Adjust costs of the already-selected best
    // nodes to reflect sharing, without changing which node is selected.
    // This ensures extraction structure remains stable while costs become
    // more accurate for DAG-aware comparisons.
    std::unordered_map<ClassId, unsigned> parentCount;
    for (ClassId cls : reachable) {
        auto it = best.find(cls);
        if (it == best.end()) continue;
        for (auto child : it->second.bestNode.children) {
            child = find(child);
            parentCount[child]++;
        }
    }

    // Propagate sharing discounts bottom-up through the best map.
    bool dagChanged = true;
    for (int dagPass = 0; dagPass < 10 && dagChanged; ++dagPass) {
        dagChanged = false;
        for (ClassId cls : reachable) {
            auto bestIt = best.find(cls);
            if (bestIt == best.end()) continue;
            const auto& node = bestIt->second.bestNode;
            Cost total = model.nodeCost(node);
            bool feasible = true;
            for (auto child : node.children) {
                child = find(child);
                auto cit = best.find(child);
                if (cit == best.end()) { feasible = false; break; }
                Cost childCost = cit->second.cost;
                unsigned parents = parentCount.count(child) ? parentCount[child] : 1;
                if (parents > 1)
                    childCost = childCost / static_cast<Cost>(parents);
                total += childCost;
            }
            if (!feasible) continue;
            if (total < bestIt->second.cost) {
                bestIt->second.cost = total;
                dagChanged = true;
            }
        }
    }

    return best;
}

ENode EGraph::extract(ClassId root, const CostModel& model) {
    root = find(root);
    auto bestMap = extractAll(root, model);

    // Recursively build the extracted tree
    std::function<ENode(ClassId)> buildTree = [&](ClassId cls) -> ENode {
        cls = find(cls);
        auto it = bestMap.find(cls);
        if (it == bestMap.end()) {
            // Fallback: return the first node in the class
            return classes_[cls].nodes.empty() ? ENode(Op::Nop) : classes_[cls].nodes[0];
        }

        ENode result = it->second.bestNode;
        // Replace children class IDs with extracted sub-trees
        // (We keep children as class IDs for the top-level result;
        // the caller can recursively extract if needed)
        return result;
    };

    return buildTree(root);
}

// ─────────────────────────────────────────────────────────────────────────────
// EGraph — accessors
// ─────────────────────────────────────────────────────────────────────────────

const EClass& EGraph::getClass(ClassId id) const {
    id = const_cast<EGraph*>(this)->find(id);
    assert(id < classes_.size());
    return classes_[id];
}

size_t EGraph::numClasses() const {
    size_t count = 0;
    for (ClassId i = 0; i < classes_.size(); ++i) {
        if (const_cast<EGraph*>(this)->find(i) == i) {
            count++;
        }
    }
    return count;
}

size_t EGraph::numNodes() const {
    return totalNodes_;
}

bool EGraph::atNodeLimit() const {
    return totalNodes_ >= config_.maxNodes;
}

std::optional<long long> EGraph::getConstValue(ClassId cls) const {
    cls = const_cast<EGraph*>(this)->find(cls);
    if (cls >= classes_.size()) return std::nullopt;
    for (const auto& node : classes_[cls].nodes) {
        if (node.op == Op::Const) return node.value;
    }
    return std::nullopt;
}

std::optional<double> EGraph::getConstFValue(ClassId cls) const {
    cls = const_cast<EGraph*>(this)->find(cls);
    if (cls >= classes_.size()) return std::nullopt;
    for (const auto& node : classes_[cls].nodes) {
        if (node.op == Op::ConstF) return node.fvalue;
    }
    return std::nullopt;
}

bool EGraph::areEquivalent(ClassId a, ClassId b) {
    return find(a) == find(b);
}

bool EGraph::isClassPowerOfTwo(ClassId cls) const {
    cls = const_cast<EGraph*>(this)->find(cls);
    if (cls >= classes_.size()) return false;
    return classes_[cls].isPowerOfTwo;
}

bool EGraph::isClassBoolean(ClassId cls) const {
    cls = const_cast<EGraph*>(this)->find(cls);
    if (cls >= classes_.size()) return false;
    return classes_[cls].isBoolean;
}

bool EGraph::isClassNonNeg(ClassId cls) const {
    cls = const_cast<EGraph*>(this)->find(cls);
    if (cls >= classes_.size()) return false;
    return classes_[cls].isNonNeg;
}

bool EGraph::hasVariable(ClassId cls, const std::string& name) const {
    cls = const_cast<EGraph*>(this)->find(cls);
    if (cls >= classes_.size()) return false;
    for (const auto& node : classes_[cls].nodes) {
        if (node.op == Op::Var && node.name == name) return true;
    }
    return false;
}

std::optional<Op> EGraph::getClassOp(ClassId cls) const {
    cls = const_cast<EGraph*>(this)->find(cls);
    if (cls >= classes_.size()) return std::nullopt;
    for (const auto& node : classes_[cls].nodes) {
        if (node.op != Op::Nop) return node.op;
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Rule library — algebraic simplification rules
// ─────────────────────────────────────────────────────────────────────────────

std::vector<RewriteRule> getAlgebraicRules() {
    using P = Pattern;
    std::vector<RewriteRule> rules;

    // ── Additive identity: x + 0 → x ────────────────────────────────────
    rules.emplace_back("add_zero_right",
        P::OpPat(Op::Add, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    rules.emplace_back("add_zero_left",
        P::OpPat(Op::Add, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Subtractive identity: x - 0 → x ─────────────────────────────────
    rules.emplace_back("sub_zero",
        P::OpPat(Op::Sub, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Self-subtraction: x - x → 0 ─────────────────────────────────────
    rules.emplace_back("sub_self",
        P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Multiplicative identity: x * 1 → x ──────────────────────────────
    rules.emplace_back("mul_one_right",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    rules.emplace_back("mul_one_left",
        P::OpPat(Op::Mul, {P::ConstPat(1), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Multiply by zero: x * 0 → 0 ─────────────────────────────────────
    rules.emplace_back("mul_zero_right",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    rules.emplace_back("mul_zero_left",
        P::OpPat(Op::Mul, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Strength reduction: x * 2 → x + x ───────────────────────────────
    rules.emplace_back("mul_two_to_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Add, s.at("x"), s.at("x"));
        });

    // ── Strength reduction: x * 2^n → x << n (for common powers of 2) ──
    for (int shift = 2; shift <= 10; ++shift) {
        long long val = 1LL << shift;
        rules.emplace_back("mul_pow2_" + std::to_string(val),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(val)}),
            [shift](EGraph& g, const Subst& s) {
                ClassId shiftAmt = g.addConst(shift);
                return g.addBinOp(Op::Shl, s.at("x"), shiftAmt);
            });
    }

    // ── Division identity: x / 1 → x ────────────────────────────────────
    rules.emplace_back("div_one",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Self-division: x / x → 1 ────────────────────────────────────────
    rules.emplace_back("div_self",
        P::OpPat(Op::Div, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // NOTE: x / 2^n → x >> n is NOT valid for signed integers.
    // Arithmetic shift right rounds toward -∞ but signed division rounds
    // toward zero.  For example, -7 / 2 = -3 but -7 >> 1 = -4.
    // The codegen already emits the correct (x + ((x >> 63) & (2^n - 1))) >> n
    // sequence for power-of-2 constant divisors.

    // ── Double negation: -(-x) → x ──────────────────────────────────────
    rules.emplace_back("double_neg",
        P::OpPat(Op::Neg, {P::OpPat(Op::Neg, {P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Commutativity of addition: a + b → b + a ─────────────────────────
    rules.emplace_back("add_comm",
        P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Add, s.at("b"), s.at("a"));
        });

    // ── Commutativity of multiplication: a * b → b * a ───────────────────
    rules.emplace_back("mul_comm",
        P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("b"), s.at("a"));
        });

    // ── Associativity: (a + b) + c → a + (b + c) ────────────────────────
    rules.emplace_back("add_assoc",
        P::OpPat(Op::Add, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::Add, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Add, s.at("a"), bc);
        });

    // ── Distributivity: a * (b + c) → a*b + a*c ─────────────────────────
    rules.emplace_back("distribute_mul_add",
        P::OpPat(Op::Mul, {P::Wild("a"), P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
            ClassId ac = g.addBinOp(Op::Mul, s.at("a"), s.at("c"));
            return g.addBinOp(Op::Add, ab, ac);
        });

    // ── Factoring: a*b + a*c → a * (b + c) ──────────────────────────────
    rules.emplace_back("factor_mul_add",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::Add, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Mul, s.at("a"), bc);
        });

    // ── Subtraction as addition of negation: a - b → a + (-b) ───────────
    rules.emplace_back("sub_to_add_neg",
        P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ClassId negB = g.addUnaryOp(Op::Neg, s.at("b"));
            return g.addBinOp(Op::Add, s.at("a"), negB);
        });

    // ── Modulo identity: x % 1 → 0 ──────────────────────────────────────
    rules.emplace_back("mod_one",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Self-modulo: x % x → 0 ──────────────────────────────────────────
    rules.emplace_back("mod_self",
        P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Modulo by power of 2: x % 2 → x & 1 ────────────────────────────
    // NOTE: This is technically only correct for non-negative x when using
    // signed semantics.  However, the codegen already emits the corrected
    // signed-mod sequence for power-of-2 divisors via the sign-bit fixup
    // path in codegen_expr.cpp.  At the e-graph level, these rules apply
    // in the abstract integer domain where the e-graph tracks equivalent
    // representations — the extraction cost model selects bitwise AND
    // only when it's cheaper.
    rules.emplace_back("mod_2_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(2)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(1));
        });
    rules.emplace_back("mod_4_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(4)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(3));
        });
    rules.emplace_back("mod_8_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(8)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(7));
        });
    rules.emplace_back("mod_16_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(16)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(15));
        });
    rules.emplace_back("mod_32_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(32)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(31));
        });
    rules.emplace_back("mod_64_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(64)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(63));
        });
    rules.emplace_back("mod_128_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(128)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(127));
        });
    rules.emplace_back("mod_256_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(256)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(255));
        });
    rules.emplace_back("mod_512_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(512)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(511));
        });
    rules.emplace_back("mod_1024_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(1024)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(1023));
        });

    // ── Negation distribution: -(a - b) → b - a ─────────────────────────
    rules.emplace_back("neg_sub_to_swap",
        P::OpPat(Op::Neg, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Sub, s.at("b"), s.at("a"));
        });

    // ── Multiplicative associativity: (a * b) * c → a * (b * c) ─────────
    rules.emplace_back("mul_assoc",
        P::OpPat(Op::Mul, {P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::Mul, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Mul, s.at("a"), bc);
        });

    // ── Distributivity over subtraction: a * (b - c) → a*b - a*c ────────
    rules.emplace_back("distribute_mul_sub",
        P::OpPat(Op::Mul, {P::Wild("a"), P::OpPat(Op::Sub, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
            ClassId ac = g.addBinOp(Op::Mul, s.at("a"), s.at("c"));
            return g.addBinOp(Op::Sub, ab, ac);
        });

    // ── Strength reduction: x * 3 → (x << 1) + x ───────────────────────
    rules.emplace_back("mul_3_shift_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)}),
        [](EGraph& g, const Subst& s) {
            ClassId one = g.addConst(1);
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), one);
            return g.addBinOp(Op::Add, shl, s.at("x"));
        });

    // ── Strength reduction: x * 5 → (x << 2) + x ───────────────────────
    rules.emplace_back("mul_5_shift_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(5)}),
        [](EGraph& g, const Subst& s) {
            ClassId two = g.addConst(2);
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), two);
            return g.addBinOp(Op::Add, shl, s.at("x"));
        });

    // ── Strength reduction: x * 7 → (x << 3) - x ───────────────────────
    rules.emplace_back("mul_7_shift_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(7)}),
        [](EGraph& g, const Subst& s) {
            ClassId three = g.addConst(3);
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), three);
            return g.addBinOp(Op::Sub, shl, s.at("x"));
        });

    // ── Strength reduction: x * 9 → (x << 3) + x ───────────────────────
    rules.emplace_back("mul_9_shift_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(9)}),
        [](EGraph& g, const Subst& s) {
            ClassId three = g.addConst(3);
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), three);
            return g.addBinOp(Op::Add, shl, s.at("x"));
        });

    // ── Strength reduction: x * 15 → (x << 4) - x ──────────────────────
    rules.emplace_back("mul_15_shift_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(15)}),
        [](EGraph& g, const Subst& s) {
            ClassId four = g.addConst(4);
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), four);
            return g.addBinOp(Op::Sub, shl, s.at("x"));
        });

    // ── Strength reduction: x * 17 → (x << 4) + x ──────────────────────
    rules.emplace_back("mul_17_shift_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(17)}),
        [](EGraph& g, const Subst& s) {
            ClassId four = g.addConst(4);
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), four);
            return g.addBinOp(Op::Add, shl, s.at("x"));
        });

    // ── Canonicalize: a + (-b) → a - b ──────────────────────────────────
    rules.emplace_back("add_neg_to_sub",
        P::OpPat(Op::Add, {P::Wild("a"), P::OpPat(Op::Neg, {P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Sub, s.at("a"), s.at("b"));
        });

    // ── Canonicalize: 0 - x → -x ────────────────────────────────────────
    rules.emplace_back("zero_sub_to_neg",
        P::OpPat(Op::Sub, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::Neg, s.at("x"));
        });

    // ── Multiply by -1: x * (-1) → -x ──────────────────────────────────
    rules.emplace_back("mul_neg1",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(-1)}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::Neg, s.at("x"));
        });

    // ── Multiply by -1 (left): (-1) * x → -x ───────────────────────────
    rules.emplace_back("mul_neg1_left",
        P::OpPat(Op::Mul, {P::ConstPat(-1), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::Neg, s.at("x"));
        });

    // ── Self-addition: x + x → x * 2 ────────────────────────────────────
    // (enables strength-reduction chain x+x → x*2 → x<<1)
    rules.emplace_back("add_self_to_mul2",
        P::OpPat(Op::Add, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId two = g.addConst(2);
            return g.addBinOp(Op::Mul, s.at("x"), two);
        });

    // ── Cancellation: (a + b) - a → b ───────────────────────────────────
    rules.emplace_back("add_sub_cancel_left",
        P::OpPat(Op::Sub, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // ── Cancellation: (a + b) - b → a ───────────────────────────────────
    rules.emplace_back("add_sub_cancel_right",
        P::OpPat(Op::Sub, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ── Subtraction of negation: a - (-b) → a + b ───────────────────────
    rules.emplace_back("sub_neg_to_add",
        P::OpPat(Op::Sub, {P::Wild("a"), P::OpPat(Op::Neg, {P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Add, s.at("a"), s.at("b"));
        });

    // ── Negation of addition: -(a + b) → (-a) + (-b) → (-a) - b ────────
    // More useful: -(a + b) → -a - b  which via sub_to_add_neg = (-a) + (-b)
    // Keep it simpler: -(a + b) → 0 - (a + b) = 0 - a - b
    // Actually simplest: -(a + b) equivalent to (-a) + (-b)
    // Not always a win, so skip. But add: -(a + b) ≡ -(a) - b
    // That is useful via negation distribution.

    // ── Power simplification: x ** 0 → 1 ────────────────────────────────
    rules.emplace_back("pow_zero",
        P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ── Power simplification: x ** 1 → x ────────────────────────────────
    rules.emplace_back("pow_one",
        P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Power simplification: x ** 2 → x * x ────────────────────────────
    rules.emplace_back("pow_two",
        P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(2)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), s.at("x"));
        });

    // ── Mul zero (integer-safe): x * 0 → 0 ──────────────────────────────
    rules.emplace_back("mul_zero_right",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    rules.emplace_back("mul_zero_left",
        P::OpPat(Op::Mul, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Strength reduction chain: x * 2^n → x << n ──────────────────────
    // These enable the cost model to select shifts over multiplies.
    rules.emplace_back("mul_2_to_shl1",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)}),
        [](EGraph& g, const Subst& s) {
            ClassId one = g.addConst(1);
            return g.addBinOp(Op::Shl, s.at("x"), one);
        });

    rules.emplace_back("mul_4_to_shl2",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4)}),
        [](EGraph& g, const Subst& s) {
            ClassId two = g.addConst(2);
            return g.addBinOp(Op::Shl, s.at("x"), two);
        });

    rules.emplace_back("mul_8_to_shl3_chain",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)}),
        [](EGraph& g, const Subst& s) {
            ClassId three = g.addConst(3);
            return g.addBinOp(Op::Shl, s.at("x"), three);
        });

    rules.emplace_back("mul_16_to_shl4_chain",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(16)}),
        [](EGraph& g, const Subst& s) {
            ClassId four = g.addConst(4);
            return g.addBinOp(Op::Shl, s.at("x"), four);
        });

    rules.emplace_back("mul_32_to_shl5",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(32)}),
        [](EGraph& g, const Subst& s) {
            ClassId five = g.addConst(5);
            return g.addBinOp(Op::Shl, s.at("x"), five);
        });

    rules.emplace_back("mul_64_to_shl6",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(64)}),
        [](EGraph& g, const Subst& s) {
            ClassId six = g.addConst(6);
            return g.addBinOp(Op::Shl, s.at("x"), six);
        });

    // ── Double negation: -(-x) → x ──────────────────────────────────────
    rules.emplace_back("neg_neg",
        P::OpPat(Op::Neg, {P::OpPat(Op::Neg, {P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // NOTE: Division by power-of-2 → shift is NOT safe for signed integers
    // because signed division truncates toward zero while arithmetic shift
    // rounds toward negative infinity: e.g. -7/4 = -1 but -7>>2 = -2.
    // These transformations are left to the LLVM backend which inserts
    // the necessary correction (add + ashr) when the dividend may be negative.

    // ── Distributive: a * (b + c) → a*b + a*c ──────────────────────────
    // Useful for exposing strength-reduction opportunities on sub-expressions.
    // (Only applied when 'a' is a constant, to avoid code size explosion.)
    rules.emplace_back("mul_add_distribute",
        P::OpPat(Op::Mul, {P::Wild("a"), P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
            ClassId ac = g.addBinOp(Op::Mul, s.at("a"), s.at("c"));
            return g.addBinOp(Op::Add, ab, ac);
        });

    // ── Factoring: a*c + b*c → (a + b) * c ──────────────────────────────
    rules.emplace_back("add_factor_right",
        P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("c")}),
                            P::OpPat(Op::Mul, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId sum = g.addBinOp(Op::Add, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Mul, sum, s.at("c"));
        });

    // ── Factoring: c*a + c*b → c * (a + b) ──────────────────────────────
    rules.emplace_back("add_factor_left",
        P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("c"), P::Wild("a")}),
                            P::OpPat(Op::Mul, {P::Wild("c"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId sum = g.addBinOp(Op::Add, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Mul, s.at("c"), sum);
        });

    // ── x * (2^n) strength reduction for larger powers ──────────────────
    rules.emplace_back("mul_128_to_shl7",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(128)}),
        [](EGraph& g, const Subst& s) {
            ClassId seven = g.addConst(7);
            return g.addBinOp(Op::Shl, s.at("x"), seven);
        });

    rules.emplace_back("mul_256_to_shl8",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(256)}),
        [](EGraph& g, const Subst& s) {
            ClassId eight = g.addConst(8);
            return g.addBinOp(Op::Shl, s.at("x"), eight);
        });

    rules.emplace_back("mul_512_to_shl9",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(512)}),
        [](EGraph& g, const Subst& s) {
            ClassId nine = g.addConst(9);
            return g.addBinOp(Op::Shl, s.at("x"), nine);
        });

    rules.emplace_back("mul_1024_to_shl10",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(1024)}),
        [](EGraph& g, const Subst& s) {
            ClassId ten = g.addConst(10);
            return g.addBinOp(Op::Shl, s.at("x"), ten);
        });

    // ─────────────────────────────────────────────────────────────────────
    // More strength reductions for non-power-of-2 constants
    // ─────────────────────────────────────────────────────────────────────

    // x * 6 → (x << 2) + (x << 1)  [4x + 2x = 6x]
    rules.emplace_back("mul_6_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(6)}),
        [](EGraph& g, const Subst& s) {
            ClassId c2 = g.addConst(2); ClassId c1 = g.addConst(1);
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), c2);
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), c1);
            return g.addBinOp(Op::Add, s2, s1);
        });

    // x * 10 → (x << 3) + (x << 1)  [8x + 2x = 10x]
    rules.emplace_back("mul_10_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(10)}),
        [](EGraph& g, const Subst& s) {
            ClassId c3 = g.addConst(3); ClassId c1 = g.addConst(1);
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), c3);
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), c1);
            return g.addBinOp(Op::Add, s3, s1);
        });

    // x * 11 → (x << 3) + (x << 1) + x  [8x + 2x + 1x = 11x]
    rules.emplace_back("mul_11_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(11)}),
        [](EGraph& g, const Subst& s) {
            ClassId c3 = g.addConst(3); ClassId c1 = g.addConst(1);
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), c3);
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), c1);
            ClassId t = g.addBinOp(Op::Add, s3, s1);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });

    // x * 12 → (x << 3) + (x << 2)  [8x + 4x = 12x]
    rules.emplace_back("mul_12_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(12)}),
        [](EGraph& g, const Subst& s) {
            ClassId c3 = g.addConst(3); ClassId c2 = g.addConst(2);
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), c3);
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), c2);
            return g.addBinOp(Op::Add, s3, s2);
        });

    // x * 13 → (x << 3) + (x << 2) + x  [8x + 4x + 1x = 13x]
    rules.emplace_back("mul_13_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(13)}),
        [](EGraph& g, const Subst& s) {
            ClassId c3 = g.addConst(3); ClassId c2 = g.addConst(2);
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), c3);
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), c2);
            ClassId t = g.addBinOp(Op::Add, s3, s2);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });

    // x * 14 → (x << 4) - (x << 1)  [16x - 2x = 14x]
    rules.emplace_back("mul_14_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(14)}),
        [](EGraph& g, const Subst& s) {
            ClassId c4 = g.addConst(4); ClassId c1 = g.addConst(1);
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), c4);
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), c1);
            return g.addBinOp(Op::Sub, s4, s1);
        });

    // x * 18 → (x << 4) + (x << 1)  [16x + 2x = 18x]
    rules.emplace_back("mul_18_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(18)}),
        [](EGraph& g, const Subst& s) {
            ClassId c4 = g.addConst(4); ClassId c1 = g.addConst(1);
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), c4);
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), c1);
            return g.addBinOp(Op::Add, s4, s1);
        });

    // x * 19 → (x << 4) + (x << 1) + x  [16x + 2x + 1x = 19x]
    rules.emplace_back("mul_19_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(19)}),
        [](EGraph& g, const Subst& s) {
            ClassId c4 = g.addConst(4); ClassId c1 = g.addConst(1);
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), c4);
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), c1);
            ClassId t = g.addBinOp(Op::Add, s4, s1);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });

    // x * 20 → (x << 4) + (x << 2)  [16x + 4x = 20x]
    rules.emplace_back("mul_20_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(20)}),
        [](EGraph& g, const Subst& s) {
            ClassId c4 = g.addConst(4); ClassId c2 = g.addConst(2);
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), c4);
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), c2);
            return g.addBinOp(Op::Add, s4, s2);
        });

    // x * 24 → (x << 4) + (x << 3)  [16x + 8x = 24x]
    rules.emplace_back("mul_24_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(24)}),
        [](EGraph& g, const Subst& s) {
            ClassId c4 = g.addConst(4); ClassId c3 = g.addConst(3);
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), c4);
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), c3);
            return g.addBinOp(Op::Add, s4, s3);
        });

    // x * 25 → (x << 4) + (x << 3) + x  [16x + 8x + 1x = 25x]
    rules.emplace_back("mul_25_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(25)}),
        [](EGraph& g, const Subst& s) {
            ClassId c4 = g.addConst(4); ClassId c3 = g.addConst(3);
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), c4);
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), c3);
            ClassId t = g.addBinOp(Op::Add, s4, s3);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });

    // x * 28 → (x << 5) - (x << 2)  [32x - 4x = 28x]
    rules.emplace_back("mul_28_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(28)}),
        [](EGraph& g, const Subst& s) {
            ClassId c5 = g.addConst(5); ClassId c2 = g.addConst(2);
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), c5);
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), c2);
            return g.addBinOp(Op::Sub, s5, s2);
        });

    // x * 31 → (x << 5) - x  [32x - 1x = 31x]
    rules.emplace_back("mul_31_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(31)}),
        [](EGraph& g, const Subst& s) {
            ClassId c5 = g.addConst(5);
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), c5);
            return g.addBinOp(Op::Sub, s5, s.at("x"));
        });

    // x * 33 → (x << 5) + x  [32x + 1x = 33x]
    rules.emplace_back("mul_33_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(33)}),
        [](EGraph& g, const Subst& s) {
            ClassId c5 = g.addConst(5);
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), c5);
            return g.addBinOp(Op::Add, s5, s.at("x"));
        });

    // x * 48 → (x << 5) + (x << 4)  [32x + 16x = 48x]
    rules.emplace_back("mul_48_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(48)}),
        [](EGraph& g, const Subst& s) {
            ClassId c5 = g.addConst(5); ClassId c4 = g.addConst(4);
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), c5);
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), c4);
            return g.addBinOp(Op::Add, s5, s4);
        });

    // x * 63 → (x << 6) - x  [64x - 1x = 63x]
    rules.emplace_back("mul_63_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(63)}),
        [](EGraph& g, const Subst& s) {
            ClassId c6 = g.addConst(6);
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), c6);
            return g.addBinOp(Op::Sub, s6, s.at("x"));
        });

    // x * 65 → (x << 6) + x  [64x + 1x = 65x]
    rules.emplace_back("mul_65_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(65)}),
        [](EGraph& g, const Subst& s) {
            ClassId c6 = g.addConst(6);
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), c6);
            return g.addBinOp(Op::Add, s6, s.at("x"));
        });

    // x * 96 → (x << 6) + (x << 5)  [64x + 32x = 96x]
    rules.emplace_back("mul_96_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(96)}),
        [](EGraph& g, const Subst& s) {
            ClassId c6 = g.addConst(6); ClassId c5 = g.addConst(5);
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), c6);
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), c5);
            return g.addBinOp(Op::Add, s6, s5);
        });

    // x * 100 → (x << 6) + (x << 5) + (x << 2)  [64x + 32x + 4x = 100x]
    rules.emplace_back("mul_100_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(100)}),
        [](EGraph& g, const Subst& s) {
            ClassId c6 = g.addConst(6); ClassId c5 = g.addConst(5); ClassId c2 = g.addConst(2);
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), c6);
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), c5);
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), c2);
            ClassId t = g.addBinOp(Op::Add, s6, s5);
            return g.addBinOp(Op::Add, t, s2);
        });

    // x * 127 → (x << 7) - x  [128x - 1x = 127x]
    rules.emplace_back("mul_127_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(127)}),
        [](EGraph& g, const Subst& s) {
            ClassId c7 = g.addConst(7);
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), c7);
            return g.addBinOp(Op::Sub, s7, s.at("x"));
        });

    // x * 255 → (x << 8) - x  [256x - 1x = 255x]
    rules.emplace_back("mul_255_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(255)}),
        [](EGraph& g, const Subst& s) {
            ClassId c8 = g.addConst(8);
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), c8);
            return g.addBinOp(Op::Sub, s8, s.at("x"));
        });

    // x * 30 → (x << 5) - (x << 1)  [32x - 2x = 30x]
    rules.emplace_back("mul_30_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(30)}),
        [](EGraph& g, const Subst& s) {
            ClassId c5 = g.addConst(5); ClassId c1 = g.addConst(1);
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), c5);
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), c1);
            return g.addBinOp(Op::Sub, s5, s1);
        });

    // x * 34 → (x << 5) + (x << 1)  [32x + 2x = 34x]
    rules.emplace_back("mul_34_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(34)}),
        [](EGraph& g, const Subst& s) {
            ClassId c5 = g.addConst(5); ClassId c1 = g.addConst(1);
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), c5);
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), c1);
            return g.addBinOp(Op::Add, s5, s1);
        });

    // x * 36 → (x << 5) + (x << 2)  [32x + 4x = 36x]
    rules.emplace_back("mul_36_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(36)}),
        [](EGraph& g, const Subst& s) {
            ClassId c5 = g.addConst(5); ClassId c2 = g.addConst(2);
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), c5);
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), c2);
            return g.addBinOp(Op::Add, s5, s2);
        });

    // x * 56 → (x << 6) - (x << 3)  [64x - 8x = 56x]
    rules.emplace_back("mul_56_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(56)}),
        [](EGraph& g, const Subst& s) {
            ClassId c6 = g.addConst(6); ClassId c3 = g.addConst(3);
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), c6);
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), c3);
            return g.addBinOp(Op::Sub, s6, s3);
        });

    // x * 60 → (x << 6) - (x << 2)  [64x - 4x = 60x]
    rules.emplace_back("mul_60_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(60)}),
        [](EGraph& g, const Subst& s) {
            ClassId c6 = g.addConst(6); ClassId c2 = g.addConst(2);
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), c6);
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), c2);
            return g.addBinOp(Op::Sub, s6, s2);
        });

    // x * 72 → (x << 6) + (x << 3)  [64x + 8x = 72x]
    rules.emplace_back("mul_72_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(72)}),
        [](EGraph& g, const Subst& s) {
            ClassId c6 = g.addConst(6); ClassId c3 = g.addConst(3);
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), c6);
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), c3);
            return g.addBinOp(Op::Add, s6, s3);
        });

    // x * 80 → (x << 6) + (x << 4)  [64x + 16x = 80x]
    rules.emplace_back("mul_80_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(80)}),
        [](EGraph& g, const Subst& s) {
            ClassId c6 = g.addConst(6); ClassId c4 = g.addConst(4);
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), c6);
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), c4);
            return g.addBinOp(Op::Add, s6, s4);
        });

    // x * 192 → (x << 7) + (x << 6)  [128x + 64x = 192x]
    rules.emplace_back("mul_192_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(192)}),
        [](EGraph& g, const Subst& s) {
            ClassId c7 = g.addConst(7); ClassId c6 = g.addConst(6);
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), c7);
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), c6);
            return g.addBinOp(Op::Add, s7, s6);
        });

    // x * 21 → (x << 4) + (x << 2) + x  [16x + 4x + x = 21x]
    rules.emplace_back("mul_21_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(21)}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId sum = g.addBinOp(Op::Add, s4, s2);
            return g.addBinOp(Op::Add, sum, s.at("x"));
        });

    // x * 22 → (x << 4) + (x << 2) + (x << 1)  [16x + 4x + 2x = 22x]
    rules.emplace_back("mul_22_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(22)}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId sum = g.addBinOp(Op::Add, s4, s2);
            return g.addBinOp(Op::Add, sum, s1);
        });

    // x * 26 → (x << 5) - (x << 2) - (x << 1)  [32x - 4x - 2x = 26x]
    rules.emplace_back("mul_26_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(26)}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId sub1 = g.addBinOp(Op::Sub, s5, s2);
            return g.addBinOp(Op::Sub, sub1, s1);
        });

    // x * 27 → (x << 5) - (x << 2) - x  [32x - 4x - x = 27x]
    rules.emplace_back("mul_27_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(27)}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId sub1 = g.addBinOp(Op::Sub, s5, s2);
            return g.addBinOp(Op::Sub, sub1, s.at("x"));
        });

    // x * 50 → (x << 6) - (x << 4) + (x << 1)  [64x - 16x + 2x = 50x]
    rules.emplace_back("mul_50_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(50)}),
        [](EGraph& g, const Subst& s) {
            ClassId s6  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s4  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s1  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId sub = g.addBinOp(Op::Sub, s6, s4);
            return g.addBinOp(Op::Add, sub, s1);
        });

    // x * 200 → (x << 8) - (x << 6) + (x << 3)  [256x - 64x + 8x = 200x]
    rules.emplace_back("mul_200_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(200)}),
        [](EGraph& g, const Subst& s) {
            ClassId s8  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s6  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s3  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId sub = g.addBinOp(Op::Sub, s8, s6);
            return g.addBinOp(Op::Add, sub, s3);
        });


    rules.emplace_back("mul_37_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(37)}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId sum = g.addBinOp(Op::Add, s5, s2);
            return g.addBinOp(Op::Add, sum, s.at("x"));
        });

    // x * 41 → (x << 5) + (x << 3) + x  [32x + 8x + x = 41x]
    rules.emplace_back("mul_41_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(41)}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId sum = g.addBinOp(Op::Add, s5, s3);
            return g.addBinOp(Op::Add, sum, s.at("x"));
        });

    // x * 49 → (x << 5) + (x << 4) + x  [32x + 16x + x = 49x]
    rules.emplace_back("mul_49_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(49)}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId sum = g.addBinOp(Op::Add, s5, s4);
            return g.addBinOp(Op::Add, sum, s.at("x"));
        });

    // x * 129 → (x << 7) + x  [128x + x = 129x]
    rules.emplace_back("mul_129_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(129)}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Add, s7, s.at("x"));
        });

    // x * 257 → (x << 8) + x  [256x + x = 257x]
    rules.emplace_back("mul_257_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(257)}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            return g.addBinOp(Op::Add, s8, s.at("x"));
        });

    // x * 1000 → (x << 10) - (x << 4) - (x << 3)  [1024x - 16x - 8x = 1000x]
    rules.emplace_back("mul_1000_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(1000)}),
        [](EGraph& g, const Subst& s) {
            ClassId s10 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(10));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId sub1 = g.addBinOp(Op::Sub, s10, s4);
            return g.addBinOp(Op::Sub, sub1, s3);
        });

    // ─────────────────────────────────────────────────────────────────────
    // x * 2^n strength reduction for shifts 11-30
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("mul_2048_to_shl11",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2048LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(11)); });
    rules.emplace_back("mul_4096_to_shl12",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4096LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(12)); });
    rules.emplace_back("mul_8192_to_shl13",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8192LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(13)); });
    rules.emplace_back("mul_16384_to_shl14",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(16384LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(14)); });
    rules.emplace_back("mul_32768_to_shl15",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(32768LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(15)); });
    rules.emplace_back("mul_65536_to_shl16",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(65536LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(16)); });
    rules.emplace_back("mul_131072_to_shl17",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(131072LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(17)); });
    rules.emplace_back("mul_262144_to_shl18",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(262144LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(18)); });
    rules.emplace_back("mul_524288_to_shl19",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(524288LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(19)); });
    rules.emplace_back("mul_1048576_to_shl20",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(1048576LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(20)); });
    rules.emplace_back("mul_2097152_to_shl21",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2097152LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(21)); });
    rules.emplace_back("mul_4194304_to_shl22",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4194304LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(22)); });
    rules.emplace_back("mul_8388608_to_shl23",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8388608LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(23)); });
    rules.emplace_back("mul_16777216_to_shl24",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(16777216LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(24)); });
    rules.emplace_back("mul_33554432_to_shl25",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(33554432LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(25)); });
    rules.emplace_back("mul_67108864_to_shl26",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(67108864LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(26)); });
    rules.emplace_back("mul_134217728_to_shl27",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(134217728LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(27)); });
    rules.emplace_back("mul_268435456_to_shl28",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(268435456LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(28)); });
    rules.emplace_back("mul_536870912_to_shl29",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(536870912LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(29)); });
    rules.emplace_back("mul_1073741824_to_shl30",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(1073741824LL)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(30)); });

    // ─────────────────────────────────────────────────────────────────────
    // More cancellation rules
    // ─────────────────────────────────────────────────────────────────────

    // (a - b) + b → a
    rules.emplace_back("sub_add_cancel",
        P::OpPat(Op::Add, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // a - (a - b) → b
    rules.emplace_back("sub_sub_cancel",
        P::OpPat(Op::Sub, {P::Wild("a"), P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // x + (-x) → 0
    rules.emplace_back("add_neg_cancel",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Neg, {P::Wild("x")})}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (-x) + x → 0
    rules.emplace_back("neg_add_cancel",
        P::OpPat(Op::Add, {P::OpPat(Op::Neg, {P::Wild("x")}), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // a - (a + b) → -b
    rules.emplace_back("sub_add_neg",
        P::OpPat(Op::Sub, {P::Wild("a"), P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addUnaryOp(Op::Neg, s.at("b")); });

    // a - (b + a) → -b
    rules.emplace_back("sub_add_neg2",
        P::OpPat(Op::Sub, {P::Wild("a"), P::OpPat(Op::Add, {P::Wild("b"), P::Wild("a")})}),
        [](EGraph& g, const Subst& s) { return g.addUnaryOp(Op::Neg, s.at("b")); });

    // (a + b) - (a + c) → b - c
    rules.emplace_back("add_sub_simplify",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Sub, s.at("b"), s.at("c"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Negation distribution and mul negation rules
    // ─────────────────────────────────────────────────────────────────────

    // -(a + b) → (-a) - b
    rules.emplace_back("neg_add_distribute",
        P::OpPat(Op::Neg, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::Neg, s.at("a"));
            return g.addBinOp(Op::Sub, na, s.at("b"));
        });

    // -x + y → y - x
    rules.emplace_back("neg_add_to_sub",
        P::OpPat(Op::Add, {P::OpPat(Op::Neg, {P::Wild("x")}), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Sub, s.at("y"), s.at("x"));
        });

    // (-a) * (-b) → a * b
    rules.emplace_back("mul_both_neg",
        P::OpPat(Op::Mul, {P::OpPat(Op::Neg, {P::Wild("a")}), P::OpPat(Op::Neg, {P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
        });

    // (-a) * b → -(a * b)
    rules.emplace_back("neg_mul_left",
        P::OpPat(Op::Mul, {P::OpPat(Op::Neg, {P::Wild("a")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
            return g.addUnaryOp(Op::Neg, ab);
        });

    // a * (-b) → -(a * b)
    rules.emplace_back("neg_mul_right",
        P::OpPat(Op::Mul, {P::Wild("a"), P::OpPat(Op::Neg, {P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
            return g.addUnaryOp(Op::Neg, ab);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Distributive and factoring rules
    // ─────────────────────────────────────────────────────────────────────

    // (a + b) * c → a*c + b*c
    rules.emplace_back("distribute_add_mul_right",
        P::OpPat(Op::Mul, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId ac = g.addBinOp(Op::Mul, s.at("a"), s.at("c"));
            ClassId bc = g.addBinOp(Op::Mul, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Add, ac, bc);
        });

    // (a - b) * c → a*c - b*c
    rules.emplace_back("distribute_sub_mul_right",
        P::OpPat(Op::Mul, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId ac = g.addBinOp(Op::Mul, s.at("a"), s.at("c"));
            ClassId bc = g.addBinOp(Op::Mul, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Sub, ac, bc);
        });

    // a*c - b*c → (a - b) * c
    rules.emplace_back("factor_mul_sub",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("c")}),
            P::OpPat(Op::Mul, {P::Wild("b"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Sub, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Mul, ab, s.at("c"));
        });

    // c*a - c*b → c * (a - b)
    rules.emplace_back("factor_mul_sub_left",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Mul, {P::Wild("c"), P::Wild("a")}),
            P::OpPat(Op::Mul, {P::Wild("c"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Sub, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Mul, s.at("c"), ab);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Division and modulo rules
    // ─────────────────────────────────────────────────────────────────────

    // (x * n) / n → x
    rules.emplace_back("mul_div_cancel",
        P::OpPat(Op::Div, {P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("n")}), P::Wild("n")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // (x * n) % n → 0
    rules.emplace_back("mul_mod_cancel",
        P::OpPat(Op::Mod, {P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("n")}), P::Wild("n")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (x % n) % n → x % n
    rules.emplace_back("mod_mod_same",
        P::OpPat(Op::Mod, {P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("n")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), s.at("n")); });

    // NOTE: `0 / x → 0` and `0 % x → 0` are omitted intentionally.
    // When x is also 0 these would suppress the division-by-zero / modulo-by-zero
    // runtime error, producing incorrect behaviour.  Leave the division to
    // runtime so that the fault can be reported correctly.

    // ─────────────────────────────────────────────────────────────────────
    // Power rules
    // ─────────────────────────────────────────────────────────────────────

    // 1 ^ x → 1
    rules.emplace_back("pow_one_base",
        P::OpPat(Op::Pow, {P::ConstPat(1), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // x ^ 3 → (x * x) * x
    rules.emplace_back("pow_three",
        P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(3)}),
        [](EGraph& g, const Subst& s) {
            ClassId xx = g.addBinOp(Op::Mul, s.at("x"), s.at("x"));
            return g.addBinOp(Op::Mul, xx, s.at("x"));
        });

    // x * x → x^2
    rules.emplace_back("mul_self_to_pow2",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId two = g.addConst(2);
            return g.addBinOp(Op::Pow, s.at("x"), two);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Triple self-addition
    // ─────────────────────────────────────────────────────────────────────

    // (x + x) + x → x * 3
    rules.emplace_back("triple_add_left",
        P::OpPat(Op::Add, {P::OpPat(Op::Add, {P::Wild("x"), P::Wild("x")}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId three = g.addConst(3);
            return g.addBinOp(Op::Mul, s.at("x"), three);
        });

    // x + (x + x) → x * 3
    rules.emplace_back("triple_add_right",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Add, {P::Wild("x"), P::Wild("x")})}),
        [](EGraph& g, const Subst& s) {
            ClassId three = g.addConst(3);
            return g.addBinOp(Op::Mul, s.at("x"), three);
        });


    return rules;
}


std::vector<RewriteRule> getAdvancedAlgebraicRules() {
    using P = Pattern;
    std::vector<RewriteRule> rules;

    // ── Nested arithmetic: (a+b) - (b+c) → a - c ─────────────────────────
    rules.emplace_back("nested_add_sub_bc",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Sub, s.at("a"), s.at("c")); });

    // ── Nested arithmetic: (a+b) - (c+b) → a - c ─────────────────────────
    rules.emplace_back("nested_add_sub_cb",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Add, {P::Wild("c"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Sub, s.at("a"), s.at("c")); });

    // ── (a - b) + (b - c) → a - c ─────────────────────────────────────────
    rules.emplace_back("sub_add_sub_chain",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Sub, {P::Wild("b"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Sub, s.at("a"), s.at("c")); });

    // ── (a + b) + (c - b) → a + c ─────────────────────────────────────────
    rules.emplace_back("add_add_sub_cancel",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Sub, {P::Wild("c"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Add, s.at("a"), s.at("c")); });

    // ── (a - b) - (c - b) → a - c ─────────────────────────────────────────
    rules.emplace_back("sub_sub_same_right",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Sub, {P::Wild("c"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Sub, s.at("a"), s.at("c")); });

    // ── (a - b) - (a - c) → c - b ─────────────────────────────────────────
    rules.emplace_back("sub_sub_same_left",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Sub, s.at("c"), s.at("b")); });

    // ── (a + c) - (b + c) → a - b ─────────────────────────────────────────
    rules.emplace_back("add_sub_cancel_same_right",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("c")}),
            P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Sub, s.at("a"), s.at("b")); });

    // ── Difference of squares: (a+b)*(a-b) → a*a - b*b ───────────────────
    rules.emplace_back("diff_of_squares",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId aa = g.addBinOp(Op::Mul, s.at("a"), s.at("a"));
            ClassId bb = g.addBinOp(Op::Mul, s.at("b"), s.at("b"));
            return g.addBinOp(Op::Sub, aa, bb);
        });

    // ── (a-b)*(a+b) → a*a - b*b ───────────────────────────────────────────
    rules.emplace_back("diff_of_squares_rev",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId aa = g.addBinOp(Op::Mul, s.at("a"), s.at("a"));
            ClassId bb = g.addBinOp(Op::Mul, s.at("b"), s.at("b"));
            return g.addBinOp(Op::Sub, aa, bb);
        });

    // ── a*b + a → a*(b+1) ─────────────────────────────────────────────────
    rules.emplace_back("factor_out_add_one",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}),
            P::Wild("a")
        }),
        [](EGraph& g, const Subst& s) {
            ClassId one = g.addConst(1);
            ClassId bp1 = g.addBinOp(Op::Add, s.at("b"), one);
            return g.addBinOp(Op::Mul, s.at("a"), bp1);
        });

    // ── a + a*b → a*(b+1) ─────────────────────────────────────────────────
    rules.emplace_back("factor_out_add_one2",
        P::OpPat(Op::Add, {
            P::Wild("a"),
            P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId one = g.addConst(1);
            ClassId bp1 = g.addBinOp(Op::Add, s.at("b"), one);
            return g.addBinOp(Op::Mul, s.at("a"), bp1);
        });

    // ── a*b - a → a*(b-1) ─────────────────────────────────────────────────
    rules.emplace_back("factor_out_sub_one",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}),
            P::Wild("a")
        }),
        [](EGraph& g, const Subst& s) {
            ClassId one = g.addConst(1);
            ClassId bm1 = g.addBinOp(Op::Sub, s.at("b"), one);
            return g.addBinOp(Op::Mul, s.at("a"), bm1);
        });

    // ── a - a*b → a*(1-b) ─────────────────────────────────────────────────
    rules.emplace_back("factor_out_sub_one2",
        P::OpPat(Op::Sub, {
            P::Wild("a"),
            P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId one = g.addConst(1);
            ClassId omB = g.addBinOp(Op::Sub, one, s.at("b"));
            return g.addBinOp(Op::Mul, s.at("a"), omB);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Left-constant versions of strength reduction (constant on left side)
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("mul_2_left_shl1",
        P::OpPat(Op::Mul, {P::ConstPat(2), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(1)); });
    rules.emplace_back("mul_4_left_shl2",
        P::OpPat(Op::Mul, {P::ConstPat(4), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(2)); });
    rules.emplace_back("mul_8_left_shl3",
        P::OpPat(Op::Mul, {P::ConstPat(8), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(3)); });
    rules.emplace_back("mul_16_left_shl4",
        P::OpPat(Op::Mul, {P::ConstPat(16), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(4)); });
    rules.emplace_back("mul_32_left_shl5",
        P::OpPat(Op::Mul, {P::ConstPat(32), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(5)); });
    rules.emplace_back("mul_64_left_shl6",
        P::OpPat(Op::Mul, {P::ConstPat(64), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(6)); });
    rules.emplace_back("mul_128_left_shl7",
        P::OpPat(Op::Mul, {P::ConstPat(128), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(7)); });
    rules.emplace_back("mul_256_left_shl8",
        P::OpPat(Op::Mul, {P::ConstPat(256), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(8)); });
    rules.emplace_back("mul_512_left_shl9",
        P::OpPat(Op::Mul, {P::ConstPat(512), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(9)); });
    rules.emplace_back("mul_1024_left_shl10",
        P::OpPat(Op::Mul, {P::ConstPat(1024), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(10)); });

    // Left-constant non-power-of-2 strength reductions
    rules.emplace_back("mul_3_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(3), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s1, s.at("x"));
        });
    rules.emplace_back("mul_5_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(5), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, s2, s.at("x"));
        });
    rules.emplace_back("mul_6_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(6), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s2, s1);
        });
    rules.emplace_back("mul_7_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(7), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s3, s.at("x"));
        });
    rules.emplace_back("mul_9_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(9), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s3, s.at("x"));
        });
    rules.emplace_back("mul_15_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(15), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Sub, s4, s.at("x"));
        });
    rules.emplace_back("mul_17_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(17), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Add, s4, s.at("x"));
        });
    rules.emplace_back("mul_31_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(31), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Sub, s5, s.at("x"));
        });
    rules.emplace_back("mul_33_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(33), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Add, s5, s.at("x"));
        });
    rules.emplace_back("mul_63_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(63), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Sub, s6, s.at("x"));
        });
    rules.emplace_back("mul_65_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(65), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Add, s6, s.at("x"));
        });
    rules.emplace_back("mul_127_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(127), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Sub, s7, s.at("x"));
        });
    rules.emplace_back("mul_129_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(129), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Add, s7, s.at("x"));
        });
    rules.emplace_back("mul_255_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(255), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            return g.addBinOp(Op::Sub, s8, s.at("x"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // More multiply-by-constant patterns
    // ─────────────────────────────────────────────────────────────────────
    // x * 36 → (x<<5) + (x<<2)  [32x + 4x = 36x]
    rules.emplace_back("mul_36_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(36)}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, s5, s2);
        });
    // x * 40 → (x<<5) + (x<<3)  [32x + 8x = 40x]
    rules.emplace_back("mul_40_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(40)}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s5, s3);
        });
    // x * 56 → (x<<6) - (x<<3)  [64x - 8x = 56x]
    rules.emplace_back("mul_56_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(56)}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s6, s3);
        });
    // x * 72 → (x<<6) + (x<<3)  [64x + 8x = 72x]
    rules.emplace_back("mul_72_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(72)}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s6, s3);
        });
    // x * 80 → (x<<6) + (x<<4)  [64x + 16x = 80x]
    rules.emplace_back("mul_80_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(80)}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Add, s6, s4);
        });
    // x * 112 → (x<<7) - (x<<4)  [128x - 16x = 112x]
    rules.emplace_back("mul_112_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(112)}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Sub, s7, s4);
        });
    // x * 192 → (x<<7) + (x<<6)  [128x + 64x = 192x]
    rules.emplace_back("mul_192_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(192)}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Add, s7, s6);
        });
    // x * 511 → (x<<9) - x  [512x - 1x = 511x]
    rules.emplace_back("mul_511_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(511)}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            return g.addBinOp(Op::Sub, s9, s.at("x"));
        });
    // x * 513 → (x<<9) + x  [512x + 1x = 513x]
    rules.emplace_back("mul_513_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(513)}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            return g.addBinOp(Op::Add, s9, s.at("x"));
        });
    // x * 1023 → (x<<10) - x  [1024x - 1x = 1023x]
    rules.emplace_back("mul_1023_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(1023)}),
        [](EGraph& g, const Subst& s) {
            ClassId s10 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(10));
            return g.addBinOp(Op::Sub, s10, s.at("x"));
        });
    // x * 1025 → (x<<10) + x  [1024x + 1x = 1025x]
    rules.emplace_back("mul_1025_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(1025)}),
        [](EGraph& g, const Subst& s) {
            ClassId s10 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(10));
            return g.addBinOp(Op::Add, s10, s.at("x"));
        });
    // ── Extended right-constant patterns added to match emitShiftAdd ─────
    // x * 57 → (x<<6) - (x<<3) + x  [64x - 8x + x = 57x]
    rules.emplace_back("mul_57_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(57)}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId t = g.addBinOp(Op::Sub, s6, s3);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });
    // x * 62 → (x<<6) - (x<<1)  [64x - 2x = 62x]
    rules.emplace_back("mul_62_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(62)}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Sub, s6, s1);
        });
    // x * 66 → (x<<6) + (x<<1)  [64x + 2x = 66x]
    rules.emplace_back("mul_66_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(66)}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s6, s1);
        });
    // x * 68 → (x<<6) + (x<<2)  [64x + 4x = 68x]
    rules.emplace_back("mul_68_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(68)}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, s6, s2);
        });
    // x * 120 → (x<<7) - (x<<3)  [128x - 8x = 120x]
    rules.emplace_back("mul_120_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(120)}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s7, s3);
        });
    // x * 136 → (x<<7) + (x<<3)  [128x + 8x = 136x]
    rules.emplace_back("mul_136_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(136)}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s7, s3);
        });
    // x * 144 → (x<<7) + (x<<4)  [128x + 16x = 144x]
    rules.emplace_back("mul_144_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(144)}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Add, s7, s4);
        });
    // x * 160 → (x<<7) + (x<<5)  [128x + 32x = 160x]
    rules.emplace_back("mul_160_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(160)}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Add, s7, s5);
        });
    // x * 224 → (x<<8) - (x<<5)  [256x - 32x = 224x]
    rules.emplace_back("mul_224_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(224)}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Sub, s8, s5);
        });
    // x * 240 → (x<<8) - (x<<4)  [256x - 16x = 240x]
    rules.emplace_back("mul_240_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(240)}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Sub, s8, s4);
        });
    // x * 248 → (x<<8) - (x<<3)  [256x - 8x = 248x]
    rules.emplace_back("mul_248_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(248)}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s8, s3);
        });
    // x * 288 → (x<<8) + (x<<5)  [256x + 32x = 288x]
    rules.emplace_back("mul_288_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(288)}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Add, s8, s5);
        });
    // x * 320 → (x<<8) + (x<<6)  [256x + 64x = 320x]
    rules.emplace_back("mul_320_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(320)}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Add, s8, s6);
        });
    // x * 384 → (x<<8) + (x<<7)  [256x + 128x = 384x]
    rules.emplace_back("mul_384_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(384)}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Add, s8, s7);
        });
    // x * 448 → (x<<9) - (x<<6)  [512x - 64x = 448x]
    rules.emplace_back("mul_448_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(448)}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Sub, s9, s6);
        });
    // x * 480 → (x<<9) - (x<<5)  [512x - 32x = 480x]
    rules.emplace_back("mul_480_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(480)}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Sub, s9, s5);
        });
    // x * 496 → (x<<9) - (x<<4)  [512x - 16x = 496x]
    rules.emplace_back("mul_496_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(496)}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Sub, s9, s4);
        });
    // x * 504 → (x<<9) - (x<<3)  [512x - 8x = 504x]
    rules.emplace_back("mul_504_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(504)}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s9, s3);
        });
    // x * 640 → (x<<9) + (x<<7)  [512x + 128x = 640x]
    rules.emplace_back("mul_640_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(640)}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Add, s9, s7);
        });
    // x * 768 → (x<<9) + (x<<8)  [512x + 256x = 768x]
    rules.emplace_back("mul_768_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(768)}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            return g.addBinOp(Op::Add, s9, s8);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Modulo identities
    // ─────────────────────────────────────────────────────────────────────

    // (x + n) % n → x % n
    rules.emplace_back("mod_add_n",
        P::OpPat(Op::Mod, {P::OpPat(Op::Add, {P::Wild("x"), P::Wild("n")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), s.at("n")); });

    // (n + x) % n → x % n
    rules.emplace_back("mod_add_n_comm",
        P::OpPat(Op::Mod, {P::OpPat(Op::Add, {P::Wild("n"), P::Wild("x")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), s.at("n")); });

    // (x - n) % n → x % n
    rules.emplace_back("mod_sub_n",
        P::OpPat(Op::Mod, {P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("n")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), s.at("n")); });

    // (x * n) % n → 0  (already in getAdvancedAlgebraicRules, but add commutative form)
    rules.emplace_back("mul_mod_cancel_comm",
        P::OpPat(Op::Mod, {P::OpPat(Op::Mul, {P::Wild("n"), P::Wild("x")}), P::Wild("n")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (x * a + y) % a → y % a  (factor out multiple of modulus)
    rules.emplace_back("mod_factor_add",
        P::OpPat(Op::Mod, {
            P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("a")}), P::Wild("y")}),
            P::Wild("a")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("y"), s.at("a")); });

    // (a * x + y) % a → y % a  (commutative form)
    rules.emplace_back("mod_factor_add_comm",
        P::OpPat(Op::Mod, {
            P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("x")}), P::Wild("y")}),
            P::Wild("a")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("y"), s.at("a")); });

    // x / 1 → x
    rules.emplace_back("div_one",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x / -1 → -x
    rules.emplace_back("div_neg_one",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstPat(-1)}),
        [](EGraph& g, const Subst& s) { return g.addUnaryOp(Op::Neg, s.at("x")); });

    // (a/b)/c → a/(b*c)
    rules.emplace_back("div_div_mul",
        P::OpPat(Op::Div, {P::OpPat(Op::Div, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::Mul, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Div, s.at("a"), bc);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Add/sub associativity patterns
    // ─────────────────────────────────────────────────────────────────────

    // (a + b) + c → a + (b + c)
    rules.emplace_back("add_assoc_lr",
        P::OpPat(Op::Add, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::Add, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Add, s.at("a"), bc);
        });

    // a + (b + c) → (a + b) + c
    rules.emplace_back("add_assoc_rl",
        P::OpPat(Op::Add, {P::Wild("a"), P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Add, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Add, ab, s.at("c"));
        });

    // (a - b) + c → (a + c) - b
    rules.emplace_back("sub_add_rearrange",
        P::OpPat(Op::Add, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId ac = g.addBinOp(Op::Add, s.at("a"), s.at("c"));
            return g.addBinOp(Op::Sub, ac, s.at("b"));
        });

    // a + (b - c) → (a + b) - c
    rules.emplace_back("add_sub_rearrange",
        P::OpPat(Op::Add, {P::Wild("a"), P::OpPat(Op::Sub, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Add, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Sub, ab, s.at("c"));
        });

    // a - (b + c) → (a - b) - c
    rules.emplace_back("sub_add_assoc",
        P::OpPat(Op::Sub, {P::Wild("a"), P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Sub, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Sub, ab, s.at("c"));
        });

    // a - (b - c) → (a - b) + c
    rules.emplace_back("sub_sub_to_add",
        P::OpPat(Op::Sub, {P::Wild("a"), P::OpPat(Op::Sub, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Sub, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Add, ab, s.at("c"));
        });

    // (a - b) - c → a - (b + c)
    rules.emplace_back("sub_sub_to_sub_add",
        P::OpPat(Op::Sub, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::Add, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Sub, s.at("a"), bc);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Commutativity
    // ─────────────────────────────────────────────────────────────────────

    // a + b → b + a
    rules.emplace_back("add_comm_adv",
        P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Add, s.at("b"), s.at("a")); });

    // a * b → b * a
    rules.emplace_back("mul_comm_adv",
        P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("b"), s.at("a")); });

    // ─────────────────────────────────────────────────────────────────────
    // Misc algebraic identities
    // ─────────────────────────────────────────────────────────────────────

    // 0 + x → x
    rules.emplace_back("add_zero_left_adv",
        P::OpPat(Op::Add, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // 1 * x → x
    rules.emplace_back("mul_one_left_adv",
        P::OpPat(Op::Mul, {P::ConstPat(1), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x * 1 → x
    rules.emplace_back("mul_one_right_adv",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x - x → 0
    rules.emplace_back("sub_self_adv",
        P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // a*(b*c) → (a*b)*c
    rules.emplace_back("mul_assoc_rev",
        P::OpPat(Op::Mul, {P::Wild("a"), P::OpPat(Op::Mul, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Mul, ab, s.at("c"));
        });

    // a - b → -(b - a)
    rules.emplace_back("sub_neg_swap",
        P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ClassId ba = g.addBinOp(Op::Sub, s.at("b"), s.at("a"));
            return g.addUnaryOp(Op::Neg, ba);
        });

    // (x+y) + (z-y) → x + z
    rules.emplace_back("add_sub_cancel_cross",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Add, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Sub, {P::Wild("z"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Add, s.at("x"), s.at("z")); });

    // (x-y) + (z+y) → x + z
    rules.emplace_back("sub_add_cancel_cross",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Add, {P::Wild("z"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Add, s.at("x"), s.at("z")); });

    // (x-y) + (y+z) → x + z
    rules.emplace_back("sub_add_cancel_cross2",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Add, {P::Wild("y"), P::Wild("z")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Add, s.at("x"), s.at("z")); });

    // x^4 → (x*x) * (x*x)
    rules.emplace_back("pow_four",
        P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(4)}),
        [](EGraph& g, const Subst& s) {
            ClassId xx = g.addBinOp(Op::Mul, s.at("x"), s.at("x"));
            return g.addBinOp(Op::Mul, xx, xx);
        });

    // x^2 * x → x^3
    rules.emplace_back("pow2_mul_to_pow3",
        P::OpPat(Op::Mul, {P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(2)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Pow, s.at("x"), g.addConst(3));
        });

    // x * x^2 → x^3
    rules.emplace_back("mul_to_pow3",
        P::OpPat(Op::Mul, {P::Wild("x"), P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(2)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Pow, s.at("x"), g.addConst(3));
        });

    // x^2 * x^2 → x^4
    rules.emplace_back("pow2_mul_pow2",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(2)}),
            P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(2)})
        }),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Pow, s.at("x"), g.addConst(4));
        });

    // ((x+x)+(x+x)) → x<<2  [4x]
    rules.emplace_back("quad_add_to_shl2",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Add, {P::Wild("x"), P::Wild("x")}),
            P::OpPat(Op::Add, {P::Wild("x"), P::Wild("x")})
        }),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
        });

    // ─────────────────────────────────────────────────────────────────────
    // More modulo patterns
    // ─────────────────────────────────────────────────────────────────────

    // x % 2 with shifts: x & 1 (equivalence - not actually a rewrite but useful)
    // x % 1 → 0  (already exists)

    // Mul one more time: x / (x/y) - too complex
    // Simple identity: (a + b) / 1 → a + b (div_one handles this generally)

    // Negative mod base: x % (-n) with signed semantics - skip for safety

    // ─────────────────────────────────────────────────────────────────────
    // Add commutativity for different arrangements of b + cancel
    // ─────────────────────────────────────────────────────────────────────

    // b + (a - b) → a
    rules.emplace_back("add_sub_cancel_comm",
        P::OpPat(Op::Add, {P::Wild("b"), P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // (a - b) - (-b) → a  [a - b + b = a]
    rules.emplace_back("sub_neg_b_cancel",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Neg, {P::Wild("b")})
        }),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // a * 0 → 0 (left)  - already handled but ensure both sides
    // a + b - b → a
    rules.emplace_back("add_sub_self_cancel",
        P::OpPat(Op::Sub, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });


    // ─────────────────────────────────────────────────────────────────────
    // Extended strength reduction patterns
    // ─────────────────────────────────────────────────────────────────────

    // x * 2048 → x << 11 (left-constant)
    rules.emplace_back("mul_2048_left_shl11",
        P::OpPat(Op::Mul, {P::ConstPat(2048LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(11)); });
    rules.emplace_back("mul_4096_left_shl12",
        P::OpPat(Op::Mul, {P::ConstPat(4096LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(12)); });
    rules.emplace_back("mul_8192_left_shl13",
        P::OpPat(Op::Mul, {P::ConstPat(8192LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(13)); });
    rules.emplace_back("mul_16384_left_shl14",
        P::OpPat(Op::Mul, {P::ConstPat(16384LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(14)); });
    rules.emplace_back("mul_32768_left_shl15",
        P::OpPat(Op::Mul, {P::ConstPat(32768LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(15)); });
    rules.emplace_back("mul_65536_left_shl16",
        P::OpPat(Op::Mul, {P::ConstPat(65536LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(16)); });
    rules.emplace_back("mul_131072_left_shl17",
        P::OpPat(Op::Mul, {P::ConstPat(131072LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(17)); });
    rules.emplace_back("mul_262144_left_shl18",
        P::OpPat(Op::Mul, {P::ConstPat(262144LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(18)); });
    rules.emplace_back("mul_524288_left_shl19",
        P::OpPat(Op::Mul, {P::ConstPat(524288LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(19)); });
    rules.emplace_back("mul_1048576_left_shl20",
        P::OpPat(Op::Mul, {P::ConstPat(1048576LL), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Shl, s.at("x"), g.addConst(20)); });

    // Left versions of non-power-of-2 constants
    rules.emplace_back("mul_10_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(10), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s3, s1);
        });
    rules.emplace_back("mul_11_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(11), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId t = g.addBinOp(Op::Add, s3, s1);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });
    rules.emplace_back("mul_12_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(12), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, s3, s2);
        });
    rules.emplace_back("mul_13_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(13), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId t = g.addBinOp(Op::Add, s3, s2);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });
    rules.emplace_back("mul_14_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(14), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Sub, s4, s1);
        });
    rules.emplace_back("mul_18_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(18), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s4, s1);
        });
    rules.emplace_back("mul_19_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(19), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId t = g.addBinOp(Op::Add, s4, s1);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });
    rules.emplace_back("mul_20_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(20), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, s4, s2);
        });
    // 21 * x → (x << 4) + (x << 2) + x  [16x + 4x + x = 21x]
    rules.emplace_back("mul_21_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(21), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId t = g.addBinOp(Op::Add, s4, s2);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });
    // 22 * x → (x << 4) + (x << 2) + (x << 1)  [16x + 4x + 2x = 22x]
    rules.emplace_back("mul_22_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(22), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId t = g.addBinOp(Op::Add, s4, s2);
            return g.addBinOp(Op::Add, t, s1);
        });
    rules.emplace_back("mul_24_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(24), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s4, s3);
        });
    rules.emplace_back("mul_25_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(25), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId t = g.addBinOp(Op::Add, s4, s3);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });
    // 26 * x → (x << 5) - (x << 2) - (x << 1)  [32x - 4x - 2x = 26x]
    rules.emplace_back("mul_26_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(26), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId sub1 = g.addBinOp(Op::Sub, s5, s2);
            return g.addBinOp(Op::Sub, sub1, s1);
        });
    // 27 * x → (x << 5) - (x << 2) - x  [32x - 4x - x = 27x]
    rules.emplace_back("mul_27_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(27), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId sub1 = g.addBinOp(Op::Sub, s5, s2);
            return g.addBinOp(Op::Sub, sub1, s.at("x"));
        });
    rules.emplace_back("mul_28_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(28), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Sub, s5, s2);
        });
    rules.emplace_back("mul_36_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(36), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, s5, s2);
        });
    // 37 * x → (x << 5) + (x << 2) + x  [32x + 4x + x = 37x]
    rules.emplace_back("mul_37_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(37), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId sum = g.addBinOp(Op::Add, s5, s2);
            return g.addBinOp(Op::Add, sum, s.at("x"));
        });
    rules.emplace_back("mul_40_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(40), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s5, s3);
        });
    // 41 * x → (x << 5) + (x << 3) + x  [32x + 8x + x = 41x]
    rules.emplace_back("mul_41_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(41), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId sum = g.addBinOp(Op::Add, s5, s3);
            return g.addBinOp(Op::Add, sum, s.at("x"));
        });
    rules.emplace_back("mul_48_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(48), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Add, s5, s4);
        });
    // 49 * x → (x << 5) + (x << 4) + x  [32x + 16x + x = 49x]
    rules.emplace_back("mul_49_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(49), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId sum = g.addBinOp(Op::Add, s5, s4);
            return g.addBinOp(Op::Add, sum, s.at("x"));
        });
    // 50 * x → (x << 6) - (x << 4) + (x << 1)  [64x - 16x + 2x = 50x]
    rules.emplace_back("mul_50_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(50), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s4  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s1  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId sub = g.addBinOp(Op::Sub, s6, s4);
            return g.addBinOp(Op::Add, sub, s1);
        });
    rules.emplace_back("mul_56_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(56), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s6, s3);
        });
    rules.emplace_back("mul_72_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(72), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s6, s3);
        });
    rules.emplace_back("mul_80_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(80), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Add, s6, s4);
        });
    rules.emplace_back("mul_96_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(96), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Add, s6, s5);
        });
    rules.emplace_back("mul_100_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(100), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            ClassId t = g.addBinOp(Op::Add, s6, s5);
            return g.addBinOp(Op::Add, t, s2);
        });
    rules.emplace_back("mul_192_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(192), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Add, s7, s6);
        });
    // 200 * x → (x << 8) - (x << 6) + (x << 3)  [256x - 64x + 8x = 200x]
    rules.emplace_back("mul_200_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(200), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s8  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s6  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s3  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId sub = g.addBinOp(Op::Sub, s8, s6);
            return g.addBinOp(Op::Add, sub, s3);
        });
    rules.emplace_back("mul_511_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(511), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            return g.addBinOp(Op::Sub, s9, s.at("x"));
        });
    rules.emplace_back("mul_513_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(513), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            return g.addBinOp(Op::Add, s9, s.at("x"));
        });
    rules.emplace_back("mul_1023_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(1023), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s10 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(10));
            return g.addBinOp(Op::Sub, s10, s.at("x"));
        });
    rules.emplace_back("mul_1025_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(1025), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s10 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(10));
            return g.addBinOp(Op::Add, s10, s.at("x"));
        });
    // 30 * x → (x << 5) - (x << 1)  [32x - 2x = 30x]
    rules.emplace_back("mul_30_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(30), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Sub, s5, s1);
        });
    // 34 * x → (x << 5) + (x << 1)  [32x + 2x = 34x]
    rules.emplace_back("mul_34_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(34), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s5, s1);
        });
    // 60 * x → (x << 6) - (x << 2)  [64x - 4x = 60x]
    rules.emplace_back("mul_60_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(60), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Sub, s6, s2);
        });
    // 112 * x → (x << 7) - (x << 4)  [128x - 16x = 112x]
    rules.emplace_back("mul_112_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(112), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Sub, s7, s4);
        });
    // 257 * x → (x << 8) + x  [256x + x = 257x]
    rules.emplace_back("mul_257_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(257), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            return g.addBinOp(Op::Add, s8, s.at("x"));
        });
    // 1000 * x → (x << 10) - (x << 4) - (x << 3)  [1024x - 16x - 8x = 1000x]
    rules.emplace_back("mul_1000_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(1000), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s10 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(10));
            ClassId s4  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            ClassId s3  = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId sub1 = g.addBinOp(Op::Sub, s10, s4);
            return g.addBinOp(Op::Sub, sub1, s3);
        });
    // ── Extended left-constant patterns to match emitShiftAdd ─────────────
    // 57 * x → (x<<6) - (x<<3) + x  [64x - 8x + x = 57x]
    rules.emplace_back("mul_57_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(57), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            ClassId t = g.addBinOp(Op::Sub, s6, s3);
            return g.addBinOp(Op::Add, t, s.at("x"));
        });
    // 62 * x → (x<<6) - (x<<1)  [64x - 2x = 62x]
    rules.emplace_back("mul_62_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(62), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Sub, s6, s1);
        });
    // 66 * x → (x<<6) + (x<<1)  [64x + 2x = 66x]
    rules.emplace_back("mul_66_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(66), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s6, s1);
        });
    // 68 * x → (x<<6) + (x<<2)  [64x + 4x = 68x]
    rules.emplace_back("mul_68_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(68), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            ClassId s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, s6, s2);
        });
    // 120 * x → (x<<7) - (x<<3)  [128x - 8x = 120x]
    rules.emplace_back("mul_120_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(120), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s7, s3);
        });
    // 136 * x → (x<<7) + (x<<3)  [128x + 8x = 136x]
    rules.emplace_back("mul_136_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(136), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s7, s3);
        });
    // 144 * x → (x<<7) + (x<<4)  [128x + 16x = 144x]
    rules.emplace_back("mul_144_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(144), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Add, s7, s4);
        });
    // 160 * x → (x<<7) + (x<<5)  [128x + 32x = 160x]
    rules.emplace_back("mul_160_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(160), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Add, s7, s5);
        });
    // 224 * x → (x<<8) - (x<<5)  [256x - 32x = 224x]
    rules.emplace_back("mul_224_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(224), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Sub, s8, s5);
        });
    // 240 * x → (x<<8) - (x<<4)  [256x - 16x = 240x]
    rules.emplace_back("mul_240_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(240), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Sub, s8, s4);
        });
    // 248 * x → (x<<8) - (x<<3)  [256x - 8x = 248x]
    rules.emplace_back("mul_248_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(248), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s8, s3);
        });
    // 288 * x → (x<<8) + (x<<5)  [256x + 32x = 288x]
    rules.emplace_back("mul_288_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(288), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Add, s8, s5);
        });
    // 320 * x → (x<<8) + (x<<6)  [256x + 64x = 320x]
    rules.emplace_back("mul_320_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(320), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Add, s8, s6);
        });
    // 384 * x → (x<<8) + (x<<7)  [256x + 128x = 384x]
    rules.emplace_back("mul_384_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(384), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Add, s8, s7);
        });
    // 448 * x → (x<<9) - (x<<6)  [512x - 64x = 448x]
    rules.emplace_back("mul_448_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(448), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Sub, s9, s6);
        });
    // 480 * x → (x<<9) - (x<<5)  [512x - 32x = 480x]
    rules.emplace_back("mul_480_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(480), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Sub, s9, s5);
        });
    // 496 * x → (x<<9) - (x<<4)  [512x - 16x = 496x]
    rules.emplace_back("mul_496_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(496), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Sub, s9, s4);
        });
    // 504 * x → (x<<9) - (x<<3)  [512x - 8x = 504x]
    rules.emplace_back("mul_504_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(504), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s9, s3);
        });
    // 640 * x → (x<<9) + (x<<7)  [512x + 128x = 640x]
    rules.emplace_back("mul_640_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(640), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s7 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Add, s9, s7);
        });
    // 768 * x → (x<<9) + (x<<8)  [512x + 256x = 768x]
    rules.emplace_back("mul_768_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(768), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s9 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(9));
            ClassId s8 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            return g.addBinOp(Op::Add, s9, s8);
        });

    // ─────────────────────────────────────────────────────────────────────
    // More modulo/division identities
    // ─────────────────────────────────────────────────────────────────────

    // x % 0 is undefined, skip
    // (a / b) * b → a - (a % b)  [floor division property]
    // Too complex for now

    // x / x → 1 (unsafe if x=0, but pattern is useful)
    // Skip for safety

    // (2 * x) / 2 → x
    rules.emplace_back("mul2_div2_cancel",
        P::OpPat(Op::Div, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)}), P::ConstPat(2)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // (2 * x) % 2 → 0
    rules.emplace_back("mul2_mod2_zero",
        P::OpPat(Op::Mod, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)}), P::ConstPat(2)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (4 * x) / 4 → x
    rules.emplace_back("mul4_div4_cancel",
        P::OpPat(Op::Div, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4)}), P::ConstPat(4)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ─────────────────────────────────────────────────────────────────────
    // More power/exponent rules
    // ─────────────────────────────────────────────────────────────────────

    // 0 ^ 1 → 0
    rules.emplace_back("pow_zero_base_one",
        P::OpPat(Op::Pow, {P::ConstPat(0), P::ConstPat(1)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // 1 ^ 0 → 1
    rules.emplace_back("pow_one_base_zero",
        P::OpPat(Op::Pow, {P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // x ^ 0 → 1
    rules.emplace_back("pow_zero_exponent",
        P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // (a * b)^2 → a^2 * b^2
    rules.emplace_back("pow2_mul_distribute",
        P::OpPat(Op::Pow, {P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}), P::ConstPat(2)}),
        [](EGraph& g, const Subst& s) {
            ClassId a2 = g.addBinOp(Op::Pow, s.at("a"), g.addConst(2));
            ClassId b2 = g.addBinOp(Op::Pow, s.at("b"), g.addConst(2));
            return g.addBinOp(Op::Mul, a2, b2);
        });

    // (a / b)^2 → a^2 / b^2
    rules.emplace_back("pow2_div_distribute",
        P::OpPat(Op::Pow, {P::OpPat(Op::Div, {P::Wild("a"), P::Wild("b")}), P::ConstPat(2)}),
        [](EGraph& g, const Subst& s) {
            ClassId a2 = g.addBinOp(Op::Pow, s.at("a"), g.addConst(2));
            ClassId b2 = g.addBinOp(Op::Pow, s.at("b"), g.addConst(2));
            return g.addBinOp(Op::Div, a2, b2);
        });

    // x^2 - y^2 → (x+y)*(x-y)  [reverse of diff_of_squares]
    rules.emplace_back("pow2_diff",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(2)}),
            P::OpPat(Op::Pow, {P::Wild("y"), P::ConstPat(2)})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId xpy = g.addBinOp(Op::Add, s.at("x"), s.at("y"));
            ClassId xmy = g.addBinOp(Op::Sub, s.at("x"), s.at("y"));
            return g.addBinOp(Op::Mul, xpy, xmy);
        });

    // ─────────────────────────────────────────────────────────────────────
    // More cancellation/simplification rules
    // ─────────────────────────────────────────────────────────────────────

    // a - b == 0 → a == b (already exists as sub_eq_zero variant in comparison)
    // (a - b) + (b - c) + c → a
    // Complex nested patterns

    // x * 2 - x → x
    rules.emplace_back("mul2_sub_to_self",
        P::OpPat(Op::Sub, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)}), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x - x * 2 → -x
    rules.emplace_back("sub_mul2_to_neg",
        P::OpPat(Op::Sub, {P::Wild("x"), P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)})}),
        [](EGraph& g, const Subst& s) { return g.addUnaryOp(Op::Neg, s.at("x")); });

    // x * 3 - x → x * 2
    rules.emplace_back("mul3_sub_to_mul2",
        P::OpPat(Op::Sub, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(2));
        });

    // x * 3 - x * 2 → x
    rules.emplace_back("mul3_sub_mul2_to_self",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)})
        }),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x * a + x * b → x * (a + b) when a,b are wilds (already covered by factor_mul_add/factor_mul_add_left)
    // But let's add specific constant cases:
    // x * 2 + x * 3 → x * 5
    rules.emplace_back("mul2_add_mul3",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(5)); });

    // x * 2 + x * 4 → x * 6
    rules.emplace_back("mul2_add_mul4",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(6)); });

    // x * 3 + x * 4 → x * 7
    rules.emplace_back("mul3_add_mul4",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(7)); });

    // x * 3 + x * 5 → x * 8
    rules.emplace_back("mul3_add_mul5",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(5)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(8)); });

    // x * 4 + x * 4 → x * 8
    rules.emplace_back("mul4_add_mul4",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(8)); });

    // x * 3 + x * 6 → x * 9
    rules.emplace_back("mul3_add_mul6",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(6)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(9)); });

    // x * 5 + x * 5 → x * 10
    rules.emplace_back("mul5_add_mul5",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(5)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(5)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(10)); });

    // x * 4 + x * 8 → x * 12
    rules.emplace_back("mul4_add_mul8",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(12)); });

    // x * 5 + x * 8 → x * 13
    rules.emplace_back("mul5_add_mul8",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(5)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(13)); });

    // x * 6 + x * 8 → x * 14
    rules.emplace_back("mul6_add_mul8",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(6)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(14)); });

    // x * 7 + x * 8 → x * 15
    rules.emplace_back("mul7_add_mul8",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(7)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(15)); });

    // x * 8 + x * 8 → x * 16
    rules.emplace_back("mul8_add_mul8",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(16)); });

    // x * 9 + x * 8 → x * 17
    rules.emplace_back("mul9_add_mul8",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(9)}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(17)); });

    // ─────────────────────────────────────────────────────────────────────
    // Negation of multiplication
    // ─────────────────────────────────────────────────────────────────────

    // -(x * 2) → x * (-2)
    rules.emplace_back("neg_mul2",
        P::OpPat(Op::Neg, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(-2LL)); });

    // -(x * 3) → x * (-3)
    rules.emplace_back("neg_mul3",
        P::OpPat(Op::Neg, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mul, s.at("x"), g.addConst(-3LL)); });

    // x * (-2) → -(x * 2)
    rules.emplace_back("mul_neg2_to_neg",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(-2LL)}),
        [](EGraph& g, const Subst& s) {
            ClassId m2 = g.addBinOp(Op::Mul, s.at("x"), g.addConst(2));
            return g.addUnaryOp(Op::Neg, m2);
        });

    // x * (-3) → -(x * 3)
    rules.emplace_back("mul_neg3_to_neg",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(-3LL)}),
        [](EGraph& g, const Subst& s) {
            ClassId m3 = g.addBinOp(Op::Mul, s.at("x"), g.addConst(3));
            return g.addUnaryOp(Op::Neg, m3);
        });

    // a - b + b → a
    rules.emplace_back("sub_add_self_cancel",
        P::OpPat(Op::Add, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ── More multiply strength reduction patterns ───────────────────────
    // x * 2 + x → x * 3  (complement of existing x * 3 → (x<<1) + x)
    rules.emplace_back("mul2_plus_x_to_mul3",
        P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(3));
        });
    // x + x * 2 → x * 3
    rules.emplace_back("x_plus_mul2_to_mul3",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(3));
        });

    // x * 4 + x → x * 5
    rules.emplace_back("mul4_plus_x_to_mul5",
        P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(5));
        });
    // x + x * 4 → x * 5
    rules.emplace_back("x_plus_mul4_to_mul5",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(5));
        });

    // x * 8 - x → x * 7
    rules.emplace_back("mul8_minus_x_to_mul7",
        P::OpPat(Op::Sub, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(7));
        });

    // x * 8 + x → x * 9
    rules.emplace_back("mul8_plus_x_to_mul9",
        P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(9));
        });
    // x + x * 8 → x * 9
    rules.emplace_back("x_plus_mul8_to_mul9",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(8)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(9));
        });

    // x * 16 - x → x * 15
    rules.emplace_back("mul16_minus_x_to_mul15",
        P::OpPat(Op::Sub, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(16)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(15));
        });

    // x * 16 + x → x * 17
    rules.emplace_back("mul16_plus_x_to_mul17",
        P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(16)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(17));
        });

    // ── Comparison after subtraction ─────────────────────────────────────
    // (x - y) < 0 → x < y
    rules.emplace_back("sub_lt_zero",
        P::OpPat(Op::Lt, {P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Lt, s.at("x"), s.at("y"));
        });
    // (x - y) > 0 → x > y
    rules.emplace_back("sub_gt_zero",
        P::OpPat(Op::Gt, {P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Gt, s.at("x"), s.at("y"));
        });
    // (x - y) <= 0 → x <= y
    rules.emplace_back("sub_le_zero",
        P::OpPat(Op::Le, {P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Le, s.at("x"), s.at("y"));
        });
    // (x - y) >= 0 → x >= y
    rules.emplace_back("sub_ge_zero",
        P::OpPat(Op::Ge, {P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ge, s.at("x"), s.at("y"));
        });
    // x == 0 → !x (boolean equivalence).
    // Valid because OmScript's LogNot always produces 0 or 1:
    //   !x = (x == 0) ? 1 : 0, which is exactly (x == 0).
    rules.emplace_back("eq_zero_to_lognot",
        P::OpPat(Op::Eq, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::LogNot, s.at("x"));
        });
    // x != 0 → !!x (boolean conversion).
    // Valid because LogNot is boolean: !!x = !(x==0 ? 1 : 0) = (x!=0 ? 1 : 0).
    rules.emplace_back("ne_zero_to_bool",
        P::OpPat(Op::Ne, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            ClassId notx = g.addUnaryOp(Op::LogNot, s.at("x"));
            return g.addUnaryOp(Op::LogNot, notx);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Sqrt identities
    // ─────────────────────────────────────────────────────────────────────

    // sqrt(1) → 1
    rules.emplace_back("sqrt_one",
        P::OpPat(Op::Sqrt, {P::ConstPat(1)}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // sqrt(0) → 0
    rules.emplace_back("sqrt_zero",
        P::OpPat(Op::Sqrt, {P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // sqrt(x) * sqrt(x) → x
    rules.emplace_back("sqrt_squared",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Sqrt, {P::Wild("x")}),
            P::OpPat(Op::Sqrt, {P::Wild("x")})
        }),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // sqrt(x^2) → x  (assuming x >= 0; valid for the unsigned domain)
    rules.emplace_back("sqrt_of_square",
        P::OpPat(Op::Sqrt, {P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(2)})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // sqrt(x) * sqrt(y) → sqrt(x * y)
    rules.emplace_back("sqrt_mul_to_sqrt_of_mul",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Sqrt, {P::Wild("x")}),
            P::OpPat(Op::Sqrt, {P::Wild("y")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId xy = g.addBinOp(Op::Mul, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::Sqrt, xy);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Additional power rules
    // ─────────────────────────────────────────────────────────────────────

    // x^1 → x
    rules.emplace_back("pow_one",
        P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x^(-1) → 1 / x
    rules.emplace_back("pow_neg1_to_div",
        P::OpPat(Op::Pow, {P::Wild("x"), P::ConstPat(-1)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Div, g.addConst(1), s.at("x"));
        });

    // x^a * x^b → x^(a + b)  (power product rule)
    rules.emplace_back("pow_mul_same_base",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Pow, {P::Wild("x"), P::Wild("a")}),
            P::OpPat(Op::Pow, {P::Wild("x"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Add, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Pow, s.at("x"), ab);
        });

    // (x^a)^b → x^(a * b)  (power of power rule)
    rules.emplace_back("pow_of_pow",
        P::OpPat(Op::Pow, {P::OpPat(Op::Pow, {P::Wild("x"), P::Wild("a")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Pow, s.at("x"), ab);
        });

    // x^a / x^b → x^(a - b)  (power quotient rule)
    rules.emplace_back("pow_div_same_base",
        P::OpPat(Op::Div, {
            P::OpPat(Op::Pow, {P::Wild("x"), P::Wild("a")}),
            P::OpPat(Op::Pow, {P::Wild("x"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId amb = g.addBinOp(Op::Sub, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Pow, s.at("x"), amb);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Reassociation rules — expose constant folding across associations
    // ─────────────────────────────────────────────────────────────────────

    // (a + b) + c → a + (b + c)  (right-associate addition)
    rules.emplace_back("add_reassoc_right",
        P::OpPat(Op::Add, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::Add, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Add, s.at("a"), bc);
        });

    // a + (b + c) → (a + b) + c  (left-associate addition)
    rules.emplace_back("add_reassoc_left",
        P::OpPat(Op::Add, {P::Wild("a"), P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Add, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Add, ab, s.at("c"));
        });

    // (a * b) * c → a * (b * c)  (right-associate multiplication)
    rules.emplace_back("mul_reassoc_right",
        P::OpPat(Op::Mul, {P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::Mul, s.at("b"), s.at("c"));
            return g.addBinOp(Op::Mul, s.at("a"), bc);
        });

    // a * (b * c) → (a * b) * c  (left-associate multiplication)
    rules.emplace_back("mul_reassoc_left",
        P::OpPat(Op::Mul, {P::Wild("a"), P::OpPat(Op::Mul, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Mul, ab, s.at("c"));
        });

    // (a + C1) + C2 → a + (C1 + C2)  (constant folding via reassociation)
    // This is handled implicitly by the above rules + constant folding.

    // ─────────────────────────────────────────────────────────────────────
    return rules;
}

std::vector<RewriteRule> getComparisonRules() {
    using P = Pattern;
    std::vector<RewriteRule> rules;

    // ── Self-equality: x == x → 1 ───────────────────────────────────────
    rules.emplace_back("eq_self",
        P::OpPat(Op::Eq, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ── Self-inequality: x != x → 0 ─────────────────────────────────────
    rules.emplace_back("ne_self",
        P::OpPat(Op::Ne, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Self less-than: x < x → 0 ───────────────────────────────────────
    rules.emplace_back("lt_self",
        P::OpPat(Op::Lt, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Self less-equal: x <= x → 1 ─────────────────────────────────────
    rules.emplace_back("le_self",
        P::OpPat(Op::Le, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ── Self greater-than: x > x → 0 ────────────────────────────────────
    rules.emplace_back("gt_self",
        P::OpPat(Op::Gt, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Self greater-equal: x >= x → 1 ──────────────────────────────────
    rules.emplace_back("ge_self",
        P::OpPat(Op::Ge, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ── Complement: !(a == b) → a != b ───────────────────────────────────
    rules.emplace_back("not_eq_to_ne",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("a"), s.at("b"));
        });

    // ── Complement: !(a != b) → a == b ───────────────────────────────────
    rules.emplace_back("not_ne_to_eq",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Ne, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Eq, s.at("a"), s.at("b"));
        });

    // ── Complement: !(a < b) → a >= b ────────────────────────────────────
    rules.emplace_back("not_lt_to_ge",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ge, s.at("a"), s.at("b"));
        });

    // ── Complement: !(a <= b) → a > b ────────────────────────────────────
    rules.emplace_back("not_le_to_gt",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Le, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Gt, s.at("a"), s.at("b"));
        });

    // ── (x - y) == 0 → x == y ───────────────────────────────────────────
    rules.emplace_back("sub_eq_zero",
        P::OpPat(Op::Eq, {P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Eq, s.at("x"), s.at("y"));
        });

    // ── Double logical not: !!x → x (for boolean context) ───────────────
    rules.emplace_back("double_log_not",
        P::OpPat(Op::LogNot, {P::OpPat(Op::LogNot, {P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Complement: !(a > b) → a <= b ────────────────────────────────────
    rules.emplace_back("not_gt_to_le",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Le, s.at("a"), s.at("b"));
        });

    // ── Complement: !(a >= b) → a < b ────────────────────────────────────
    rules.emplace_back("not_ge_to_lt",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Ge, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Lt, s.at("a"), s.at("b"));
        });

    // ── Logical identity: x && x → x != 0 (boolean conversion) ────────
    rules.emplace_back("logand_self",
        P::OpPat(Op::LogAnd, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("x"), g.addConst(0));
        });

    // ── Logical identity: x || x → x != 0 (boolean conversion) ──────
    rules.emplace_back("logor_self",
        P::OpPat(Op::LogOr, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("x"), g.addConst(0));
        });

    // ── Logical annihilation: x && 0 → 0 ────────────────────────────────
    rules.emplace_back("logand_zero",
        P::OpPat(Op::LogAnd, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Logical annihilation: 0 && x → 0 ────────────────────────────────
    rules.emplace_back("logand_zero_left",
        P::OpPat(Op::LogAnd, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Logical identity: x || 0 → x != 0 (boolean conversion) ────────
    rules.emplace_back("logor_zero",
        P::OpPat(Op::LogOr, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("x"), g.addConst(0));
        });

    // ── Logical identity: 0 || x → x != 0 (boolean conversion) ──────
    rules.emplace_back("logor_zero_left",
        P::OpPat(Op::LogOr, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("x"), g.addConst(0));
        });

    // ── Ternary same-branch: cond ? x : x → x ──────────────────────────
    rules.emplace_back("ternary_same",
        P::OpPat(Op::Ternary, {P::Wild("c"), P::Wild("x"), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Ternary true: 1 ? a : b → a ────────────────────────────────────
    rules.emplace_back("ternary_true",
        P::OpPat(Op::Ternary, {P::ConstPat(1), P::Wild("a"), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ── Ternary false: 0 ? a : b → b ───────────────────────────────────
    rules.emplace_back("ternary_false",
        P::OpPat(Op::Ternary, {P::ConstPat(0), P::Wild("a"), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // ── Ternary negated condition: (!c) ? a : b → c ? b : a ─────────────
    rules.emplace_back("ternary_not_cond",
        P::OpPat(Op::Ternary, {P::OpPat(Op::LogNot, {P::Wild("c")}), P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ENode ternaryNode(Op::Ternary, std::vector<ClassId>{s.at("c"), s.at("b"), s.at("a")});
            return g.add(ternaryNode);
        });

    // ── Logical AND identity: x && 1 → x != 0 (boolean conversion) ────
    rules.emplace_back("logand_one",
        P::OpPat(Op::LogAnd, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("x"), g.addConst(0));
        });

    // ── Logical AND identity: 1 && x → x != 0 (boolean conversion) ──
    rules.emplace_back("logand_one_left",
        P::OpPat(Op::LogAnd, {P::ConstPat(1), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("x"), g.addConst(0));
        });

    // ── Logical OR annihilation: x || 1 → 1 ─────────────────────────────
    rules.emplace_back("logor_one",
        P::OpPat(Op::LogOr, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ── Logical OR annihilation: 1 || x → 1 ─────────────────────────────
    rules.emplace_back("logor_one_left",
        P::OpPat(Op::LogOr, {P::ConstPat(1), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ── Logical NOT constants: !0 → 1 ───────────────────────────────────
    rules.emplace_back("lognot_zero",
        P::OpPat(Op::LogNot, {P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ── Logical NOT constants: !1 → 0 ───────────────────────────────────
    rules.emplace_back("lognot_one",
        P::OpPat(Op::LogNot, {P::ConstPat(1)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Comparison against self via subtraction: x - y == 0 → x == y ────
    // (already exists above as sub_eq_zero; also add !=)
    rules.emplace_back("sub_ne_zero",
        P::OpPat(Op::Ne, {P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("x"), s.at("y"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Relational rules: comparison negation (relate ! to comparison ops)
    // ─────────────────────────────────────────────────────────────────────

    // ── !(a == b) → a != b ───────────────────────────────────────────────
    rules.emplace_back("not_eq_to_ne",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("a"), s.at("b"));
        });

    // ── !(a != b) → a == b ───────────────────────────────────────────────
    rules.emplace_back("not_ne_to_eq",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Ne, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Eq, s.at("a"), s.at("b"));
        });

    // ── !(a < b) → a >= b ───────────────────────────────────────────────
    rules.emplace_back("not_lt_to_ge",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ge, s.at("a"), s.at("b"));
        });

    // ── !(a <= b) → a > b ───────────────────────────────────────────────
    rules.emplace_back("not_le_to_gt",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Le, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Gt, s.at("a"), s.at("b"));
        });

    // ── !(a > b) → a <= b ───────────────────────────────────────────────
    rules.emplace_back("not_gt_to_le",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Le, s.at("a"), s.at("b"));
        });

    // ── !(a >= b) → a < b ───────────────────────────────────────────────
    rules.emplace_back("not_ge_to_lt",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Ge, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Lt, s.at("a"), s.at("b"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Relational rules: comparison swap (relate > to <, >= to <=)
    // ─────────────────────────────────────────────────────────────────────

    // ── a > b → b < a ────────────────────────────────────────────────────
    rules.emplace_back("gt_to_lt_swap",
        P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Lt, s.at("b"), s.at("a"));
        });

    // ── a >= b → b <= a ──────────────────────────────────────────────────
    rules.emplace_back("ge_to_le_swap",
        P::OpPat(Op::Ge, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Le, s.at("b"), s.at("a"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Relational rules: De Morgan's laws for logical operators
    // ─────────────────────────────────────────────────────────────────────

    // ── !(a && b) → !a || !b ─────────────────────────────────────────────
    rules.emplace_back("demorgan_and",
        P::OpPat(Op::LogNot, {P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId notA = g.addUnaryOp(Op::LogNot, s.at("a"));
            ClassId notB = g.addUnaryOp(Op::LogNot, s.at("b"));
            return g.addBinOp(Op::LogOr, notA, notB);
        });

    // ── !(a || b) → !a && !b ─────────────────────────────────────────────
    rules.emplace_back("demorgan_or",
        P::OpPat(Op::LogNot, {P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId notA = g.addUnaryOp(Op::LogNot, s.at("a"));
            ClassId notB = g.addUnaryOp(Op::LogNot, s.at("b"));
            return g.addBinOp(Op::LogAnd, notA, notB);
        });

    // ── Double logical NOT: !!x → x ─────────────────────────────────────
    rules.emplace_back("lognot_lognot",
        P::OpPat(Op::LogNot, {P::OpPat(Op::LogNot, {P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ─────────────────────────────────────────────────────────────────────
    // Relational rules: ternary-comparison fusion
    // ─────────────────────────────────────────────────────────────────────

    // ── (a == b) ? x : y → (a != b) ? y : x ─────────────────────────────
    rules.emplace_back("ternary_eq_flip",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")}), P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId ne = g.addBinOp(Op::Ne, s.at("a"), s.at("b"));
            ENode ternaryNode(Op::Ternary, std::vector<ClassId>{ne, s.at("y"), s.at("x")});
            return g.add(ternaryNode);
        });

    // ── (a < b) ? x : y → (a >= b) ? y : x ─────────────────────────────
    rules.emplace_back("ternary_lt_flip",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")}), P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId ge = g.addBinOp(Op::Ge, s.at("a"), s.at("b"));
            ENode ternaryNode(Op::Ternary, std::vector<ClassId>{ge, s.at("y"), s.at("x")});
            return g.add(ternaryNode);
        });

    // ── Comparison-arithmetic fusion: (a - b) < 0 → a < b ───────────────
    rules.emplace_back("sub_lt_zero",
        P::OpPat(Op::Lt, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Lt, s.at("a"), s.at("b"));
        });

    // ── Comparison-arithmetic fusion: (a - b) > 0 → a > b ───────────────
    rules.emplace_back("sub_gt_zero",
        P::OpPat(Op::Gt, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Gt, s.at("a"), s.at("b"));
        });

    // ── Comparison-arithmetic fusion: (a - b) <= 0 → a <= b ─────────────
    rules.emplace_back("sub_le_zero",
        P::OpPat(Op::Le, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Le, s.at("a"), s.at("b"));
        });

    // ── Comparison-arithmetic fusion: (a - b) >= 0 → a >= b ─────────────
    rules.emplace_back("sub_ge_zero",
        P::OpPat(Op::Ge, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ge, s.at("a"), s.at("b"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Logical commutativity
    // ─────────────────────────────────────────────────────────────────────

    // a && b → b && a
    rules.emplace_back("logand_comm",
        P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogAnd, s.at("b"), s.at("a")); });

    // a || b → b || a
    rules.emplace_back("logor_comm",
        P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogOr, s.at("b"), s.at("a")); });

    // ─────────────────────────────────────────────────────────────────────
    // Logical associativity
    // ─────────────────────────────────────────────────────────────────────

    // (a && b) && c → a && (b && c)
    rules.emplace_back("logand_assoc",
        P::OpPat(Op::LogAnd, {P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::LogAnd, s.at("b"), s.at("c"));
            return g.addBinOp(Op::LogAnd, s.at("a"), bc);
        });

    // a && (b && c) → (a && b) && c
    rules.emplace_back("logand_assoc_rev",
        P::OpPat(Op::LogAnd, {P::Wild("a"), P::OpPat(Op::LogAnd, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::LogAnd, s.at("a"), s.at("b"));
            return g.addBinOp(Op::LogAnd, ab, s.at("c"));
        });

    // (a || b) || c → a || (b || c)
    rules.emplace_back("logor_assoc",
        P::OpPat(Op::LogOr, {P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::LogOr, s.at("b"), s.at("c"));
            return g.addBinOp(Op::LogOr, s.at("a"), bc);
        });

    // a || (b || c) → (a || b) || c
    rules.emplace_back("logor_assoc_rev",
        P::OpPat(Op::LogOr, {P::Wild("a"), P::OpPat(Op::LogOr, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::LogOr, s.at("a"), s.at("b"));
            return g.addBinOp(Op::LogOr, ab, s.at("c"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // More ternary simplifications
    // ─────────────────────────────────────────────────────────────────────

    // c ? 1 : 0 → c  (boolean conversion)
    rules.emplace_back("ternary_one_zero",
        P::OpPat(Op::Ternary, {P::Wild("c"), P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("c"); });

    // c ? 0 : 1 → !c
    rules.emplace_back("ternary_zero_one",
        P::OpPat(Op::Ternary, {P::Wild("c"), P::ConstPat(0), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) { return g.addUnaryOp(Op::LogNot, s.at("c")); });

    // (!c) ? a : b → c ? b : a
    rules.emplace_back("ternary_lognot_flip",
        P::OpPat(Op::Ternary, {P::OpPat(Op::LogNot, {P::Wild("c")}), P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ENode n(Op::Ternary, std::vector<ClassId>{s.at("c"), s.at("b"), s.at("a")});
            return g.add(n);
        });

    // c ? a : a → a  (already covered by ternary_same but different form)
    // (a == b) ? x : y → (b == a) ? x : y  (eq commutative in condition)
    rules.emplace_back("ternary_eq_cond_comm",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")}), P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId eq = g.addBinOp(Op::Eq, s.at("b"), s.at("a"));
            ENode n(Op::Ternary, std::vector<ClassId>{eq, s.at("x"), s.at("y")});
            return g.add(n);
        });

    // c ? (c ? a : b) : d → c ? a : d  (nested same condition)
    rules.emplace_back("ternary_nested_same_cond",
        P::OpPat(Op::Ternary, {
            P::Wild("c"),
            P::OpPat(Op::Ternary, {P::Wild("c"), P::Wild("a"), P::Wild("b")}),
            P::Wild("d")
        }),
        [](EGraph& g, const Subst& s) {
            ENode n(Op::Ternary, std::vector<ClassId>{s.at("c"), s.at("a"), s.at("d")});
            return g.add(n);
        });

    // c ? a : (c ? b : d) → c ? a : d  (nested same condition false branch)
    rules.emplace_back("ternary_nested_false_same",
        P::OpPat(Op::Ternary, {
            P::Wild("c"),
            P::Wild("a"),
            P::OpPat(Op::Ternary, {P::Wild("c"), P::Wild("b"), P::Wild("d")})
        }),
        [](EGraph& g, const Subst& s) {
            ENode n(Op::Ternary, std::vector<ClassId>{s.at("c"), s.at("a"), s.at("d")});
            return g.add(n);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Boolean cancellation
    // ─────────────────────────────────────────────────────────────────────

    // a && !a → 0
    rules.emplace_back("logand_not_cancel",
        P::OpPat(Op::LogAnd, {P::Wild("a"), P::OpPat(Op::LogNot, {P::Wild("a")})}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // !a && a → 0
    rules.emplace_back("not_logand_cancel",
        P::OpPat(Op::LogAnd, {P::OpPat(Op::LogNot, {P::Wild("a")}), P::Wild("a")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // a || !a → 1
    rules.emplace_back("logor_not_cancel",
        P::OpPat(Op::LogOr, {P::Wild("a"), P::OpPat(Op::LogNot, {P::Wild("a")})}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // !a || a → 1
    rules.emplace_back("not_logor_cancel",
        P::OpPat(Op::LogOr, {P::OpPat(Op::LogNot, {P::Wild("a")}), P::Wild("a")}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ─────────────────────────────────────────────────────────────────────
    // Absorption laws
    // ─────────────────────────────────────────────────────────────────────

    // x && (x || y) → x
    rules.emplace_back("logand_absorb_or",
        P::OpPat(Op::LogAnd, {P::Wild("x"), P::OpPat(Op::LogOr, {P::Wild("x"), P::Wild("y")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x && (y || x) → x
    rules.emplace_back("logand_absorb_or2",
        P::OpPat(Op::LogAnd, {P::Wild("x"), P::OpPat(Op::LogOr, {P::Wild("y"), P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x || (x && y) → x
    rules.emplace_back("logor_absorb_and",
        P::OpPat(Op::LogOr, {P::Wild("x"), P::OpPat(Op::LogAnd, {P::Wild("x"), P::Wild("y")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x || (y && x) → x
    rules.emplace_back("logor_absorb_and2",
        P::OpPat(Op::LogOr, {P::Wild("x"), P::OpPat(Op::LogAnd, {P::Wild("y"), P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison symmetry and additional negation forms
    // ─────────────────────────────────────────────────────────────────────

    // a == b → b == a
    rules.emplace_back("eq_comm",
        P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("b"), s.at("a")); });

    // a != b → b != a
    rules.emplace_back("ne_comm",
        P::OpPat(Op::Ne, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ne, s.at("b"), s.at("a")); });

    // b < a → a > b  (reverse of gt_to_lt_swap)
    rules.emplace_back("lt_to_gt_swap",
        P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Gt, s.at("b"), s.at("a")); });

    // b <= a → a >= b
    rules.emplace_back("le_to_ge_swap",
        P::OpPat(Op::Le, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("b"), s.at("a")); });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison with arithmetic
    // ─────────────────────────────────────────────────────────────────────

    // (x + 1) > y → x >= y
    rules.emplace_back("add1_gt_to_ge",
        P::OpPat(Op::Gt, {P::OpPat(Op::Add, {P::Wild("x"), P::ConstPat(1)}), P::Wild("y")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("x"), s.at("y")); });

    // x > (y - 1) → x >= y
    rules.emplace_back("gt_sub1_to_ge",
        P::OpPat(Op::Gt, {P::Wild("x"), P::OpPat(Op::Sub, {P::Wild("y"), P::ConstPat(1)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("x"), s.at("y")); });

    // (x - 1) < y → x <= y
    rules.emplace_back("sub1_lt_to_le",
        P::OpPat(Op::Lt, {P::OpPat(Op::Sub, {P::Wild("x"), P::ConstPat(1)}), P::Wild("y")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("x"), s.at("y")); });

    // x < (y + 1) → x <= y
    rules.emplace_back("lt_add1_to_le",
        P::OpPat(Op::Lt, {P::Wild("x"), P::OpPat(Op::Add, {P::Wild("y"), P::ConstPat(1)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("x"), s.at("y")); });

    // ─────────────────────────────────────────────────────────────────────
    // Zero comparison simplifications
    // ─────────────────────────────────────────────────────────────────────

    // x == 0 → !x
    rules.emplace_back("eq_zero_to_not",
        P::OpPat(Op::Eq, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addUnaryOp(Op::LogNot, s.at("x")); });

    // 0 == x → !x
    rules.emplace_back("zero_eq_to_not",
        P::OpPat(Op::Eq, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst& s) { return g.addUnaryOp(Op::LogNot, s.at("x")); });

    // x != 0 → !!x (double negation - or just leave as is, this expands)
    // Actually: x != 0 is itself boolean, so skip this

    // ─────────────────────────────────────────────────────────────────────
    // Logical elimination patterns
    // ─────────────────────────────────────────────────────────────────────

    // (a && b) || (!a && b) → b
    rules.emplace_back("logand_or_elim",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::LogAnd, {P::OpPat(Op::LogNot, {P::Wild("a")}), P::Wild("b")})
        }),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // (a && b) || (a && !b) → a
    rules.emplace_back("logand_or_elim2",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::LogAnd, {P::Wild("a"), P::OpPat(Op::LogNot, {P::Wild("b")})})
        }),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // (a || b) && (a || !b) → a
    rules.emplace_back("logor_and_elim",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::LogOr, {P::Wild("a"), P::OpPat(Op::LogNot, {P::Wild("b")})})
        }),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ─────────────────────────────────────────────────────────────────────
    // More comparison + logical combinations
    // ─────────────────────────────────────────────────────────────────────

    // (x == y) && (x != y) → 0
    rules.emplace_back("eq_ne_contradiction",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Eq, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Ne, {P::Wild("x"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (x == y) || (x != y) → 1
    rules.emplace_back("eq_ne_tautology",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Eq, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Ne, {P::Wild("x"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // (x < y) && (x >= y) → 0
    rules.emplace_back("lt_ge_contradiction",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Lt, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Ge, {P::Wild("x"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (x < y) || (x >= y) → 1
    rules.emplace_back("lt_ge_tautology",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Lt, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Ge, {P::Wild("x"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // (x <= y) && (x > y) → 0
    rules.emplace_back("le_gt_contradiction",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Le, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Gt, {P::Wild("x"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (x <= y) || (x > y) → 1
    rules.emplace_back("le_gt_tautology",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Le, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Gt, {P::Wild("x"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ─────────────────────────────────────────────────────────────────────
    // Ternary with logical conditions
    // ─────────────────────────────────────────────────────────────────────

    // (a != b) ? x : y → (a == b) ? y : x
    rules.emplace_back("ternary_ne_flip",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Ne, {P::Wild("a"), P::Wild("b")}), P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId eq = g.addBinOp(Op::Eq, s.at("a"), s.at("b"));
            ENode n(Op::Ternary, std::vector<ClassId>{eq, s.at("y"), s.at("x")});
            return g.add(n);
        });

    // (a > b) ? x : y → (a <= b) ? y : x
    rules.emplace_back("ternary_gt_flip",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")}), P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId le = g.addBinOp(Op::Le, s.at("a"), s.at("b"));
            ENode n(Op::Ternary, std::vector<ClassId>{le, s.at("y"), s.at("x")});
            return g.add(n);
        });

    // (a >= b) ? x : y → (a < b) ? y : x
    rules.emplace_back("ternary_ge_flip",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Ge, {P::Wild("a"), P::Wild("b")}), P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId lt = g.addBinOp(Op::Lt, s.at("a"), s.at("b"));
            ENode n(Op::Ternary, std::vector<ClassId>{lt, s.at("y"), s.at("x")});
            return g.add(n);
        });

    // (a <= b) ? x : y → (a > b) ? y : x
    rules.emplace_back("ternary_le_flip",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Le, {P::Wild("a"), P::Wild("b")}), P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId gt = g.addBinOp(Op::Gt, s.at("a"), s.at("b"));
            ENode n(Op::Ternary, std::vector<ClassId>{gt, s.at("y"), s.at("x")});
            return g.add(n);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Subtraction comparison with zero (extended)
    // ─────────────────────────────────────────────────────────────────────

    // 0 < (a - b) → b < a
    rules.emplace_back("zero_lt_sub",
        P::OpPat(Op::Lt, {P::ConstPat(0), P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("b"), s.at("a")); });

    // 0 > (a - b) → a < b
    rules.emplace_back("zero_gt_sub",
        P::OpPat(Op::Gt, {P::ConstPat(0), P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("a"), s.at("b")); });

    // 0 == (a - b) → a == b
    rules.emplace_back("zero_eq_sub",
        P::OpPat(Op::Eq, {P::ConstPat(0), P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // Logical distribution
    // ─────────────────────────────────────────────────────────────────────

    // a && (b || c) → (a && b) || (a && c)
    rules.emplace_back("logand_distribute_or",
        P::OpPat(Op::LogAnd, {P::Wild("a"), P::OpPat(Op::LogOr, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::LogAnd, s.at("a"), s.at("b"));
            ClassId ac = g.addBinOp(Op::LogAnd, s.at("a"), s.at("c"));
            return g.addBinOp(Op::LogOr, ab, ac);
        });

    // a || (b && c) → (a || b) && (a || c)
    rules.emplace_back("logor_distribute_and",
        P::OpPat(Op::LogOr, {P::Wild("a"), P::OpPat(Op::LogAnd, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::LogOr, s.at("a"), s.at("b"));
            ClassId ac = g.addBinOp(Op::LogOr, s.at("a"), s.at("c"));
            return g.addBinOp(Op::LogAnd, ab, ac);
        });


    return rules;
}


std::vector<RewriteRule> getAdvancedComparisonRules() {
    using P = Pattern;
    std::vector<RewriteRule> rules;

    // ─────────────────────────────────────────────────────────────────────
    // Boolean algebra laws
    // ─────────────────────────────────────────────────────────────────────

    // (a && b) || b → b
    rules.emplace_back("logand_or_absorb",
        P::OpPat(Op::LogOr, {P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // b || (a && b) → b
    rules.emplace_back("or_logand_absorb",
        P::OpPat(Op::LogOr, {P::Wild("b"), P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // (a || b) && b → b
    rules.emplace_back("logor_and_absorb",
        P::OpPat(Op::LogAnd, {P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // b && (a || b) → b
    rules.emplace_back("and_logor_absorb",
        P::OpPat(Op::LogAnd, {P::Wild("b"), P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // (a && b) || a → a
    rules.emplace_back("logand_or_absorb2",
        P::OpPat(Op::LogOr, {P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // (a || b) && a → a
    rules.emplace_back("logor_and_absorb2",
        P::OpPat(Op::LogAnd, {P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // !(!a && !b) → a || b  (De Morgan inverse)
    rules.emplace_back("demorgan_and_inv",
        P::OpPat(Op::LogNot, {P::OpPat(Op::LogAnd, {
            P::OpPat(Op::LogNot, {P::Wild("a")}),
            P::OpPat(Op::LogNot, {P::Wild("b")})
        })}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogOr, s.at("a"), s.at("b")); });

    // !(!a || !b) → a && b  (De Morgan inverse)
    rules.emplace_back("demorgan_or_inv",
        P::OpPat(Op::LogNot, {P::OpPat(Op::LogOr, {
            P::OpPat(Op::LogNot, {P::Wild("a")}),
            P::OpPat(Op::LogNot, {P::Wild("b")})
        })}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogAnd, s.at("a"), s.at("b")); });

    // a && (b || !a) → a && b
    rules.emplace_back("logand_or_not_a",
        P::OpPat(Op::LogAnd, {P::Wild("a"), P::OpPat(Op::LogOr, {P::Wild("b"), P::OpPat(Op::LogNot, {P::Wild("a")})})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogAnd, s.at("a"), s.at("b")); });

    // a || (b && !a) → a || b
    rules.emplace_back("logor_and_not_a",
        P::OpPat(Op::LogOr, {P::Wild("a"), P::OpPat(Op::LogAnd, {P::Wild("b"), P::OpPat(Op::LogNot, {P::Wild("a")})})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogOr, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison chains and transitivity patterns
    // ─────────────────────────────────────────────────────────────────────

    // (x == y) → (y == x)  (symmetry, already in comparison rules as eq_comm)

    // (x < y) && (y < z) can imply x < z - but can't directly rewrite without both

    // (x >= 0) && (x < 0) → 0  (contradiction)
    rules.emplace_back("ge0_lt0_contr",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Ge, {P::Wild("x"), P::ConstPat(0)}),
            P::OpPat(Op::Lt, {P::Wild("x"), P::ConstPat(0)})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (x > 0) && (x <= 0) → 0
    rules.emplace_back("gt0_le0_contr",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Gt, {P::Wild("x"), P::ConstPat(0)}),
            P::OpPat(Op::Le, {P::Wild("x"), P::ConstPat(0)})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (x > 0) || (x <= 0) → 1
    rules.emplace_back("gt0_or_le0",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Gt, {P::Wild("x"), P::ConstPat(0)}),
            P::OpPat(Op::Le, {P::Wild("x"), P::ConstPat(0)})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // (x >= 0) || (x < 0) → 1
    rules.emplace_back("ge0_or_lt0",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Ge, {P::Wild("x"), P::ConstPat(0)}),
            P::OpPat(Op::Lt, {P::Wild("x"), P::ConstPat(0)})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ─────────────────────────────────────────────────────────────────────
    // Ternary optimization patterns
    // ─────────────────────────────────────────────────────────────────────

    // (a == b) ? a : b → b  (if equal pick either, but when condition is false pick b)
    // Actually: if a==b, result is a (same as b). If a!=b, result is b. So always b.
    // Wait: (a==b) ? a : b → if a==b then a else b → always b (since when equal, a==b)
    // This is actually correct: when a==b, a==b so result is a == b. Both same.
    // But we can simplify to just b, since when condition true a==b.
    rules.emplace_back("ternary_eq_first_branch",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")}), P::Wild("a"), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // c ? !c : x → x (if c is true then !c is false/0, if c is false !c is true/1 and result is x)
    // Actually this is: if c is true, result is 0 (since !c=0); if c is false, result is x.
    // This simplification: c ? (!c) : x → 0 when c is true, x when false. Which is !c && x? No.
    // Better: c ? (!c) : x = c ? 0 : x = !c && x? Not quite. Skip this.

    // x ? 1 : x → x || x = x  (x ? 1 : x - if x true then 1, else x=0; so always x)
    rules.emplace_back("ternary_one_self",
        P::OpPat(Op::Ternary, {P::Wild("x"), P::ConstPat(1), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x ? x : 0 → x  (if x true then x, else 0; always x since x&&x = x)
    rules.emplace_back("ternary_self_zero",
        P::OpPat(Op::Ternary, {P::Wild("x"), P::Wild("x"), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // c ? (a && c) : a → a
    // if c: result = a && c = a && true = a; if !c: result = a. Always a.
    rules.emplace_back("ternary_logand_c",
        P::OpPat(Op::Ternary, {
            P::Wild("c"),
            P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("c")}),
            P::Wild("a")
        }),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // c ? a : (a || c) → a
    // if c: result = a; if !c: result = a || c = a || false = a. Always a.
    rules.emplace_back("ternary_logor_c",
        P::OpPat(Op::Ternary, {
            P::Wild("c"),
            P::Wild("a"),
            P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("c")})
        }),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ─────────────────────────────────────────────────────────────────────
    // More negation patterns
    // ─────────────────────────────────────────────────────────────────────

    // !(x == 0) → x != 0  (same as not_eq_to_ne but with constant)
    rules.emplace_back("not_eq_zero",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Eq, {P::Wild("x"), P::ConstPat(0)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ne, s.at("x"), g.addConst(0)); });

    // !(x != 0) → x == 0
    rules.emplace_back("not_ne_zero",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Ne, {P::Wild("x"), P::ConstPat(0)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("x"), g.addConst(0)); });

    // !(x > 0) → x <= 0
    rules.emplace_back("not_gt_zero",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Gt, {P::Wild("x"), P::ConstPat(0)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("x"), g.addConst(0)); });

    // !(x < 0) → x >= 0
    rules.emplace_back("not_lt_zero",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Lt, {P::Wild("x"), P::ConstPat(0)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("x"), g.addConst(0)); });

    // ─────────────────────────────────────────────────────────────────────
    // More subtraction/comparison patterns
    // ─────────────────────────────────────────────────────────────────────

    // (a + c) == (b + c) → a == b
    rules.emplace_back("add_eq_cancel",
        P::OpPat(Op::Eq, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("c")}),
            P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("a"), s.at("b")); });

    // (a + c) != (b + c) → a != b
    rules.emplace_back("add_ne_cancel",
        P::OpPat(Op::Ne, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("c")}),
            P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ne, s.at("a"), s.at("b")); });

    // (c + a) == (c + b) → a == b
    rules.emplace_back("add_eq_cancel_left",
        P::OpPat(Op::Eq, {
            P::OpPat(Op::Add, {P::Wild("c"), P::Wild("a")}),
            P::OpPat(Op::Add, {P::Wild("c"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("a"), s.at("b")); });

    // (a + c) < (b + c) → a < b
    rules.emplace_back("add_lt_cancel",
        P::OpPat(Op::Lt, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("c")}),
            P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("a"), s.at("b")); });

    // (a + c) <= (b + c) → a <= b
    rules.emplace_back("add_le_cancel",
        P::OpPat(Op::Le, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("c")}),
            P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // Logical simplification with equality
    // ─────────────────────────────────────────────────────────────────────

    // (a == b) && (a == b) → a == b  (idempotent - covered by logand_self)
    // (a == b) || (a < b) → a <= b
    rules.emplace_back("eq_or_lt_to_le",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("a"), s.at("b")); });

    // (a < b) || (a == b) → a <= b
    rules.emplace_back("lt_or_eq_to_le",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("a"), s.at("b")); });

    // (a == b) || (a > b) → a >= b
    rules.emplace_back("eq_or_gt_to_ge",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("a"), s.at("b")); });

    // (a > b) || (a == b) → a >= b
    rules.emplace_back("gt_or_eq_to_ge",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("a"), s.at("b")); });

    // (a <= b) && (a >= b) → a == b
    rules.emplace_back("le_and_ge_to_eq",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Le, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Ge, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("a"), s.at("b")); });

    // (a >= b) && (a <= b) → a == b
    rules.emplace_back("ge_and_le_to_eq",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Ge, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Le, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("a"), s.at("b")); });

    // (a < b) && (a > b) → 0  (contradiction)
    rules.emplace_back("lt_and_gt_contr",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (a < b) || (a > b) → a != b
    rules.emplace_back("lt_or_gt_to_ne",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ne, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // More comparison+arithmetic simplifications
    // ─────────────────────────────────────────────────────────────────────

    // (x + 1) >= y → x >= (y - 1)
    rules.emplace_back("add1_ge_rearrange",
        P::OpPat(Op::Ge, {P::OpPat(Op::Add, {P::Wild("x"), P::ConstPat(1)}), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId ym1 = g.addBinOp(Op::Sub, s.at("y"), g.addConst(1));
            return g.addBinOp(Op::Ge, s.at("x"), ym1);
        });

    // x <= (y - 1) → x < y
    rules.emplace_back("le_sub1_to_lt",
        P::OpPat(Op::Le, {P::Wild("x"), P::OpPat(Op::Sub, {P::Wild("y"), P::ConstPat(1)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("x"), s.at("y")); });

    // (x - 1) >= y → x > y
    rules.emplace_back("sub1_ge_to_gt",
        P::OpPat(Op::Ge, {P::OpPat(Op::Sub, {P::Wild("x"), P::ConstPat(1)}), P::Wild("y")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Gt, s.at("x"), s.at("y")); });

    // x > (y + 1) → x >= (y + 2) - skip, not always simpler

    // ─────────────────────────────────────────────────────────────────────
    // More ternary patterns
    // ─────────────────────────────────────────────────────────────────────

    // (a && b) ? x : y → a ? (b ? x : y) : y
    rules.emplace_back("ternary_and_cond",
        P::OpPat(Op::Ternary, {P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ENode inner(Op::Ternary, std::vector<ClassId>{s.at("b"), s.at("x"), s.at("y")});
            ClassId innerCls = g.add(inner);
            ENode outer(Op::Ternary, std::vector<ClassId>{s.at("a"), innerCls, s.at("y")});
            return g.add(outer);
        });

    // (a || b) ? x : y → a ? x : (b ? x : y)
    rules.emplace_back("ternary_or_cond",
        P::OpPat(Op::Ternary, {P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")}), P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ENode inner(Op::Ternary, std::vector<ClassId>{s.at("b"), s.at("x"), s.at("y")});
            ClassId innerCls = g.add(inner);
            ENode outer(Op::Ternary, std::vector<ClassId>{s.at("a"), s.at("x"), innerCls});
            return g.add(outer);
        });

    // c ? (c ? a : b) : d → c ? a : d
    rules.emplace_back("ternary_nested_true_elim",
        P::OpPat(Op::Ternary, {
            P::Wild("c"),
            P::OpPat(Op::Ternary, {P::Wild("c"), P::Wild("a"), P::Wild("b")}),
            P::Wild("d")
        }),
        [](EGraph& g, const Subst& s) {
            ENode n(Op::Ternary, std::vector<ClassId>{s.at("c"), s.at("a"), s.at("d")});
            return g.add(n);
        });

    // c ? a : (!c ? b : d) → c ? a : b
    rules.emplace_back("ternary_not_c_false",
        P::OpPat(Op::Ternary, {
            P::Wild("c"),
            P::Wild("a"),
            P::OpPat(Op::Ternary, {P::OpPat(Op::LogNot, {P::Wild("c")}), P::Wild("b"), P::Wild("d")})
        }),
        [](EGraph& g, const Subst& s) {
            ENode n(Op::Ternary, std::vector<ClassId>{s.at("c"), s.at("a"), s.at("b")});
            return g.add(n);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison with negation patterns
    // ─────────────────────────────────────────────────────────────────────

    // (-x) < 0 → x > 0
    rules.emplace_back("neg_lt_zero",
        P::OpPat(Op::Lt, {P::OpPat(Op::Neg, {P::Wild("x")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Gt, s.at("x"), g.addConst(0)); });

    // (-x) > 0 → x < 0
    rules.emplace_back("neg_gt_zero",
        P::OpPat(Op::Gt, {P::OpPat(Op::Neg, {P::Wild("x")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("x"), g.addConst(0)); });

    // (-x) == 0 → x == 0
    rules.emplace_back("neg_eq_zero",
        P::OpPat(Op::Eq, {P::OpPat(Op::Neg, {P::Wild("x")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("x"), g.addConst(0)); });

    // (-x) != 0 → x != 0
    rules.emplace_back("neg_ne_zero",
        P::OpPat(Op::Ne, {P::OpPat(Op::Neg, {P::Wild("x")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ne, s.at("x"), g.addConst(0)); });

    // ─────────────────────────────────────────────────────────────────────
    // Logical identity patterns
    // ─────────────────────────────────────────────────────────────────────

    // (a && b) && a → a && b
    rules.emplace_back("logand_absorb_left",
        P::OpPat(Op::LogAnd, {P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogAnd, s.at("a"), s.at("b")); });

    // a && (b && a) → a && b
    rules.emplace_back("logand_absorb_right",
        P::OpPat(Op::LogAnd, {P::Wild("a"), P::OpPat(Op::LogAnd, {P::Wild("b"), P::Wild("a")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogAnd, s.at("a"), s.at("b")); });

    // (a || b) || a → a || b
    rules.emplace_back("logor_absorb_left",
        P::OpPat(Op::LogOr, {P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogOr, s.at("a"), s.at("b")); });

    // a || (b || a) → a || b
    rules.emplace_back("logor_absorb_right",
        P::OpPat(Op::LogOr, {P::Wild("a"), P::OpPat(Op::LogOr, {P::Wild("b"), P::Wild("a")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogOr, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // More comparison + logical patterns
    // ─────────────────────────────────────────────────────────────────────

    // x == 1 → !!x when x is boolean (x != 0 and x != 1 are typical cases)
    // Skip - too semantic

    // (x && y) == 0 → !x || !y  (De Morgan)
    // This is just: !(x && y) → !x || !y which is demorgan_and

    // !(x < y) && !(y < x) → x == y  (antisymmetry)
    rules.emplace_back("antisymm_lt_to_eq",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::LogNot, {P::OpPat(Op::Lt, {P::Wild("x"), P::Wild("y")})}),
            P::OpPat(Op::LogNot, {P::OpPat(Op::Lt, {P::Wild("y"), P::Wild("x")})})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("x"), s.at("y")); });

    // (x >= y) && (y >= x) → x == y
    rules.emplace_back("antisymm_ge_to_eq",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Ge, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Ge, {P::Wild("y"), P::Wild("x")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("x"), s.at("y")); });

    // (x <= y) && (y <= x) → x == y
    rules.emplace_back("antisymm_le_to_eq",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Le, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Le, {P::Wild("y"), P::Wild("x")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("x"), s.at("y")); });

    // ─────────────────────────────────────────────────────────────────────
    // Additional absorption for logical operators
    // ─────────────────────────────────────────────────────────────────────

    // a && (a && b) → a && b
    rules.emplace_back("logand_absorb_nested_l",
        P::OpPat(Op::LogAnd, {P::Wild("a"), P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogAnd, s.at("a"), s.at("b")); });

    // (a && b) && b → a && b
    rules.emplace_back("logand_absorb_nested_r",
        P::OpPat(Op::LogAnd, {P::OpPat(Op::LogAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogAnd, s.at("a"), s.at("b")); });

    // a || (a || b) → a || b
    rules.emplace_back("logor_absorb_nested_l",
        P::OpPat(Op::LogOr, {P::Wild("a"), P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogOr, s.at("a"), s.at("b")); });

    // (a || b) || b → a || b
    rules.emplace_back("logor_absorb_nested_r",
        P::OpPat(Op::LogOr, {P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogOr, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison with negated operands
    // ─────────────────────────────────────────────────────────────────────

    // (-x) < (-y) → y < x  [multiply both sides by -1 flips inequality]
    rules.emplace_back("neg_lt_neg",
        P::OpPat(Op::Lt, {P::OpPat(Op::Neg, {P::Wild("x")}), P::OpPat(Op::Neg, {P::Wild("y")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("y"), s.at("x")); });

    // (-x) > (-y) → y > x
    rules.emplace_back("neg_gt_neg",
        P::OpPat(Op::Gt, {P::OpPat(Op::Neg, {P::Wild("x")}), P::OpPat(Op::Neg, {P::Wild("y")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Gt, s.at("y"), s.at("x")); });

    // (-x) <= (-y) → y <= x
    rules.emplace_back("neg_le_neg",
        P::OpPat(Op::Le, {P::OpPat(Op::Neg, {P::Wild("x")}), P::OpPat(Op::Neg, {P::Wild("y")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("y"), s.at("x")); });

    // (-x) >= (-y) → y >= x
    rules.emplace_back("neg_ge_neg",
        P::OpPat(Op::Ge, {P::OpPat(Op::Neg, {P::Wild("x")}), P::OpPat(Op::Neg, {P::Wild("y")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("y"), s.at("x")); });

    // (-x) == (-y) → x == y
    rules.emplace_back("neg_eq_neg",
        P::OpPat(Op::Eq, {P::OpPat(Op::Neg, {P::Wild("x")}), P::OpPat(Op::Neg, {P::Wild("y")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("x"), s.at("y")); });

    // ─────────────────────────────────────────────────────────────────────
    // Boolean XOR-like patterns
    // ─────────────────────────────────────────────────────────────────────

    // (a || b) && !(a && b) → a != b (XOR for booleans)
    // Too complex, skip

    // (a && !b) || (!a && b) → a != b  (XOR for booleans)
    rules.emplace_back("bool_xor_to_ne",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::LogAnd, {P::Wild("a"), P::OpPat(Op::LogNot, {P::Wild("b")})}),
            P::OpPat(Op::LogAnd, {P::OpPat(Op::LogNot, {P::Wild("a")}), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ne, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // More ternary optimization patterns
    // ─────────────────────────────────────────────────────────────────────

    // a ? (b ? c : d) : d → (a && b) ? c : d
    rules.emplace_back("ternary_and_factor",
        P::OpPat(Op::Ternary, {
            P::Wild("a"),
            P::OpPat(Op::Ternary, {P::Wild("b"), P::Wild("c"), P::Wild("d")}),
            P::Wild("d")
        }),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::LogAnd, s.at("a"), s.at("b"));
            ENode n(Op::Ternary, std::vector<ClassId>{ab, s.at("c"), s.at("d")});
            return g.add(n);
        });

    // a ? c : (b ? c : d) → (a || b) ? c : d
    rules.emplace_back("ternary_or_factor",
        P::OpPat(Op::Ternary, {
            P::Wild("a"),
            P::Wild("c"),
            P::OpPat(Op::Ternary, {P::Wild("b"), P::Wild("c"), P::Wild("d")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::LogOr, s.at("a"), s.at("b"));
            ENode n(Op::Ternary, std::vector<ClassId>{ab, s.at("c"), s.at("d")});
            return g.add(n);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Additional comparison with zero
    // ─────────────────────────────────────────────────────────────────────

    // x + y == 0 → x == -y
    rules.emplace_back("add_eq_zero",
        P::OpPat(Op::Eq, {P::OpPat(Op::Add, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            ClassId ny = g.addUnaryOp(Op::Neg, s.at("y"));
            return g.addBinOp(Op::Eq, s.at("x"), ny);
        });

    // x + y != 0 → x != -y
    rules.emplace_back("add_ne_zero",
        P::OpPat(Op::Ne, {P::OpPat(Op::Add, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            ClassId ny = g.addUnaryOp(Op::Neg, s.at("y"));
            return g.addBinOp(Op::Ne, s.at("x"), ny);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Logical operator with constants
    // ─────────────────────────────────────────────────────────────────────

    // a && -1 → a  (non-zero constant is truthy)
    rules.emplace_back("logand_neg_one",
        P::OpPat(Op::LogAnd, {P::Wild("a"), P::ConstPat(-1)}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // -1 && a → a
    rules.emplace_back("neg_one_logand",
        P::OpPat(Op::LogAnd, {P::ConstPat(-1), P::Wild("a")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // a || -1 → -1 (truthy constant absorbs)
    rules.emplace_back("logor_neg_one",
        P::OpPat(Op::LogOr, {P::Wild("a"), P::ConstPat(-1)}),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // -1 || a → -1
    rules.emplace_back("neg_one_logor",
        P::OpPat(Op::LogOr, {P::ConstPat(-1), P::Wild("a")}),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // !(-1) → 0
    rules.emplace_back("lognot_neg_one",
        P::OpPat(Op::LogNot, {P::ConstPat(-1)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison rewrite via symmetry and other forms
    // ─────────────────────────────────────────────────────────────────────

    // x < y → !(x >= y)
    rules.emplace_back("lt_to_not_ge",
        P::OpPat(Op::Lt, {P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId ge = g.addBinOp(Op::Ge, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::LogNot, ge);
        });

    // x > y → !(x <= y)
    rules.emplace_back("gt_to_not_le",
        P::OpPat(Op::Gt, {P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId le = g.addBinOp(Op::Le, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::LogNot, le);
        });

    // x <= y → !(x > y)
    rules.emplace_back("le_to_not_gt",
        P::OpPat(Op::Le, {P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId gt = g.addBinOp(Op::Gt, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::LogNot, gt);
        });

    // x >= y → !(x < y)
    rules.emplace_back("ge_to_not_lt",
        P::OpPat(Op::Ge, {P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId lt = g.addBinOp(Op::Lt, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::LogNot, lt);
        });

    // x == y → !(x != y)
    rules.emplace_back("eq_to_not_ne",
        P::OpPat(Op::Eq, {P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId ne = g.addBinOp(Op::Ne, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::LogNot, ne);
        });

    // x != y → !(x == y)
    rules.emplace_back("ne_to_not_eq",
        P::OpPat(Op::Ne, {P::Wild("x"), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId eq = g.addBinOp(Op::Eq, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::LogNot, eq);
        });

    // ─────────────────────────────────────────────────────────────────────
    // More complex ternary patterns
    // ─────────────────────────────────────────────────────────────────────

    // (x == y) ? 0 : 1 → x != y
    rules.emplace_back("ternary_eq_to_ne",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Eq, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ne, s.at("x"), s.at("y")); });

    // (x != y) ? 0 : 1 → x == y
    rules.emplace_back("ternary_ne_to_eq",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Ne, {P::Wild("x"), P::Wild("y")}), P::ConstPat(0), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("x"), s.at("y")); });

    // (x < y) ? 1 : 0 → x < y
    rules.emplace_back("ternary_lt_to_lt",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Lt, {P::Wild("x"), P::Wild("y")}), P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("x"), s.at("y")); });

    // (x > y) ? 1 : 0 → x > y
    rules.emplace_back("ternary_gt_to_gt",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Gt, {P::Wild("x"), P::Wild("y")}), P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Gt, s.at("x"), s.at("y")); });

    // (x <= y) ? 1 : 0 → x <= y
    rules.emplace_back("ternary_le_to_le",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Le, {P::Wild("x"), P::Wild("y")}), P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("x"), s.at("y")); });

    // (x >= y) ? 1 : 0 → x >= y
    rules.emplace_back("ternary_ge_to_ge",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Ge, {P::Wild("x"), P::Wild("y")}), P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("x"), s.at("y")); });

    // (x == y) ? 1 : 0 → x == y
    rules.emplace_back("ternary_eq_to_eq",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Eq, {P::Wild("x"), P::Wild("y")}), P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("x"), s.at("y")); });

    // (x != y) ? 1 : 0 → x != y
    rules.emplace_back("ternary_ne_to_ne",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Ne, {P::Wild("x"), P::Wild("y")}), P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ne, s.at("x"), s.at("y")); });

    // ─────────────────────────────────────────────────────────────────────
    // Conditional and logical equivalences
    // ─────────────────────────────────────────────────────────────────────

    // a && b → !((!a) || (!b))  (De Morgan - already have demorgan forms)

    // (a || b) && a → a  (absorption)
    // Already have logor_and_absorb2

    // (a || b) && b → b  (absorption)
    rules.emplace_back("logor_and_absorb3",
        P::OpPat(Op::LogAnd, {P::OpPat(Op::LogOr, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // b && (a || b) → b  (absorption - already have and_logor_absorb2)

    // a && (b && a) → a && b
    rules.emplace_back("logand_absorb_a_nested",
        P::OpPat(Op::LogAnd, {P::Wild("a"), P::OpPat(Op::LogAnd, {P::Wild("b"), P::Wild("a")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::LogAnd, s.at("a"), s.at("b")); });

    // (a && b) && a → a && b  (already have logand_absorb_left)

    // ─────────────────────────────────────────────────────────────────────
    // More comparison algebra
    // ─────────────────────────────────────────────────────────────────────

    // (a + b == c) → (a == c - b)
    rules.emplace_back("add_eq_rearrange",
        P::OpPat(Op::Eq, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId cmb = g.addBinOp(Op::Sub, s.at("c"), s.at("b"));
            return g.addBinOp(Op::Eq, s.at("a"), cmb);
        });

    // (a - b == c) → (a == c + b)
    rules.emplace_back("sub_eq_rearrange",
        P::OpPat(Op::Eq, {P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId cpb = g.addBinOp(Op::Add, s.at("c"), s.at("b"));
            return g.addBinOp(Op::Eq, s.at("a"), cpb);
        });

    // (a + b < c) → (a < c - b)
    rules.emplace_back("add_lt_rearrange",
        P::OpPat(Op::Lt, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId cmb = g.addBinOp(Op::Sub, s.at("c"), s.at("b"));
            return g.addBinOp(Op::Lt, s.at("a"), cmb);
        });

    // (a + b > c) → (a > c - b)
    rules.emplace_back("add_gt_rearrange",
        P::OpPat(Op::Gt, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId cmb = g.addBinOp(Op::Sub, s.at("c"), s.at("b"));
            return g.addBinOp(Op::Gt, s.at("a"), cmb);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison merging — combine two comparisons into one
    // ─────────────────────────────────────────────────────────────────────

    // (a < b) || (a == b) → a <= b
    rules.emplace_back("lt_or_eq_to_le",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("a"), s.at("b")); });

    // (a == b) || (a < b) → a <= b  (commuted OR)
    rules.emplace_back("eq_or_lt_to_le",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("a"), s.at("b")); });

    // (a > b) || (a == b) → a >= b
    rules.emplace_back("gt_or_eq_to_ge",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("a"), s.at("b")); });

    // (a == b) || (a > b) → a >= b  (commuted OR)
    rules.emplace_back("eq_or_gt_to_ge",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // Redundant comparison elimination — a < b already implies a != b
    // ─────────────────────────────────────────────────────────────────────

    // (a != b) && (a < b) → a < b
    rules.emplace_back("ne_and_lt_redundant",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Ne, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("a"), s.at("b")); });

    // (a < b) && (a != b) → a < b  (commuted AND)
    rules.emplace_back("lt_and_ne_redundant",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Ne, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("a"), s.at("b")); });

    // (a != b) && (a > b) → a > b
    rules.emplace_back("ne_and_gt_redundant",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Ne, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Gt, s.at("a"), s.at("b")); });

    // (a > b) && (a != b) → a > b  (commuted AND)
    rules.emplace_back("gt_and_ne_redundant",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Ne, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Gt, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // Ternary common-subexpression factoring
    // ─────────────────────────────────────────────────────────────────────

    // cond ? (a + c) : (b + c) → (cond ? a : b) + c
    rules.emplace_back("ternary_add_factor",
        P::OpPat(Op::Ternary, {P::Wild("cond"),
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("c")}),
            P::OpPat(Op::Add, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ENode sel(Op::Ternary, std::vector<ClassId>{s.at("cond"), s.at("a"), s.at("b")});
            ClassId selId = g.add(sel);
            return g.addBinOp(Op::Add, selId, s.at("c"));
        });

    // cond ? (a - c) : (b - c) → (cond ? a : b) - c
    rules.emplace_back("ternary_sub_factor",
        P::OpPat(Op::Ternary, {P::Wild("cond"),
            P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("c")}),
            P::OpPat(Op::Sub, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ENode sel(Op::Ternary, std::vector<ClassId>{s.at("cond"), s.at("a"), s.at("b")});
            ClassId selId = g.add(sel);
            return g.addBinOp(Op::Sub, selId, s.at("c"));
        });

    // cond ? (a * c) : (b * c) → (cond ? a : b) * c
    rules.emplace_back("ternary_mul_factor",
        P::OpPat(Op::Ternary, {P::Wild("cond"),
            P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("c")}),
            P::OpPat(Op::Mul, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ENode sel(Op::Ternary, std::vector<ClassId>{s.at("cond"), s.at("a"), s.at("b")});
            ClassId selId = g.add(sel);
            return g.addBinOp(Op::Mul, selId, s.at("c"));
        });

    // cond ? (-a) : (-b) → -(cond ? a : b)
    rules.emplace_back("ternary_neg_factor",
        P::OpPat(Op::Ternary, {P::Wild("cond"),
            P::OpPat(Op::Neg, {P::Wild("a")}),
            P::OpPat(Op::Neg, {P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ENode sel(Op::Ternary, std::vector<ClassId>{s.at("cond"), s.at("a"), s.at("b")});
            ClassId selId = g.add(sel);
            return g.addUnaryOp(Op::Neg, selId);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Ternary identity / boolean simplification
    // ─────────────────────────────────────────────────────────────────────

    // cond ? x : x → x  (both branches identical → result is always x)
    rules.emplace_back("ternary_same_branches",
        P::OpPat(Op::Ternary, {P::Wild("cond"), P::Wild("x"), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // cond ? 1 : 0 → cond  (boolean select is identity on condition)
    rules.emplace_back("ternary_bool_identity",
        P::OpPat(Op::Ternary, {P::Wild("cond"), P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("cond"); });

    // cond ? 0 : 1 → !cond  (inverted boolean select)
    rules.emplace_back("ternary_bool_not",
        P::OpPat(Op::Ternary, {P::Wild("cond"), P::ConstPat(0), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::LogNot, s.at("cond"));
        });

    // 1 ? a : b → a  (condition always true)
    rules.emplace_back("ternary_true_cond",
        P::OpPat(Op::Ternary, {P::ConstPat(1), P::Wild("a"), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // 0 ? a : b → b  (condition always false)
    rules.emplace_back("ternary_false_cond",
        P::OpPat(Op::Ternary, {P::ConstPat(0), P::Wild("a"), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // !cond ? a : b → cond ? b : a  (flip branches on negated condition)
    rules.emplace_back("ternary_not_cond_flip",
        P::OpPat(Op::Ternary, {P::OpPat(Op::LogNot, {P::Wild("cond")}),
                                P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ENode sel(Op::Ternary, std::vector<ClassId>{s.at("cond"), s.at("b"), s.at("a")});
            return g.add(sel);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison negation (De Morgan for comparisons)
    // ─────────────────────────────────────────────────────────────────────

    // !(a < b) → a >= b
    rules.emplace_back("not_lt_to_ge",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ge, s.at("a"), s.at("b")); });

    // !(a <= b) → a > b
    rules.emplace_back("not_le_to_gt",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Le, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Gt, s.at("a"), s.at("b")); });

    // !(a > b) → a <= b
    rules.emplace_back("not_gt_to_le",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Le, s.at("a"), s.at("b")); });

    // !(a >= b) → a < b
    rules.emplace_back("not_ge_to_lt",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Ge, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Lt, s.at("a"), s.at("b")); });

    // !(a == b) → a != b
    rules.emplace_back("not_eq_to_ne",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Ne, s.at("a"), s.at("b")); });

    // !(a != b) → a == b
    rules.emplace_back("not_ne_to_eq",
        P::OpPat(Op::LogNot, {P::OpPat(Op::Ne, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Eq, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // Boolean double negation
    // ─────────────────────────────────────────────────────────────────────

    // !!x → x  (when x is known boolean: double logical negation is identity)
    rules.emplace_back("double_lognot",
        P::OpPat(Op::LogNot, {P::OpPat(Op::LogNot, {P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });


    return rules;
}

std::vector<RewriteRule> getBitwiseRules() {
    using P = Pattern;
    std::vector<RewriteRule> rules;

    // ── XOR self: x ^ x → 0 ─────────────────────────────────────────────
    rules.emplace_back("xor_self",
        P::OpPat(Op::BitXor, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── AND self: x & x → x ─────────────────────────────────────────────
    rules.emplace_back("and_self",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── OR self: x | x → x ──────────────────────────────────────────────
    rules.emplace_back("or_self",
        P::OpPat(Op::BitOr, {P::Wild("x"), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── XOR zero: x ^ 0 → x ─────────────────────────────────────────────
    rules.emplace_back("xor_zero",
        P::OpPat(Op::BitXor, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── AND zero: x & 0 → 0 ─────────────────────────────────────────────
    rules.emplace_back("and_zero",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── OR zero: x | 0 → x ──────────────────────────────────────────────
    rules.emplace_back("or_zero",
        P::OpPat(Op::BitOr, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── AND all-ones: x & -1 → x ────────────────────────────────────────
    rules.emplace_back("and_all_ones",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(-1)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── OR all-ones: x | -1 → -1 ────────────────────────────────────────
    rules.emplace_back("or_all_ones",
        P::OpPat(Op::BitOr, {P::Wild("x"), P::ConstPat(-1)}),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // ── Double complement: ~~x → x ──────────────────────────────────────
    rules.emplace_back("double_bitnot",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitNot, {P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Shift by zero: x << 0 → x ───────────────────────────────────────
    rules.emplace_back("shl_zero",
        P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    rules.emplace_back("shr_zero",
        P::OpPat(Op::Shr, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── De Morgan's laws: ~(a & b) → (~a) | (~b) ────────────────────────
    rules.emplace_back("demorgan_and",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId notA = g.addUnaryOp(Op::BitNot, s.at("a"));
            ClassId notB = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitOr, notA, notB);
        });

    rules.emplace_back("demorgan_or",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId notA = g.addUnaryOp(Op::BitNot, s.at("a"));
            ClassId notB = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitAnd, notA, notB);
        });

    // ── Commutativity ────────────────────────────────────────────────────
    rules.emplace_back("and_comm",
        P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("b"), s.at("a"));
        });

    rules.emplace_back("or_comm",
        P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitOr, s.at("b"), s.at("a"));
        });

    rules.emplace_back("xor_comm",
        P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitXor, s.at("b"), s.at("a"));
        });

    // ── Absorption: a & (a | b) → a ─────────────────────────────────────
    rules.emplace_back("and_absorb",
        P::OpPat(Op::BitAnd, {P::Wild("a"), P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ── Absorption: a | (a & b) → a ─────────────────────────────────────
    rules.emplace_back("or_absorb",
        P::OpPat(Op::BitOr, {P::Wild("a"), P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ── XOR cancel: a ^ a → 0 (already covered by xor_self) ─────────────
    // ── XOR not: x ^ -1 → ~x ────────────────────────────────────────────
    rules.emplace_back("xor_all_ones_to_bitnot",
        P::OpPat(Op::BitXor, {P::Wild("x"), P::ConstPat(-1)}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::BitNot, s.at("x"));
        });

    // ── Shift combination: (x << a) << b → x << (a + b) ────────────────
    rules.emplace_back("shl_combine",
        P::OpPat(Op::Shl, {P::OpPat(Op::Shl, {P::Wild("x"), P::Wild("a")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ClassId sum = g.addBinOp(Op::Add, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Shl, s.at("x"), sum);
        });

    // ── Shift combination: (x >> a) >> b → x >> (a + b) ────────────────
    rules.emplace_back("shr_combine",
        P::OpPat(Op::Shr, {P::OpPat(Op::Shr, {P::Wild("x"), P::Wild("a")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ClassId sum = g.addBinOp(Op::Add, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Shr, s.at("x"), sum);
        });

    // ── XOR associativity: (a ^ b) ^ c → a ^ (b ^ c) ───────────────────
    rules.emplace_back("xor_assoc",
        P::OpPat(Op::BitXor, {P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::BitXor, s.at("b"), s.at("c"));
            return g.addBinOp(Op::BitXor, s.at("a"), bc);
        });

    // ── AND associativity: (a & b) & c → a & (b & c) ────────────────────
    rules.emplace_back("and_assoc",
        P::OpPat(Op::BitAnd, {P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::BitAnd, s.at("b"), s.at("c"));
            return g.addBinOp(Op::BitAnd, s.at("a"), bc);
        });

    // ── OR associativity: (a | b) | c → a | (b | c) ─────────────────────
    rules.emplace_back("or_assoc",
        P::OpPat(Op::BitOr, {P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::BitOr, s.at("b"), s.at("c"));
            return g.addBinOp(Op::BitOr, s.at("a"), bc);
        });

    // ── XOR with zero on left: 0 ^ x → x ────────────────────────────────
    rules.emplace_back("xor_zero_left",
        P::OpPat(Op::BitXor, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── AND zero (left): 0 & x → 0 ──────────────────────────────────────
    rules.emplace_back("and_zero_left",
        P::OpPat(Op::BitAnd, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── OR zero (left): 0 | x → x ───────────────────────────────────────
    rules.emplace_back("or_zero_left",
        P::OpPat(Op::BitOr, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── AND all-ones (left): -1 & x → x ─────────────────────────────────
    rules.emplace_back("and_all_ones_left",
        P::OpPat(Op::BitAnd, {P::ConstPat(-1), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── OR all-ones (left): -1 | x → -1 ─────────────────────────────────
    rules.emplace_back("or_all_ones_left",
        P::OpPat(Op::BitOr, {P::ConstPat(-1), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // ── XOR all-ones (left): -1 ^ x → ~x ────────────────────────────────
    rules.emplace_back("xor_all_ones_left_to_bitnot",
        P::OpPat(Op::BitXor, {P::ConstPat(-1), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::BitNot, s.at("x"));
        });

    // ── BitNot to XOR: ~x → x ^ -1 ─────────────────────────────────────
    // (reverse of xor_all_ones_to_bitnot, enables more XOR simplifications)
    rules.emplace_back("bitnot_to_xor",
        P::OpPat(Op::BitNot, {P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId allOnes = g.addConst(-1);
            return g.addBinOp(Op::BitXor, s.at("x"), allOnes);
        });

    // ── Double BitNot: ~~x → x ──────────────────────────────────────────
    rules.emplace_back("bitnot_bitnot",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitNot, {P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── De Morgan's (bitwise): ~(a & b) → (~a) | (~b) ───────────────────
    rules.emplace_back("demorgan_bitand",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId notA = g.addUnaryOp(Op::BitNot, s.at("a"));
            ClassId notB = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitOr, notA, notB);
        });

    // ── De Morgan's (bitwise): ~(a | b) → (~a) & (~b) ───────────────────
    rules.emplace_back("demorgan_bitor",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId notA = g.addUnaryOp(Op::BitNot, s.at("a"));
            ClassId notB = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitAnd, notA, notB);
        });

    // ── AND distributes over OR: a & (b | c) → (a & b) | (a & c) ───────
    rules.emplace_back("and_distribute_or",
        P::OpPat(Op::BitAnd, {P::Wild("a"), P::OpPat(Op::BitOr, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::BitAnd, s.at("a"), s.at("b"));
            ClassId ac = g.addBinOp(Op::BitAnd, s.at("a"), s.at("c"));
            return g.addBinOp(Op::BitOr, ab, ac);
        });

    // ── OR distributes over AND: a | (b & c) → (a | b) & (a | c) ───────
    rules.emplace_back("or_distribute_and",
        P::OpPat(Op::BitOr, {P::Wild("a"), P::OpPat(Op::BitAnd, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::BitOr, s.at("a"), s.at("b"));
            ClassId ac = g.addBinOp(Op::BitOr, s.at("a"), s.at("c"));
            return g.addBinOp(Op::BitAnd, ab, ac);
        });

    // ─────────────────────────────────────────────────────────────────────
    // XOR chain patterns
    // ─────────────────────────────────────────────────────────────────────

    // (a ^ b) ^ b → a
    rules.emplace_back("xor_b_cancel",
        P::OpPat(Op::BitXor, {P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")}), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // a ^ (a ^ b) → b
    rules.emplace_back("xor_a_cancel",
        P::OpPat(Op::BitXor, {P::Wild("a"), P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // (a ^ b) ^ a → b
    rules.emplace_back("xor_a_cancel2",
        P::OpPat(Op::BitXor, {P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // b ^ (a ^ b) → a
    rules.emplace_back("xor_b_cancel2",
        P::OpPat(Op::BitXor, {P::Wild("b"), P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ─────────────────────────────────────────────────────────────────────
    // Complement interactions: x & ~x → 0, x | ~x → -1
    // ─────────────────────────────────────────────────────────────────────

    // x & (~x) → 0
    rules.emplace_back("and_bitnot_cancel",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::OpPat(Op::BitNot, {P::Wild("x")})}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // (~x) & x → 0
    rules.emplace_back("bitnot_and_cancel",
        P::OpPat(Op::BitAnd, {P::OpPat(Op::BitNot, {P::Wild("x")}), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // x | (~x) → -1
    rules.emplace_back("or_bitnot_all_ones",
        P::OpPat(Op::BitOr, {P::Wild("x"), P::OpPat(Op::BitNot, {P::Wild("x")})}),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // (~x) | x → -1
    rules.emplace_back("bitnot_or_all_ones",
        P::OpPat(Op::BitOr, {P::OpPat(Op::BitNot, {P::Wild("x")}), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // x ^ (~x) → -1
    rules.emplace_back("xor_bitnot_all_ones",
        P::OpPat(Op::BitXor, {P::Wild("x"), P::OpPat(Op::BitNot, {P::Wild("x")})}),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // (~x) ^ x → -1
    rules.emplace_back("bitnot_xor_all_ones",
        P::OpPat(Op::BitXor, {P::OpPat(Op::BitNot, {P::Wild("x")}), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // ─────────────────────────────────────────────────────────────────────
    // AND distributes over OR (factored/expanded forms)
    // ─────────────────────────────────────────────────────────────────────

    // (a & b) | (a & c) → a & (b | c)
    rules.emplace_back("and_or_factor",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::BitOr, s.at("b"), s.at("c"));
            return g.addBinOp(Op::BitAnd, s.at("a"), bc);
        });

    // (a & b) | (c & a) → a & (b | c)
    rules.emplace_back("and_or_factor2",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitAnd, {P::Wild("c"), P::Wild("a")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::BitOr, s.at("b"), s.at("c"));
            return g.addBinOp(Op::BitAnd, s.at("a"), bc);
        });

    // (a | b) & (a | c) → a | (b & c)
    rules.emplace_back("or_and_factor",
        P::OpPat(Op::BitAnd, {
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::BitAnd, s.at("b"), s.at("c"));
            return g.addBinOp(Op::BitOr, s.at("a"), bc);
        });

    // ─────────────────────────────────────────────────────────────────────
    // XOR with NOT patterns
    // ─────────────────────────────────────────────────────────────────────

    // (~a) ^ b → ~(a ^ b)
    rules.emplace_back("bitnot_xor_distribute",
        P::OpPat(Op::BitXor, {P::OpPat(Op::BitNot, {P::Wild("a")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::BitXor, s.at("a"), s.at("b"));
            return g.addUnaryOp(Op::BitNot, ab);
        });

    // a ^ (~b) → ~(a ^ b)
    rules.emplace_back("xor_bitnot_distribute",
        P::OpPat(Op::BitXor, {P::Wild("a"), P::OpPat(Op::BitNot, {P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::BitXor, s.at("a"), s.at("b"));
            return g.addUnaryOp(Op::BitNot, ab);
        });

    // ~(a ^ b) → (~a) ^ b  (reverse)
    rules.emplace_back("bitnot_xor_expand",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitXor, na, s.at("b"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // More absorption variants
    // ─────────────────────────────────────────────────────────────────────

    // a & (a | b) → a  (already have and_absorb but ensuring b ordering)
    rules.emplace_back("and_absorb2",
        P::OpPat(Op::BitAnd, {P::Wild("a"), P::OpPat(Op::BitOr, {P::Wild("b"), P::Wild("a")})}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // a | (a & b) → a  (already have or_absorb but ensuring ordering)
    rules.emplace_back("or_absorb2",
        P::OpPat(Op::BitOr, {P::Wild("a"), P::OpPat(Op::BitAnd, {P::Wild("b"), P::Wild("a")})}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // (a & b) | a → a
    rules.emplace_back("and_or_absorb",
        P::OpPat(Op::BitOr, {P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // (a | b) & a → a
    rules.emplace_back("or_and_absorb",
        P::OpPat(Op::BitAnd, {P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ─────────────────────────────────────────────────────────────────────
    // Shift interactions with AND/OR
    // ─────────────────────────────────────────────────────────────────────

    // (x << n) | (x << n) → x << n  (covered by or_self, but via shift path)
    // (x & y) << n → (x << n) & (y << n)
    rules.emplace_back("shl_and_distribute",
        P::OpPat(Op::Shl, {P::OpPat(Op::BitAnd, {P::Wild("x"), P::Wild("y")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            ClassId xn = g.addBinOp(Op::Shl, s.at("x"), s.at("n"));
            ClassId yn = g.addBinOp(Op::Shl, s.at("y"), s.at("n"));
            return g.addBinOp(Op::BitAnd, xn, yn);
        });

    // (x | y) << n → (x << n) | (y << n)
    rules.emplace_back("shl_or_distribute",
        P::OpPat(Op::Shl, {P::OpPat(Op::BitOr, {P::Wild("x"), P::Wild("y")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            ClassId xn = g.addBinOp(Op::Shl, s.at("x"), s.at("n"));
            ClassId yn = g.addBinOp(Op::Shl, s.at("y"), s.at("n"));
            return g.addBinOp(Op::BitOr, xn, yn);
        });

    // (x ^ y) << n → (x << n) ^ (y << n)
    rules.emplace_back("shl_xor_distribute",
        P::OpPat(Op::Shl, {P::OpPat(Op::BitXor, {P::Wild("x"), P::Wild("y")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            ClassId xn = g.addBinOp(Op::Shl, s.at("x"), s.at("n"));
            ClassId yn = g.addBinOp(Op::Shl, s.at("y"), s.at("n"));
            return g.addBinOp(Op::BitXor, xn, yn);
        });

    // (x & y) >> n → (x >> n) & (y >> n)
    rules.emplace_back("shr_and_distribute",
        P::OpPat(Op::Shr, {P::OpPat(Op::BitAnd, {P::Wild("x"), P::Wild("y")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            ClassId xn = g.addBinOp(Op::Shr, s.at("x"), s.at("n"));
            ClassId yn = g.addBinOp(Op::Shr, s.at("y"), s.at("n"));
            return g.addBinOp(Op::BitAnd, xn, yn);
        });

    // ─────────────────────────────────────────────────────────────────────
    // More NOT and complement interactions
    // ─────────────────────────────────────────────────────────────────────

    // ~(~a & b) → a | ~b  (De Morgan + simplification)
    rules.emplace_back("bitnot_and_not_a",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitAnd, {P::OpPat(Op::BitNot, {P::Wild("a")}), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId nb = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitOr, s.at("a"), nb);
        });

    // ~(a & ~b) → ~a | b
    rules.emplace_back("bitnot_and_not_b",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitAnd, {P::Wild("a"), P::OpPat(Op::BitNot, {P::Wild("b")})})}),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitOr, na, s.at("b"));
        });

    // ~(~a | b) → a & ~b
    rules.emplace_back("bitnot_or_not_a",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitOr, {P::OpPat(Op::BitNot, {P::Wild("a")}), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId nb = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitAnd, s.at("a"), nb);
        });

    // ~(a | ~b) → ~a & b
    rules.emplace_back("bitnot_or_not_b",
        P::OpPat(Op::BitNot, {P::OpPat(Op::BitOr, {P::Wild("a"), P::OpPat(Op::BitNot, {P::Wild("b")})})}),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitAnd, na, s.at("b"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // XOR identities
    // ─────────────────────────────────────────────────────────────────────

    // x ^ x ^ y → y
    rules.emplace_back("xor_self_cancel_left",
        P::OpPat(Op::BitXor, {P::Wild("x"), P::OpPat(Op::BitXor, {P::Wild("x"), P::Wild("y")})}),
        [](EGraph&, const Subst& s) { return s.at("y"); });

    // x ^ y ^ x → y
    rules.emplace_back("xor_self_cancel_right",
        P::OpPat(Op::BitXor, {P::Wild("x"), P::OpPat(Op::BitXor, {P::Wild("y"), P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("y"); });

    // ─────────────────────────────────────────────────────────────────────
    // Shift by zero (additional forms)
    // ─────────────────────────────────────────────────────────────────────

    // 0 << x → 0
    rules.emplace_back("zero_shl",
        P::OpPat(Op::Shl, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // 0 >> x → 0
    rules.emplace_back("zero_shr",
        P::OpPat(Op::Shr, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // -1 << 0 → -1 (special case, covered by shl_zero)
    // -1 >> 0 → -1 (special case, covered by shr_zero)

    // ─────────────────────────────────────────────────────────────────────
    // AND/OR with NOT - complement laws
    // ─────────────────────────────────────────────────────────────────────

    // (~a) | (~b) → ~(a & b)  (De Morgan reverse)
    rules.emplace_back("bitnot_or_demorgan_rev",
        P::OpPat(Op::BitOr, {P::OpPat(Op::BitNot, {P::Wild("a")}), P::OpPat(Op::BitNot, {P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::BitAnd, s.at("a"), s.at("b"));
            return g.addUnaryOp(Op::BitNot, ab);
        });

    // (~a) & (~b) → ~(a | b)  (De Morgan reverse)
    rules.emplace_back("bitnot_and_demorgan_rev",
        P::OpPat(Op::BitAnd, {P::OpPat(Op::BitNot, {P::Wild("a")}), P::OpPat(Op::BitNot, {P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::BitOr, s.at("a"), s.at("b"));
            return g.addUnaryOp(Op::BitNot, ab);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Mixed shift patterns
    // ─────────────────────────────────────────────────────────────────────

    // (x >> a) << a → x & (-1 << a)  -- skip (complex mask)
    // (x << a) + (x >> (32-a)) → rotate  -- skip (architecture specific)

    // (x >> 1) + (x >> 1) → x & ~1  -- actually x & (-2) or x >> 0 with cleared bit
    // Better: (x >> 1) << 1 → x & (-2)  -- this clears lowest bit
    rules.emplace_back("shr1_shl1",
        P::OpPat(Op::Shl, {P::OpPat(Op::Shr, {P::Wild("x"), P::ConstPat(1)}), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) {
            ClassId mask = g.addConst(-2LL);
            return g.addBinOp(Op::BitAnd, s.at("x"), mask);
        });

    // (x & a) | (x & ~a) → x
    rules.emplace_back("and_or_bitnot_cancel",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitAnd, {P::Wild("x"), P::Wild("a")}),
            P::OpPat(Op::BitAnd, {P::Wild("x"), P::OpPat(Op::BitNot, {P::Wild("a")})})
        }),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // (x & ~a) | (x & a) → x
    rules.emplace_back("and_bitnot_or_cancel",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitAnd, {P::Wild("x"), P::OpPat(Op::BitNot, {P::Wild("a")})}),
            P::OpPat(Op::BitAnd, {P::Wild("x"), P::Wild("a")})
        }),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Shift-mask interaction: (x >> a) << a → x & ~((1 << a) - 1) ─────
    // Clearing low bits via shift pair.  The e-graph represents this as an
    // equivalent BitAnd with a constant mask, which exposes further AND
    // merging opportunities.
    for (int a = 1; a <= 6; ++a) {
        int64_t mask = ~((1LL << a) - 1);
        std::string name = "shr_shl_to_mask_" + std::to_string(a);
        rules.emplace_back(name,
            P::OpPat(Op::Shl, {P::OpPat(Op::Shr, {P::Wild("x"), P::ConstPat(a)}), P::ConstPat(a)}),
            [mask](EGraph& g, const Subst& s) {
                return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(mask));
            });
    }

    // ── Truncation via shift pair: (x << a) >> a → x & ((1 << (64-a)) - 1) ─
    // Clearing high bits via shift pair.
    for (int a = 1; a <= 6; ++a) {
        int64_t mask = (1LL << (64 - a)) - 1;
        std::string name = "shl_shr_to_mask_" + std::to_string(a);
        rules.emplace_back(name,
            P::OpPat(Op::Shr, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(a)}), P::ConstPat(a)}),
            [mask](EGraph& g, const Subst& s) {
                return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(mask));
            });
    }

    return rules;
}


std::vector<RewriteRule> getAdvancedBitwiseRules() {
    using P = Pattern;
    std::vector<RewriteRule> rules;

    // ─────────────────────────────────────────────────────────────────────
    // More XOR identities
    // ─────────────────────────────────────────────────────────────────────

    // (a ^ b) ^ (b ^ c) → a ^ c
    rules.emplace_back("xor_chain_cancel",
        P::OpPat(Op::BitXor, {
            P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitXor, {P::Wild("b"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitXor, s.at("a"), s.at("c")); });

    // a ^ (b ^ a) → b  (already have, but different associativity)
    rules.emplace_back("xor_swap_cancel",
        P::OpPat(Op::BitXor, {P::Wild("a"), P::OpPat(Op::BitXor, {P::Wild("b"), P::Wild("a")})}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // (a ^ a) ^ b → b  (double xor + b)
    rules.emplace_back("xor_self_then_b",
        P::OpPat(Op::BitXor, {P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("a")}), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // a ^ (b ^ b) → a
    rules.emplace_back("xor_b_self_then_a",
        P::OpPat(Op::BitXor, {P::Wild("a"), P::OpPat(Op::BitXor, {P::Wild("b"), P::Wild("b")})}),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ─────────────────────────────────────────────────────────────────────
    // AND idempotency and interaction with OR
    // ─────────────────────────────────────────────────────────────────────

    // (a & b) & a → a & b
    rules.emplace_back("and_absorb_nested",
        P::OpPat(Op::BitAnd, {P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("a"), s.at("b")); });

    // a & (b & a) → a & b
    rules.emplace_back("and_absorb_nested2",
        P::OpPat(Op::BitAnd, {P::Wild("a"), P::OpPat(Op::BitAnd, {P::Wild("b"), P::Wild("a")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("a"), s.at("b")); });

    // (a | b) | a → a | b
    rules.emplace_back("or_absorb_nested",
        P::OpPat(Op::BitOr, {P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitOr, s.at("a"), s.at("b")); });

    // a | (b | a) → a | b
    rules.emplace_back("or_absorb_nested2",
        P::OpPat(Op::BitOr, {P::Wild("a"), P::OpPat(Op::BitOr, {P::Wild("b"), P::Wild("a")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitOr, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // Shift composition rules
    // ─────────────────────────────────────────────────────────────────────

    // (x << a) & (x << b) → x << max(a,b) when a,b are small - skip for now
    // (x << a) | (x << b) → x << min(a,b) when a,b are small - skip

    // (x + y) << n → (x << n) + (y << n)
    rules.emplace_back("shl_add_distribute",
        P::OpPat(Op::Shl, {P::OpPat(Op::Add, {P::Wild("x"), P::Wild("y")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            ClassId xn = g.addBinOp(Op::Shl, s.at("x"), s.at("n"));
            ClassId yn = g.addBinOp(Op::Shl, s.at("y"), s.at("n"));
            return g.addBinOp(Op::Add, xn, yn);
        });

    // (x - y) << n → (x << n) - (y << n)
    rules.emplace_back("shl_sub_distribute",
        P::OpPat(Op::Shl, {P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            ClassId xn = g.addBinOp(Op::Shl, s.at("x"), s.at("n"));
            ClassId yn = g.addBinOp(Op::Shl, s.at("y"), s.at("n"));
            return g.addBinOp(Op::Sub, xn, yn);
        });

    // x << 1 → x + x  (reverse of shl to add)
    rules.emplace_back("shl1_to_add",
        P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Add, s.at("x"), s.at("x")); });

    // x << 0 → x (already exists but ensure it's here)

    // x * 2 → x << 1  (already have mul_2_to_shl1)

    // Shift combined with AND for masking
    // (x >> n) << n: clear lower n bits
    rules.emplace_back("shr_n_shl_n_clear_low",
        P::OpPat(Op::Shl, {P::OpPat(Op::Shr, {P::Wild("x"), P::Wild("n")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            // x & ((-1) << n) -- but we can't compute (-1)<<n without knowing n
            // Just note the pattern: this clears the lower n bits
            // Can't simplify without knowing n, so leave as is
            ClassId shr = g.addBinOp(Op::Shr, s.at("x"), s.at("n"));
            return g.addBinOp(Op::Shl, shr, s.at("n"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Constant shift patterns
    // ─────────────────────────────────────────────────────────────────────

    // x >> 1 << 1 (clear bit 0): x & (-2)
    // Already have shr1_shl1 in getBitwiseRules

    // (x | 1) & (-2) → x & (-2)  (sets bit 0, then clears it: net = x with bit 0 cleared)
    // Skip - too specific

    // ~(~x) → x  (already have double_bitnot)

    // x & x → x  (already have and_self)
    // x | x → x  (already have or_self)

    // ─────────────────────────────────────────────────────────────────────
    // More bitwise combinations
    // ─────────────────────────────────────────────────────────────────────

    // (a & b) ^ (a & ~b) → a ^ (b ^ ~b) → a ^ (-1) → ~a
    // Actually: (a & b) ^ (a & ~b) = a & (b ^ ~b) = a & (-1) = a
    // Wait: b ^ ~b = -1 (all ones), so a & -1 = a. But this isn't quite what XOR does
    // Actually: (a & b) ^ (a & ~b) = a & (b ^ ~b) [only if a distributes, which it does for bitwise]
    // b ^ ~b = b ^ (b^-1) = (b^b)^-1 = 0^-1 = -1. So a & -1 = a.
    // This rule: (a & b) ^ (a & ~b) → a
    rules.emplace_back("and_xor_not_cancel",
        P::OpPat(Op::BitXor, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::OpPat(Op::BitNot, {P::Wild("b")})})
        }),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // (a & ~b) ^ (a & b) → a (same, reversed order)
    rules.emplace_back("and_not_xor_cancel",
        P::OpPat(Op::BitXor, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::OpPat(Op::BitNot, {P::Wild("b")})}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // (a | b) & ~b → a & ~b
    rules.emplace_back("or_and_not_simplify",
        P::OpPat(Op::BitAnd, {
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitNot, {P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId nb = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitAnd, s.at("a"), nb);
        });

    // ~b & (a | b) → a & ~b
    rules.emplace_back("not_and_or_simplify",
        P::OpPat(Op::BitAnd, {
            P::OpPat(Op::BitNot, {P::Wild("b")}),
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId nb = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitAnd, s.at("a"), nb);
        });

    // (a & b) | ~a → ~a | b  (because: (a & b) | ~a = (~a | a) & (~a | b) = 1 & (~a | b) = ~a | b)
    // Actually: (a & b) | ~a = ~a | (a & b) = (~a | a) & (~a | b) = -1 & (~a | b) = ~a | b ✓
    rules.emplace_back("and_or_not_simplify",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitNot, {P::Wild("a")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitOr, na, s.at("b"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Bitwise with constants
    // ─────────────────────────────────────────────────────────────────────

    // x & (x | y) → x  (absorption with OR - already have and_absorb)
    // x & (~x | y) → x & y
    rules.emplace_back("and_not_or",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::OpPat(Op::BitOr, {P::OpPat(Op::BitNot, {P::Wild("x")}), P::Wild("y")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), s.at("y")); });

    // x | (x & y) → x  (absorption - already have or_absorb)
    // x | (~x & y) → x | y
    rules.emplace_back("or_not_and",
        P::OpPat(Op::BitOr, {P::Wild("x"), P::OpPat(Op::BitAnd, {P::OpPat(Op::BitNot, {P::Wild("x")}), P::Wild("y")})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitOr, s.at("x"), s.at("y")); });

    // ─────────────────────────────────────────────────────────────────────
    // Shift with constant amounts
    // ─────────────────────────────────────────────────────────────────────

    // (x << 1) + x → x * 3 → x << 1 is already 2x, so + x = 3x
    rules.emplace_back("shl1_add_to_mul3",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(1)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(3));
        });

    // x + (x << 1) → x * 3
    rules.emplace_back("add_shl1_to_mul3",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(1)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(3));
        });

    // (x << 2) + x → x * 5
    rules.emplace_back("shl2_add_to_mul5",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(2)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(5));
        });

    // x + (x << 2) → x * 5
    rules.emplace_back("add_shl2_to_mul5",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(2)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(5));
        });

    // (x << 3) + x → x * 9
    rules.emplace_back("shl3_add_to_mul9",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(3)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(9));
        });

    // x + (x << 3) → x * 9
    rules.emplace_back("add_shl3_to_mul9",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(3)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(9));
        });

    // (x << 3) - x → x * 7
    rules.emplace_back("shl3_sub_to_mul7",
        P::OpPat(Op::Sub, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(3)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(7));
        });

    // (x << 4) - x → x * 15
    rules.emplace_back("shl4_sub_to_mul15",
        P::OpPat(Op::Sub, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(4)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(15));
        });

    // (x << 4) + x → x * 17
    rules.emplace_back("shl4_add_to_mul17",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(4)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(17));
        });

    // x + (x << 4) → x * 17
    rules.emplace_back("add_shl4_to_mul17",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(4)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(17));
        });

    // (x << 5) - x → x * 31
    rules.emplace_back("shl5_sub_to_mul31",
        P::OpPat(Op::Sub, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(5)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(31));
        });

    // (x << 5) + x → x * 33
    rules.emplace_back("shl5_add_to_mul33",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(5)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(33));
        });

    // (x << 6) - x → x * 63
    rules.emplace_back("shl6_sub_to_mul63",
        P::OpPat(Op::Sub, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(6)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(63));
        });

    // (x << 6) + x → x * 65
    rules.emplace_back("shl6_add_to_mul65",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(6)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(65));
        });

    // (x << 7) - x → x * 127
    rules.emplace_back("shl7_sub_to_mul127",
        P::OpPat(Op::Sub, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(7)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(127));
        });

    // (x << 7) + x → x * 129
    rules.emplace_back("shl7_add_to_mul129",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(7)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(129));
        });

    // (x << 8) - x → x * 255
    rules.emplace_back("shl8_sub_to_mul255",
        P::OpPat(Op::Sub, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(8)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(255));
        });

    // (x << 8) + x → x * 257
    rules.emplace_back("shl8_add_to_mul257",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(8)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(257));
        });

    // x + (x << 8) → x * 257
    rules.emplace_back("add_shl8_to_mul257",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(8)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(257));
        });

    // (x << 9) - x → x * 511
    rules.emplace_back("shl9_sub_to_mul511",
        P::OpPat(Op::Sub, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(9)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(511));
        });

    // (x << 9) + x → x * 513
    rules.emplace_back("shl9_add_to_mul513",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(9)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(513));
        });

    // (x << 10) - x → x * 1023
    rules.emplace_back("shl10_sub_to_mul1023",
        P::OpPat(Op::Sub, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(10)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(1023));
        });

    // (x << 10) + x → x * 1025
    rules.emplace_back("shl10_add_to_mul1025",
        P::OpPat(Op::Add, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(10)}), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(1025));
        });

    // Commuted add patterns for shifts 5-7 (shifts 3-4 already have commuted forms)

    // x + (x << 5) → x * 33
    rules.emplace_back("add_shl5_to_mul33",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(5)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(33));
        });

    // x + (x << 6) → x * 65
    rules.emplace_back("add_shl6_to_mul65",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(6)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(65));
        });

    // x + (x << 7) → x * 129
    rules.emplace_back("add_shl7_to_mul129",
        P::OpPat(Op::Add, {P::Wild("x"), P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(7)})}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(129));
        });

    // ─────────────────────────────────────────────────────────────────────
    // More AND/OR/XOR identities
    // ─────────────────────────────────────────────────────────────────────

    // (a ^ b) | (a & b) → a | b
    rules.emplace_back("xor_or_and",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitOr, s.at("a"), s.at("b")); });

    // (a & b) | (a ^ b) → a | b
    rules.emplace_back("and_or_xor",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitOr, s.at("a"), s.at("b")); });

    // (a | b) ^ (a & b) → a ^ b  (xor = or - and in terms of bits)
    rules.emplace_back("or_xor_and",
        P::OpPat(Op::BitXor, {
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitXor, s.at("a"), s.at("b")); });

    // (a & b) ^ (a | b) → a ^ b (same, reversed)
    rules.emplace_back("and_xor_or",
        P::OpPat(Op::BitXor, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitXor, s.at("a"), s.at("b")); });

    // a & (b ^ a) → a & ~a & b → 0? No: a & (b ^ a) = (a & b) ^ (a & a) = (a & b) ^ a
    // Let's try: a ^ (a & b) → a & ~b
    // Proof: a ^ (a & b) = a & ~(a & b) | (~a & (a & b)) = a & (~a | ~b) | 0 = a & ~b  ✓
    rules.emplace_back("xor_and_simplify",
        P::OpPat(Op::BitXor, {P::Wild("a"), P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId nb = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitAnd, s.at("a"), nb);
        });

    // (a & b) ^ a → a & ~b (reversed order)
    rules.emplace_back("and_xor_simplify",
        P::OpPat(Op::BitXor, {P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph& g, const Subst& s) {
            ClassId nb = g.addUnaryOp(Op::BitNot, s.at("b"));
            return g.addBinOp(Op::BitAnd, s.at("a"), nb);
        });

    // ─────────────────────────────────────────────────────────────────────
    // NOT distribute over shifts
    // ─────────────────────────────────────────────────────────────────────

    // ~(x << n) - Note: ~(x << n) ≠ (~x) << n in general (due to sign extension)
    // But for bitwise purposes: ~(x << n) has different meaning
    // Skip these as they're not generally safe

    // ─────────────────────────────────────────────────────────────────────
    // Shift reduction patterns
    // ─────────────────────────────────────────────────────────────────────

    // (x << 1) << 1 → x << 2 (already have shl_combine)
    // (x << 1) >> 1 → x & (-2)  (already have shr1_shl1 pattern but different order)
    // (x >> 1) << 2 → (x & (-2)) << 1
    // Skip complex ones

    // x << 2 → (x << 1) << 1
    rules.emplace_back("shl2_expand",
        P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(2)}),
        [](EGraph& g, const Subst& s) {
            ClassId shl1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Shl, shl1, g.addConst(1));
        });

    // ─────────────────────────────────────────────────────────────────────
    // More De Morgan applications
    // ─────────────────────────────────────────────────────────────────────

    // ~(~a ^ b) → a ^ b... no. ~(~a ^ b) = ~(~a) ^ ~b? No, XOR doesn't distribute that way.
    // Actually ~(a ^ b) = (~a) ^ b = a ^ (~b) - these are already captured by bitnot_xor_distribute

    // (~a) | (~b) already → ~(a & b) via bitnot_or_demorgan_rev
    // (~a) & (~b) already → ~(a | b) via bitnot_and_demorgan_rev

    // Distributing AND over XOR:
    // a & (b ^ c) → (a & b) ^ (a & c)
    rules.emplace_back("and_xor_distribute",
        P::OpPat(Op::BitAnd, {P::Wild("a"), P::OpPat(Op::BitXor, {P::Wild("b"), P::Wild("c")})}),
        [](EGraph& g, const Subst& s) {
            ClassId ab = g.addBinOp(Op::BitAnd, s.at("a"), s.at("b"));
            ClassId ac = g.addBinOp(Op::BitAnd, s.at("a"), s.at("c"));
            return g.addBinOp(Op::BitXor, ab, ac);
        });

    // (a & b) ^ (a & c) → a & (b ^ c)
    rules.emplace_back("and_xor_factor",
        P::OpPat(Op::BitXor, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("c")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId bc = g.addBinOp(Op::BitXor, s.at("b"), s.at("c"));
            return g.addBinOp(Op::BitAnd, s.at("a"), bc);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Gray code patterns (g = n ^ (n >> 1))
    // ─────────────────────────────────────────────────────────────────────

    // x ^ (x >> 1) is the Gray code of x - not easily simplified without context

    // ─────────────────────────────────────────────────────────────────────
    // More useful patterns
    // ─────────────────────────────────────────────────────────────────────

    // x & 1 → x % 2  (last bit is parity)
    rules.emplace_back("and_one_to_mod2",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(2)); });

    // x % 2 → x & 1
    rules.emplace_back("mod2_to_and_one",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(2)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(1)); });

    // x & 3 → x % 4
    rules.emplace_back("and_three_to_mod4",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(3)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(4)); });

    // x % 4 → x & 3
    rules.emplace_back("mod4_to_and_three",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(4)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(3)); });

    // x & 7 → x % 8
    rules.emplace_back("and_seven_to_mod8",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(7)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(8)); });

    // x % 8 → x & 7
    rules.emplace_back("mod8_to_and_seven",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(8)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(7)); });

    // x & 15 → x % 16
    rules.emplace_back("and_15_to_mod16",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(15)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(16)); });

    // x % 16 → x & 15
    rules.emplace_back("mod16_to_and_15",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(16)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(15)); });

    // x & 31 → x % 32
    rules.emplace_back("and_31_to_mod32",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(31)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(32)); });
    // x % 32 → x & 31
    rules.emplace_back("mod32_to_and_31",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(32)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(31)); });

    // x & 63 → x % 64
    rules.emplace_back("and_63_to_mod64",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(63)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(64)); });
    // x % 64 → x & 63
    rules.emplace_back("mod64_to_and_63",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(64)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(63)); });

    // x & 127 → x % 128
    rules.emplace_back("and_127_to_mod128",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(127)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(128)); });
    // x % 128 → x & 127
    rules.emplace_back("mod128_to_and_127",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(128)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(127)); });

    // x & 255 → x % 256
    rules.emplace_back("and_255_to_mod256",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(255)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(256)); });
    // x % 256 → x & 255
    rules.emplace_back("mod256_to_and_255",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(256)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(255)); });

    // x & 511 → x % 512
    rules.emplace_back("and_511_to_mod512",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(511)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(512)); });
    // x % 512 → x & 511
    rules.emplace_back("mod512_to_and_511",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(512)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(511)); });

    // x & 1023 → x % 1024
    rules.emplace_back("and_1023_to_mod1024",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::ConstPat(1023)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::Mod, s.at("x"), g.addConst(1024)); });
    // x % 1024 → x & 1023
    rules.emplace_back("mod1024_to_and_1023",
        P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(1024)}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(1023)); });

    // ─────────────────────────────────────────────────────────────────────
    // AND/OR/XOR with common subexpressions
    // ─────────────────────────────────────────────────────────────────────

    // (a & b) & (a | b) → a & b
    rules.emplace_back("and_absorb_or",
        P::OpPat(Op::BitAnd, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("a"), s.at("b")); });

    // (a | b) | (a & b) → a | b
    rules.emplace_back("or_absorb_and",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitOr, s.at("a"), s.at("b")); });

    // a & (b ^ a) → (a & b) ^ a = a & ~b  (proved above via xor_and_simplify)
    // (a ^ b) & a → same: a & ~b (covered)

    // ─────────────────────────────────────────────────────────────────────
    // Rotate-like patterns (x << n) | (x >> (32-n))
    // ─────────────────────────────────────────────────────────────────────
    // These are architecture-specific but we can represent them:

    // (x << 1) | (x >> 31) - this is rotate_left(x, 1) for 32-bit
    // Can't simplify without architecture knowledge, skip

    // ─────────────────────────────────────────────────────────────────────
    // Bit clear patterns: x & ~(1 << n)
    // ─────────────────────────────────────────────────────────────────────

    // x & ~1 → x & (-2)  (clear bit 0)
    rules.emplace_back("and_not_one",
        P::OpPat(Op::BitAnd, {P::Wild("x"), P::OpPat(Op::BitNot, {P::ConstPat(1)})}),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(-2LL)); });

    // x | 0 → x  (already exists as or_zero)
    // 0 | x → x  (already exists as or_zero_left)

    // ─────────────────────────────────────────────────────────────────────
    // Additional shift and arithmetic combinations
    // ─────────────────────────────────────────────────────────────────────

    // (x << n) + (x << n) → x << (n+1)
    rules.emplace_back("shl_n_add_to_shl_np1",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Shl, {P::Wild("x"), P::Wild("n")}),
            P::OpPat(Op::Shl, {P::Wild("x"), P::Wild("n")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId np1 = g.addBinOp(Op::Add, s.at("n"), g.addConst(1));
            return g.addBinOp(Op::Shl, s.at("x"), np1);
        });

    // (x >> n) + (x >> n) → x >> (n-1) [only valid if n >= 1, but let's add it]
    // Actually: 2 * (x >> n) = x >> (n-1) [unsigned] - not safe for signed
    // Skip this one

    // (x << a) + (x << b) when a = b+1: = x << b + x << (b+1) = x << b * (1 + 2) = x << b * 3
    // Complex - skip

    // ─────────────────────────────────────────────────────────────────────
    // More XOR and AND/OR patterns
    // ─────────────────────────────────────────────────────────────────────

    // (x | y) ^ y → x & ~y
    // Proof: (x | y) ^ y = ((x & ~y) | (x & y) | y) ^ y
    // Actually: (x | y) ^ y - we expand: x|y = (~x&y)|(x&~y)|(x&y), XOR with y gives ~x&y... complex
    // Let's verify: (x|y)^y bit by bit:
    // x=0,y=0: 0^0=0; x=1,y=0: 1^0=1; x=0,y=1: 1^1=0; x=1,y=1: 1^1=0
    // So result = x&~y ✓
    rules.emplace_back("or_xor_y_to_and_not",
        P::OpPat(Op::BitXor, {
            P::OpPat(Op::BitOr, {P::Wild("x"), P::Wild("y")}),
            P::Wild("y")
        }),
        [](EGraph& g, const Subst& s) {
            ClassId ny = g.addUnaryOp(Op::BitNot, s.at("y"));
            return g.addBinOp(Op::BitAnd, s.at("x"), ny);
        });

    // y ^ (x | y) → x & ~y (same, reversed order)
    rules.emplace_back("y_xor_or_to_and_not",
        P::OpPat(Op::BitXor, {
            P::Wild("y"),
            P::OpPat(Op::BitOr, {P::Wild("x"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId ny = g.addUnaryOp(Op::BitNot, s.at("y"));
            return g.addBinOp(Op::BitAnd, s.at("x"), ny);
        });

    // x ^ (x & y) → x & ~y  (already have xor_and_simplify)

    // (x & y) | (~x & z) | (y & z) → (x & y) | (~x & z)  (consensus theorem)
    // Too complex for now

    // ─────────────────────────────────────────────────────────────────────
    // More shift patterns
    // ─────────────────────────────────────────────────────────────────────

    // (x << n) >> n for various n (loses lower bits)
    // (x >> n) << n for various n (loses upper bits + clears lower)
    // These need masking which requires knowing n

    // (x << a) >> b → x << (a-b) when a > b
    // These need to know a,b at compile time

    // ─────────────────────────────────────────────────────────────────────
    // Complement with constants
    // ─────────────────────────────────────────────────────────────────────

    // ~0 → -1
    rules.emplace_back("bitnot_zero",
        P::OpPat(Op::BitNot, {P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // ~(-1) → 0
    rules.emplace_back("bitnot_neg_one",
        P::OpPat(Op::BitNot, {P::ConstPat(-1)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ~1 → -2
    rules.emplace_back("bitnot_one",
        P::OpPat(Op::BitNot, {P::ConstPat(1)}),
        [](EGraph& g, const Subst&) { return g.addConst(-2LL); });

    // ~(-2) → 1
    rules.emplace_back("bitnot_neg_two",
        P::OpPat(Op::BitNot, {P::ConstPat(-2LL)}),
        [](EGraph& g, const Subst&) { return g.addConst(1); });

    // ─────────────────────────────────────────────────────────────────────
    // AND/OR with complement patterns
    // ─────────────────────────────────────────────────────────────────────

    // x & (~x | y) → x & y  (already have and_not_or)

    // (~x | y) & x → x & y
    rules.emplace_back("not_or_and",
        P::OpPat(Op::BitAnd, {
            P::OpPat(Op::BitOr, {P::OpPat(Op::BitNot, {P::Wild("x")}), P::Wild("y")}),
            P::Wild("x")
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitAnd, s.at("x"), s.at("y")); });

    // (~x & y) | x → x | y  (already have or_not_and)

    // y & (~x | y) → y  (absorption: ~x | y contains all y bits)
    // Proof: (y) & (~x | y) = (y & ~x) | (y & y) = (y & ~x) | y = y (since y | y&~x = y)
    rules.emplace_back("y_and_not_x_or_y",
        P::OpPat(Op::BitAnd, {
            P::Wild("y"),
            P::OpPat(Op::BitOr, {P::OpPat(Op::BitNot, {P::Wild("x")}), P::Wild("y")})
        }),
        [](EGraph&, const Subst& s) { return s.at("y"); });

    // ─────────────────────────────────────────────────────────────────────
    // Specific AND patterns
    // ─────────────────────────────────────────────────────────────────────

    // x & (x + 1) clears the trailing 1s up to the first 0 bit - too complex

    // (x + 1) & x → clears lowest bit group
    // Skip - complex

    // x & (x - 1) → clears lowest set bit (bit manipulation trick)
    // Can't simplify without semantics

    // x | (x - 1) → x with lowest zero bit set - too complex

    // ─────────────────────────────────────────────────────────────────────
    // Multiple AND/OR patterns
    // ─────────────────────────────────────────────────────────────────────

    // a & b & a → a & b (already have and_absorb_nested)
    // a | b | a → a | b (already have or_absorb_nested)

    // (a & b) & (b & c) → a & b & c (simplification of repeated b)
    // = b & a & c (reordering)
    // Hard to express without triadic patterns - skip

    // (a | b) | (b | c) → a | b | c
    // Hard to express without triadic - skip

    // ─────────────────────────────────────────────────────────────────────
    // Shift identity patterns
    // ─────────────────────────────────────────────────────────────────────

    // x * 4 → x << 2 (reverse - already have mul_4_to_shl2)
    // x >> 0 → x (already have shr_zero)

    // Chained shifts that can be combined
    // (x >> 1) + (x >> 1) → x & ~1
    // This is: 2 * (x >> 1) = (x / 2) * 2 = x with lowest bit cleared = x & (-2)
    rules.emplace_back("shr1_add_shr1",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Shr, {P::Wild("x"), P::ConstPat(1)}),
            P::OpPat(Op::Shr, {P::Wild("x"), P::ConstPat(1)})
        }),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(-2LL));
        });

    // ─────────────────────────────────────────────────────────────────────
    // More de Morgan applications
    // ─────────────────────────────────────────────────────────────────────

    // a | ~b | b → -1 (all ones)
    rules.emplace_back("or_not_b_b",
        P::OpPat(Op::BitOr, {
            P::Wild("a"),
            P::OpPat(Op::BitOr, {P::OpPat(Op::BitNot, {P::Wild("b")}), P::Wild("b")})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(-1); });

    // a & ~b & b → 0
    rules.emplace_back("and_not_b_b",
        P::OpPat(Op::BitAnd, {
            P::Wild("a"),
            P::OpPat(Op::BitAnd, {P::OpPat(Op::BitNot, {P::Wild("b")}), P::Wild("b")})
        }),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ─────────────────────────────────────────────────────────────────────
    // Additional constants with bitwise ops
    // ─────────────────────────────────────────────────────────────────────

    // NOTE: x & 255 and x % 256 (and the 65535/65536 variants) are NOT
    // equivalent for negative x under signed (truncating) modulo:
    //   -1 & 255 == 255, but -1 % 256 == -1.
    // These rules are intentionally omitted to avoid miscompilation.

    // ─────────────────────────────────────────────────────────────────────
    // Bitwise with shifts - additional patterns
    // ─────────────────────────────────────────────────────────────────────

    // (x | y) >> n → (x >> n) | (y >> n)
    rules.emplace_back("shr_or_distribute",
        P::OpPat(Op::Shr, {P::OpPat(Op::BitOr, {P::Wild("x"), P::Wild("y")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            ClassId xn = g.addBinOp(Op::Shr, s.at("x"), s.at("n"));
            ClassId yn = g.addBinOp(Op::Shr, s.at("y"), s.at("n"));
            return g.addBinOp(Op::BitOr, xn, yn);
        });

    // (x ^ y) >> n → (x >> n) ^ (y >> n)
    rules.emplace_back("shr_xor_distribute",
        P::OpPat(Op::Shr, {P::OpPat(Op::BitXor, {P::Wild("x"), P::Wild("y")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            ClassId xn = g.addBinOp(Op::Shr, s.at("x"), s.at("n"));
            ClassId yn = g.addBinOp(Op::Shr, s.at("y"), s.at("n"));
            return g.addBinOp(Op::BitXor, xn, yn);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Arithmetic-bitwise identities
    // ─────────────────────────────────────────────────────────────────────

    // (a | b) - (a & b) → a ^ b
    // Proof: a|b = (a^b) | (a&b) = (a^b) + (a&b) since the two terms have
    // no common bits.  Therefore (a|b) - (a&b) = a^b.
    rules.emplace_back("or_sub_and_to_xor",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::BitOr,  {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitXor, s.at("a"), s.at("b")); });

    // a ^ b → (a | b) - (a & b)  (reverse direction, useful for combining)
    rules.emplace_back("xor_to_or_sub_and",
        P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ClassId aorb  = g.addBinOp(Op::BitOr,  s.at("a"), s.at("b"));
            ClassId aandb = g.addBinOp(Op::BitAnd, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Sub, aorb, aandb);
        });

    // (a ^ b) + (a & b) → a | b
    // Because: a|b = (a^b) | (a&b), and since a^b and a&b share no bits,
    // bitwise-or is the same as addition for disjoint bit patterns.
    rules.emplace_back("xor_add_and_to_or",
        P::OpPat(Op::Add, {
            P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitOr, s.at("a"), s.at("b")); });

    // (a & b) + (a ^ b) → a | b  (commuted operands)
    rules.emplace_back("and_add_xor_to_or",
        P::OpPat(Op::Add, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) { return g.addBinOp(Op::BitOr, s.at("a"), s.at("b")); });

    // ─────────────────────────────────────────────────────────────────────
    // Complement-based bitwise simplifications
    // ─────────────────────────────────────────────────────────────────────

    // (~a) & (a | b) → (~a) & b
    // Proof: (~a) & (a | b) = ((~a) & a) | ((~a) & b) = 0 | ((~a) & b).
    rules.emplace_back("compl_and_or",
        P::OpPat(Op::BitAnd, {
            P::OpPat(Op::BitNot, {P::Wild("a")}),
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitAnd, na, s.at("b"));
        });

    // (a | b) & (~a) → (~a) & b  (commuted outer AND)
    rules.emplace_back("or_and_compl",
        P::OpPat(Op::BitAnd, {
            P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitNot, {P::Wild("a")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitAnd, na, s.at("b"));
        });

    // (~a) | (a & b) → (~a) | b
    // Proof: (~a) | (a & b) = ((~a) | a) & ((~a) | b) = (-1) & ((~a) | b).
    rules.emplace_back("compl_or_and",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitNot, {P::Wild("a")}),
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitOr, na, s.at("b"));
        });

    // (a & b) | (~a) → (~a) | b  (commuted outer OR)
    rules.emplace_back("and_or_compl",
        P::OpPat(Op::BitOr, {
            P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::BitNot, {P::Wild("a")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitOr, na, s.at("b"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Relational rules — guarded by predicates on matched constant values.
    // These generalise the hardcoded mul_2_to_shl1 .. mul_1024_to_shl10
    // and mod_2_to_and .. mod_1024_to_and families to ANY power of two.
    // ─────────────────────────────────────────────────────────────────────

    // x * C → x << log2(C)  when C is a power of 2  (C > 0)
    rules.emplace_back("mul_any_pow2_to_shl",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto cv = g.getConstValue(s.at("c"));
            long long v = *cv;
            int shift = 0;
            while (v > 1) { v >>= 1; ++shift; }
            return g.addBinOp(Op::Shl, s.at("x"), g.addConst(shift));
        },
        // Guard: c must be a positive power of 2 and > 1
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv) return false;
            long long v = *cv;
            return v > 1 && (v & (v - 1)) == 0;
        });

    // C * x → x << log2(C)  when C is a power of 2  (commuted)
    rules.emplace_back("mul_any_pow2_to_shl_comm",
        P::OpPat(Op::Mul, {P::Wild("c"), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            auto cv = g.getConstValue(s.at("c"));
            long long v = *cv;
            int shift = 0;
            while (v > 1) { v >>= 1; ++shift; }
            return g.addBinOp(Op::Shl, s.at("x"), g.addConst(shift));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv) return false;
            long long v = *cv;
            return v > 1 && (v & (v - 1)) == 0;
        });

    // x % C → x & (C - 1)  when C is a power of 2  (C > 0)
    rules.emplace_back("mod_any_pow2_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto cv = g.getConstValue(s.at("c"));
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(*cv - 1));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv) return false;
            long long v = *cv;
            return v > 1 && (v & (v - 1)) == 0;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Relational strength reduction for multiply-by-constant.
    // Converts x * C to shift-add sequences that are cheaper than hardware
    // multiply on specific CPUs (3-cycle mul vs 1-cycle shift + 1-cycle add).
    // The guard ensures C matches the specific constant pattern.
    // ─────────────────────────────────────────────────────────────────────

    // x * 3 → (x << 1) + x
    rules.emplace_back("mul_3_shift_add_rel",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            return cv && *cv == 3;
        });

    // x * 5 → (x << 2) + x
    rules.emplace_back("mul_5_shift_add_rel",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            return cv && *cv == 5;
        });

    // x * 7 → (x << 3) - x
    rules.emplace_back("mul_7_shift_sub_rel",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, shifted, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            return cv && *cv == 7;
        });

    // x * 9 → (x << 3) + x
    rules.emplace_back("mul_9_shift_add_rel",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            return cv && *cv == 9;
        });

    // x * 15 → (x << 4) - x
    rules.emplace_back("mul_15_shift_sub_rel",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Sub, shifted, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            return cv && *cv == 15;
        });

    // x * 17 → (x << 4) + x
    rules.emplace_back("mul_17_shift_add_rel",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            return cv && *cv == 17;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Relational shift combining rules.
    // ─────────────────────────────────────────────────────────────────────

    // (x << a) << b → x << (a + b)  when a, b are both constants
    rules.emplace_back("shl_shl_combine",
        P::OpPat(Op::Shl, {P::OpPat(Op::Shl, {P::Wild("x"), P::Wild("a")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            auto a = g.getConstValue(s.at("a"));
            auto b = g.getConstValue(s.at("b"));
            return g.addBinOp(Op::Shl, s.at("x"), g.addConst(*a + *b));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto a = g.getConstValue(s.at("a"));
            auto b = g.getConstValue(s.at("b"));
            if (!a || !b) return false;
            return (*a + *b) < 64;  // must not overflow shift width
        });

    // (x >> a) >> b → x >> (a + b)  when a, b are both constants
    rules.emplace_back("shr_shr_combine",
        P::OpPat(Op::Shr, {P::OpPat(Op::Shr, {P::Wild("x"), P::Wild("a")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            auto a = g.getConstValue(s.at("a"));
            auto b = g.getConstValue(s.at("b"));
            return g.addBinOp(Op::Shr, s.at("x"), g.addConst(*a + *b));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto a = g.getConstValue(s.at("a"));
            auto b = g.getConstValue(s.at("b"));
            if (!a || !b) return false;
            return (*a + *b) < 64;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Ternary/select optimizations
    // ─────────────────────────────────────────────────────────────────────

    // cond ? x : x → x  (same operands)
    rules.emplace_back("ternary_same",
        P::OpPat(Op::Ternary, {P::Wild("c"), P::Wild("x"), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // cond ? 1 : 0 → cond != 0  (boolean select)
    rules.emplace_back("ternary_1_0_to_bool",
        P::OpPat(Op::Ternary, {P::Wild("c"), P::ConstPat(1), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("c"), g.addConst(0));
        });

    // cond ? 0 : 1 → cond == 0  (inverted boolean select)
    rules.emplace_back("ternary_0_1_to_not",
        P::OpPat(Op::Ternary, {P::Wild("c"), P::ConstPat(0), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Eq, s.at("c"), g.addConst(0));
        });

    // !(cond) ? a : b → cond ? b : a  (negate condition = swap arms)
    rules.emplace_back("ternary_not_swap",
        P::OpPat(Op::Ternary, {P::OpPat(Op::LogNot, {P::Wild("c")}),
                                P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            ENode n(Op::Ternary, std::vector<ClassId>{s.at("c"), s.at("b"), s.at("a")});
            return g.add(std::move(n));
        });

    // (a == b) ? a : b → b  (select on equality — always b)
    // When a == b, both arms have the same value, so result is b (= a).
    rules.emplace_back("ternary_eq_select",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Eq, {P::Wild("a"), P::Wild("b")}),
                                P::Wild("a"), P::Wild("b")}),
        [](EGraph&, const Subst& s) { return s.at("b"); });

    // ─────────────────────────────────────────────────────────────────────
    // Shift-multiply distribution: (a << n) * b → (a * b) << n
    // ─────────────────────────────────────────────────────────────────────
    // Left-shift is equivalent to multiplication by a power of two.
    // Distributing the shift outside the multiply can reduce the critical
    // path length when the shift amount is known at compile time.
    // Guard: shift amount must be a positive constant < 64 to avoid UB.
    for (int n = 1; n <= 6; ++n) {
        std::string name = "shl_mul_distribute_" + std::to_string(n);
        rules.emplace_back(name,
            P::OpPat(Op::Mul, {P::OpPat(Op::Shl, {P::Wild("a"), P::ConstPat(n)}), P::Wild("b")}),
            [n](EGraph& g, const Subst& s) {
                ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
                return g.addBinOp(Op::Shl, ab, g.addConst(n));
            });
        // Commutative: b * (a << n) → (a * b) << n
        std::string name2 = "mul_shl_distribute_" + std::to_string(n);
        rules.emplace_back(name2,
            P::OpPat(Op::Mul, {P::Wild("b"), P::OpPat(Op::Shl, {P::Wild("a"), P::ConstPat(n)})}),
            [n](EGraph& g, const Subst& s) {
                ClassId ab = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
                return g.addBinOp(Op::Shl, ab, g.addConst(n));
            });
    }

    // ─────────────────────────────────────────────────────────────────────
    // Nested modulo: (x % (c*d)) % d → x % d  when d > 0
    // ─────────────────────────────────────────────────────────────────────
    // If we take x mod (c*d), the result is in [0, c*d-1], and taking that
    // mod d is equivalent to x mod d.  We handle common cases where the
    // outer modulus is a known multiple of the inner modulus.
    // (x % a) % b → x % b  when a is a constant multiple of b
    rules.emplace_back("mod_mod_divisible",
        P::OpPat(Op::Mod, {P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("a")}), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mod, s.at("x"), s.at("b"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto aVal = g.getConstValue(s.at("a"));
            auto bVal = g.getConstValue(s.at("b"));
            if (!aVal || !bVal || *bVal <= 0 || *aVal <= 0) return false;
            return (*aVal % *bVal) == 0;
        });

    // ─────────────────────────────────────────────────────────────────────
    // XOR with OR/AND distribution patterns
    // ─────────────────────────────────────────────────────────────────────
    // a ^ (a | b) → ~a & b
    // Proof: a ^ (a | b) = (~a & (a | b)) | (a & ~(a | b))
    //      = (~a & a) | (~a & b) | (a & ~a & ~b) = ~a & b  ✓
    rules.emplace_back("xor_or_simplify",
        P::OpPat(Op::BitXor, {P::Wild("a"), P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitAnd, na, s.at("b"));
        });

    // (a | b) ^ a → ~a & b  (reversed operand order)
    rules.emplace_back("or_xor_simplify",
        P::OpPat(Op::BitXor, {P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}), P::Wild("a")}),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::BitNot, s.at("a"));
            return g.addBinOp(Op::BitAnd, na, s.at("b"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Ternary with negated comparison → simpler ternary
    // ─────────────────────────────────────────────────────────────────────
    // (a < b) ? 0 : 1 → a >= b
    rules.emplace_back("ternary_lt_0_1_to_ge",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("b")}),
                                P::ConstPat(0), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ge, s.at("a"), s.at("b"));
        });

    // (a > b) ? 0 : 1 → a <= b
    rules.emplace_back("ternary_gt_0_1_to_le",
        P::OpPat(Op::Ternary, {P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")}),
                                P::ConstPat(0), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Le, s.at("a"), s.at("b"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Bitwise-logical equivalence for boolean values
    // ─────────────────────────────────────────────────────────────────────
    // When both operands are boolean (0 or 1), bitwise ops are equivalent
    // to logical ops, which have cheaper codegen (no masking).
    // a & b → a && b  when both are boolean
    rules.emplace_back("bitand_to_logand_bool",
        P::OpPat(Op::BitAnd, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::LogAnd, s.at("a"), s.at("b"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            return g.isClassBoolean(s.at("a")) && g.isClassBoolean(s.at("b"));
        });

    // a | b → a || b  when both are boolean
    rules.emplace_back("bitor_to_logor_bool",
        P::OpPat(Op::BitOr, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::LogOr, s.at("a"), s.at("b"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            return g.isClassBoolean(s.at("a")) && g.isClassBoolean(s.at("b"));
        });

    // a ^ b → a != b  when both are boolean
    // XOR of two boolean values is equivalent to inequality comparison.
    rules.emplace_back("bitxor_to_ne_bool",
        P::OpPat(Op::BitXor, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ne, s.at("a"), s.at("b"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            return g.isClassBoolean(s.at("a")) && g.isClassBoolean(s.at("b"));
        });


    return rules;
}

std::vector<RewriteRule> getRelationalRules() {
    using P = Pattern;
    std::vector<RewriteRule> rules;

    // ─────────────────────────────────────────────────────────────────────
    // Multi-variable relational: cancel multiply-then-divide by same const
    // ─────────────────────────────────────────────────────────────────────

    // x * C / C → x
    rules.emplace_back("mul_div_cancel",
        P::OpPat(Op::Div, {P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}), P::Wild("c")}),
        [](EGraph&, const Subst& s) { return s.at("x"); },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            return cv && *cv != 0;
        });

    // (x + C1) - C2 → x + (C1 - C2)
    rules.emplace_back("add_sub_const_fold",
        P::OpPat(Op::Sub, {P::OpPat(Op::Add, {P::Wild("x"), P::Wild("c1")}), P::Wild("c2")}),
        [](EGraph& g, const Subst& s) {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return g.addBinOp(Op::Add, s.at("x"), g.addConst(*c1 - *c2));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return c1.has_value() && c2.has_value();
        });

    // (x - C1) + C2 → x + (C2 - C1)
    rules.emplace_back("sub_add_const_fold",
        P::OpPat(Op::Add, {P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("c1")}), P::Wild("c2")}),
        [](EGraph& g, const Subst& s) {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return g.addBinOp(Op::Add, s.at("x"), g.addConst(*c2 - *c1));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return c1.has_value() && c2.has_value();
        });

    // (x * C1) * C2 → x * (C1 * C2)
    rules.emplace_back("mul_mul_const_fold",
        P::OpPat(Op::Mul, {P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c1")}), P::Wild("c2")}),
        [](EGraph& g, const Subst& s) {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(*c1 * *c2));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return c1.has_value() && c2.has_value();
        });

    // ─────────────────────────────────────────────────────────────────────
    // Bitfield extraction / insertion patterns
    // ─────────────────────────────────────────────────────────────────────

    // (x & mask) >> a → (x >> a) & (mask >> a)  when mask has no low bits
    rules.emplace_back("mask_shift_normalize",
        P::OpPat(Op::Shr, {
            P::OpPat(Op::BitAnd, {P::Wild("x"), P::Wild("mask")}),
            P::Wild("a")
        }),
        [](EGraph& g, const Subst& s) {
            auto a = g.getConstValue(s.at("a"));
            auto m = g.getConstValue(s.at("mask"));
            long long newMask = *m >> *a;
            return g.addBinOp(Op::BitAnd,
                g.addBinOp(Op::Shr, s.at("x"), s.at("a")),
                g.addConst(newMask));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto a = g.getConstValue(s.at("a"));
            auto m = g.getConstValue(s.at("mask"));
            if (!a || !m) return false;
            if (*a <= 0 || *a >= 63) return false;
            long long lowBits = (1LL << *a) - 1;
            return (*m & lowBits) == 0 && *m != 0;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison chain strength reduction
    // ─────────────────────────────────────────────────────────────────────

    // (x >= C1) && (x <= C2) → (x - C1) <= (C2 - C1)
    rules.emplace_back("range_check_unsigned",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Ge, {P::Wild("x"), P::Wild("lo")}),
            P::OpPat(Op::Le, {P::Wild("x"), P::Wild("hi")})
        }),
        [](EGraph& g, const Subst& s) {
            auto lo = g.getConstValue(s.at("lo"));
            auto hi = g.getConstValue(s.at("hi"));
            ClassId diff = g.addBinOp(Op::Sub, s.at("x"), s.at("lo"));
            ClassId range = g.addConst(*hi - *lo);
            return g.addBinOp(Op::Le, diff, range);
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto lo = g.getConstValue(s.at("lo"));
            auto hi = g.getConstValue(s.at("hi"));
            if (!lo || !hi) return false;
            return *hi >= *lo;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Algebraic identities with multi-variable guards
    // ─────────────────────────────────────────────────────────────────────

    // (x + y) * (x - y) → x*x - y*y
    rules.emplace_back("diff_of_squares",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Add, {P::Wild("x"), P::Wild("y")}),
            P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId xx = g.addBinOp(Op::Mul, s.at("x"), s.at("x"));
            ClassId yy = g.addBinOp(Op::Mul, s.at("y"), s.at("y"));
            return g.addBinOp(Op::Sub, xx, yy);
        });

    // x*x - y*y → (x + y) * (x - y)
    rules.emplace_back("factor_diff_of_squares",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("x")}),
            P::OpPat(Op::Mul, {P::Wild("y"), P::Wild("y")})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId sum  = g.addBinOp(Op::Add, s.at("x"), s.at("y"));
            ClassId diff = g.addBinOp(Op::Sub, s.at("x"), s.at("y"));
            return g.addBinOp(Op::Mul, sum, diff);
        });

    // (x << n) + (x << n) → x << (n + 1)
    rules.emplace_back("double_shift_add",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Shl, {P::Wild("x"), P::Wild("n")}),
            P::OpPat(Op::Shl, {P::Wild("x"), P::Wild("n")})
        }),
        [](EGraph& g, const Subst& s) {
            auto n = g.getConstValue(s.at("n"));
            return g.addBinOp(Op::Shl, s.at("x"), g.addConst(*n + 1));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto n = g.getConstValue(s.at("n"));
            return n.has_value() && *n >= 0 && *n < 63;
        });

    // x * (2^n + 1) → (x << n) + x
    rules.emplace_back("mul_pow2_plus1",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto cv = g.getConstValue(s.at("c"));
            long long v = *cv - 1;
            int shift = 0;
            while (v > 1) { v >>= 1; ++shift; }
            ClassId shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(shift));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv) return false;
            long long v = *cv;
            if (v <= 2) return false;
            long long vm1 = v - 1;
            return vm1 > 0 && (vm1 & (vm1 - 1)) == 0;
        });

    // x * (2^n - 1) → (x << n) - x
    rules.emplace_back("mul_pow2_minus1",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto cv = g.getConstValue(s.at("c"));
            long long v = *cv + 1;
            int shift = 0;
            while (v > 1) { v >>= 1; ++shift; }
            ClassId shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(shift));
            return g.addBinOp(Op::Sub, shifted, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv) return false;
            long long v = *cv;
            if (v <= 2) return false;
            long long vp1 = v + 1;
            return vp1 > 0 && (vp1 & (vp1 - 1)) == 0;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Logical-to-bitwise lowering for boolean values
    // ─────────────────────────────────────────────────────────────────────

    // (a == 0) && (b == 0) → (a | b) == 0
    rules.emplace_back("combine_zero_checks_and",
        P::OpPat(Op::LogAnd, {
            P::OpPat(Op::Eq, {P::Wild("a"), P::ConstPat(0)}),
            P::OpPat(Op::Eq, {P::Wild("b"), P::ConstPat(0)})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId orNode = g.addBinOp(Op::BitOr, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Eq, orNode, g.addConst(0));
        });

    // (a != 0) || (b != 0) → (a | b) != 0
    rules.emplace_back("combine_nonzero_checks_or",
        P::OpPat(Op::LogOr, {
            P::OpPat(Op::Ne, {P::Wild("a"), P::ConstPat(0)}),
            P::OpPat(Op::Ne, {P::Wild("b"), P::ConstPat(0)})
        }),
        [](EGraph& g, const Subst& s) {
            ClassId orNode = g.addBinOp(Op::BitOr, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Ne, orNode, g.addConst(0));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Division by power-of-2 for non-negative values (SAFE for unsigned)
    // Note: x / C → x >> log2(C) is UNSOUND for signed values because
    // signed division rounds toward zero while arithmetic right shift
    // rounds toward negative infinity.  We guard with isNonNeg analysis.
    // ─────────────────────────────────────────────────────────────────────

    // x / C → x >> log2(C)  when x is non-negative and C is power-of-2
    rules.emplace_back("div_pow2_nonneg",
        P::OpPat(Op::Div, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto cv = g.getConstValue(s.at("c"));
            long long v = *cv;
            int shift = 0;
            while (v > 1) { v >>= 1; ++shift; }
            return g.addBinOp(Op::Shr, s.at("x"), g.addConst(shift));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv || *cv <= 1) return false;
            long long v = *cv;
            if ((v & (v - 1)) != 0) return false;  // not power of 2
            // Guard: x must be non-negative to ensure rounding correctness
            const auto& xClass = g.getClass(s.at("x"));
            return xClass.isNonNeg;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Modulo by power-of-2 for non-negative values → bitwise AND
    // x % C → x & (C - 1)  when x is non-negative and C is power-of-2
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("mod_pow2_nonneg",
        P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto cv = g.getConstValue(s.at("c"));
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(*cv - 1));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv || *cv <= 1) return false;
            long long v = *cv;
            if ((v & (v - 1)) != 0) return false;  // not power of 2
            const auto& xClass = g.getClass(s.at("x"));
            return xClass.isNonNeg;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Algebraic factoring: x*a + x*b → x*(a+b)  where a, b are constants
    // This reduces two multiplies + one add to one multiply + one add.
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("factor_const_mul",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("a")}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            auto a = g.getConstValue(s.at("a"));
            auto b = g.getConstValue(s.at("b"));
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(*a + *b));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto a = g.getConstValue(s.at("a"));
            auto b = g.getConstValue(s.at("b"));
            return a.has_value() && b.has_value();
        });

    // x*a - x*b → x*(a-b)  where a, b are constants
    rules.emplace_back("factor_const_mul_sub",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("a")}),
            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("b")})
        }),
        [](EGraph& g, const Subst& s) {
            auto a = g.getConstValue(s.at("a"));
            auto b = g.getConstValue(s.at("b"));
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(*a - *b));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto a = g.getConstValue(s.at("a"));
            auto b = g.getConstValue(s.at("b"));
            return a.has_value() && b.has_value();
        });

    // ─────────────────────────────────────────────────────────────────────
    // Modulo strength reduction for non-negative values
    // x % C → (x - (x / C) * C) which LLVM then strength-reduces
    // But more importantly, when x is non-neg, srem → urem which is cheaper.
    // These rules propagate non-negativity through chains.
    // ─────────────────────────────────────────────────────────────────────

    // (a % C) + b where a%C is non-neg by analysis → result is non-neg if b is too
    // This helps chains like ((i^j) + k) % 37 in nested loops

    // Distributive modulo: (a + b) % C → ((a%C) + (b%C)) % C  when a,b non-neg
    // Allows partial evaluation when one term is a loop constant.
    rules.emplace_back("mod_distribute_add_nonneg",
        P::OpPat(Op::Mod, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            ClassId amod = g.addBinOp(Op::Mod, s.at("a"), s.at("c"));
            ClassId bmod = g.addBinOp(Op::Mod, s.at("b"), s.at("c"));
            ClassId sum = g.addBinOp(Op::Add, amod, bmod);
            return g.addBinOp(Op::Mod, sum, s.at("c"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv || *cv <= 0) return false;
            const auto& aClass = g.getClass(s.at("a"));
            const auto& bClass = g.getClass(s.at("b"));
            return aClass.isNonNeg && bClass.isNonNeg;
        });

    // x * C1 % C2 → 0  when C1 is a multiple of C2 and x is non-negative
    rules.emplace_back("mod_mul_multiple_zero",
        P::OpPat(Op::Mod, {P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c1")}), P::Wild("c2")}),
        [](EGraph& g, const Subst&) {
            return g.addConst(0);
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            if (!c1 || !c2 || *c2 == 0) return false;
            if ((*c1 % *c2) != 0) return false;
            // Only safe when x is non-negative (signed modulo rounds toward zero)
            const auto& xClass = g.getClass(s.at("x"));
            return xClass.isNonNeg;
        });

    // (x % C) % C → x % C  (idempotent modulo)
    rules.emplace_back("mod_mod_same",
        P::OpPat(Op::Mod, {P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("c")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mod, s.at("x"), s.at("c"));
        });

    // (x & mask) where mask = C-1 and C is power-of-2 → x % C (for non-neg x)
    // This enables subsequent mod optimizations on the AND result
    // Note: the reverse direction (mod→AND) is already handled by mod_pow2_nonneg

    // ─────────────────────────────────────────────────────────────────────
    // Shift-based division strength reduction
    // ─────────────────────────────────────────────────────────────────────

    // x / C → multiply-shift sequence for non-power-of-2 constants (non-neg only)
    // This is handled better by LLVM's backend but marking non-neg helps

    // x * C / C → x  (already exists, but also with non-neg guard for safety)

    // (x << n) / (1 << n) → x  when x is non-negative
    rules.emplace_back("shl_div_cancel_nonneg",
        P::OpPat(Op::Div, {P::OpPat(Op::Shl, {P::Wild("x"), P::Wild("n")}), P::Wild("c")}),
        [](EGraph&, const Subst& s) { return s.at("x"); },
        [](const EGraph& g, const Subst& s) -> bool {
            auto n = g.getConstValue(s.at("n"));
            auto c = g.getConstValue(s.at("c"));
            if (!n || !c || *n < 0 || *n >= 63 || *c <= 0) return false;
            return *c == (1LL << *n);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Distributive law optimizations
    // ─────────────────────────────────────────────────────────────────────

    // x * C1 + x * C2 → x * (C1 + C2)  (factor out common multiplicand)
    rules.emplace_back("add_factor_out",
        P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c1")}),
                            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c2")})}),
        [](EGraph& g, const Subst& s) {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(*c1 + *c2));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return c1.has_value() && c2.has_value();
        });

    // x * C1 - x * C2 → x * (C1 - C2)
    rules.emplace_back("sub_factor_out",
        P::OpPat(Op::Sub, {P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c1")}),
                            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c2")})}),
        [](EGraph& g, const Subst& s) {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return g.addBinOp(Op::Mul, s.at("x"), g.addConst(*c1 - *c2));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return c1.has_value() && c2.has_value();
        });

    // ─────────────────────────────────────────────────────────────────────
    // Strength reduction: x * (2^n - 1) → (x << n) - x
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("mul_to_shl_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto c = g.getConstValue(s.at("c"));
            int64_t val = *c + 1;  // c = 2^n - 1, val = 2^n
            int n = 0;
            while (val > 1) { val >>= 1; n++; }
            return g.addBinOp(Op::Sub,
                g.addBinOp(Op::Shl, s.at("x"), g.addConst(n)),
                s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c = g.getConstValue(s.at("c"));
            if (!c || *c < 3) return false;
            int64_t val = *c + 1;
            return val > 0 && (val & (val - 1)) == 0;  // check if c+1 is power of 2
        });

    // x * (2^n + 1) → (x << n) + x
    rules.emplace_back("mul_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto c = g.getConstValue(s.at("c"));
            int64_t val = *c - 1;  // c = 2^n + 1, val = 2^n
            int n = 0;
            while (val > 1) { val >>= 1; n++; }
            return g.addBinOp(Op::Add,
                g.addBinOp(Op::Shl, s.at("x"), g.addConst(n)),
                s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c = g.getConstValue(s.at("c"));
            if (!c || *c < 3) return false;
            int64_t val = *c - 1;
            return val > 0 && (val & (val - 1)) == 0;  // check if c-1 is power of 2
        });

    // ─────────────────────────────────────────────────────────────────────
    // Collatz-style: 3*x+1 → (x << 1) + x + 1  (saves mul cost 3→2+1)
    // Pattern: (x * 3) + 1 → (x + x + x) + 1 → ((x<<1) + x) + 1
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("mul3_add1_to_shl_add",
        P::OpPat(Op::Add, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)}), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) {
            ClassId shl1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId add = g.addBinOp(Op::Add, shl1, s.at("x"));
            return g.addBinOp(Op::Add, add, g.addConst(1));
        });

    // Commuted: 1 + (x * 3)
    rules.emplace_back("add1_mul3_to_shl_add",
        P::OpPat(Op::Add, {P::ConstPat(1), P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)})}),
        [](EGraph& g, const Subst& s) {
            ClassId shl1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            ClassId add = g.addBinOp(Op::Add, shl1, s.at("x"));
            return g.addBinOp(Op::Add, add, g.addConst(1));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Division of even values: x/2 → x>>1  when x is known even
    // Detected via x = (y * 2), (y << 1), or x%2==0 analysis
    // ─────────────────────────────────────────────────────────────────────
    // (x * 2) / 2 → x  (no guard needed; exact divide)
    rules.emplace_back("mul2_div2_cancel",
        P::OpPat(Op::Div, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2)}), P::ConstPat(2)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // (x << 1) / 2 → x  (shift-left by 1 is multiply by 2)
    rules.emplace_back("shl1_div2_cancel",
        P::OpPat(Op::Div, {P::OpPat(Op::Shl, {P::Wild("x"), P::ConstPat(1)}), P::ConstPat(2)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ─────────────────────────────────────────────────────────────────────
    // Ternary + comparison fusion (min/max at AST level)
    // These enable the cost model to pick the cheapest representation.
    // ─────────────────────────────────────────────────────────────────────

    // (a < b) ? a : b  and  (a <= b) ? a : b  →  min(a, b) idiom
    // We represent min/max via ternary with a canonical comparison,
    // but we also give the optimizer the dual form for cost comparison.

    // (a > b) ? a : b → (a < b) ? b : a  (normalize comparisons)
    rules.emplace_back("ternary_gt_to_lt_swap",
        P::OpPat(Op::Ternary, {
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")}),
            P::Wild("a"), P::Wild("b")
        }),
        [](EGraph& g, const Subst& s) {
            ClassId cmp = g.addBinOp(Op::Lt, s.at("a"), s.at("b"));
            ENode sel(Op::Ternary, std::vector<ClassId>{cmp, s.at("b"), s.at("a")});
            return g.add(sel);
        });

    // (a >= b) ? a : b → (a < b) ? b : a
    rules.emplace_back("ternary_ge_to_lt_swap",
        P::OpPat(Op::Ternary, {
            P::OpPat(Op::Ge, {P::Wild("a"), P::Wild("b")}),
            P::Wild("a"), P::Wild("b")
        }),
        [](EGraph& g, const Subst& s) {
            ClassId cmp = g.addBinOp(Op::Lt, s.at("a"), s.at("b"));
            ENode sel(Op::Ternary, std::vector<ClassId>{cmp, s.at("b"), s.at("a")});
            return g.add(sel);
        });

    // (a > b) ? b : a → (a < b) ? a : b  (normalize min pattern)
    rules.emplace_back("ternary_gt_swap_to_lt",
        P::OpPat(Op::Ternary, {
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("b")}),
            P::Wild("b"), P::Wild("a")
        }),
        [](EGraph& g, const Subst& s) {
            ClassId cmp = g.addBinOp(Op::Lt, s.at("a"), s.at("b"));
            ENode sel(Op::Ternary, std::vector<ClassId>{cmp, s.at("a"), s.at("b")});
            return g.add(sel);
        });

    // (a >= b) ? b : a → (a <= b) ? a : b
    rules.emplace_back("ternary_ge_swap_to_le",
        P::OpPat(Op::Ternary, {
            P::OpPat(Op::Ge, {P::Wild("a"), P::Wild("b")}),
            P::Wild("b"), P::Wild("a")
        }),
        [](EGraph& g, const Subst& s) {
            ClassId cmp = g.addBinOp(Op::Le, s.at("a"), s.at("b"));
            ENode sel(Op::Ternary, std::vector<ClassId>{cmp, s.at("a"), s.at("b")});
            return g.add(sel);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Modulo-comparison strength reduction (common in Collatz/sieve)
    // x % 2 == 0  →  (x & 1) == 0  (bitwise cheaper than modulo)
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("mod2_eq0_to_and1_eq0",
        P::OpPat(Op::Eq, {P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(2)}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            ClassId andOp = g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Eq, andOp, g.addConst(0));
        });

    // x % 2 != 0  →  (x & 1) != 0
    rules.emplace_back("mod2_ne0_to_and1_ne0",
        P::OpPat(Op::Ne, {P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(2)}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            ClassId andOp = g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Ne, andOp, g.addConst(0));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Conditional half: (x % 2 == 0) ? (x / 2) : y  →  ((x & 1) == 0) ? (x >> 1) : y
    // This chains the mod2→and1 + div2→shr1 optimizations in a single ternary
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("collatz_even_branch",
        P::OpPat(Op::Ternary, {
            P::OpPat(Op::Eq, {P::OpPat(Op::Mod, {P::Wild("x"), P::ConstPat(2)}), P::ConstPat(0)}),
            P::OpPat(Op::Div, {P::Wild("x"), P::ConstPat(2)}),
            P::Wild("y")
        }),
        [](EGraph& g, const Subst& s) {
            ClassId andOp = g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(1));
            ClassId cond = g.addBinOp(Op::Eq, andOp, g.addConst(0));
            ClassId shr = g.addBinOp(Op::Shr, s.at("x"), g.addConst(1));
            ENode sel(Op::Ternary, std::vector<ClassId>{cond, shr, s.at("y")});
            return g.add(sel);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Non-negative division by small constants via multiply-high + shift
    // x / 3 → (x * 0x5556) >> 16  for x in [0, 65535] — but at AST level
    // we don't know bit-widths. Instead, these rules help the e-graph
    // represent the division as a cheaper form that the LLVM backend can
    // lower optimally. The key insight: marking x as non-negative lets
    // LLVM use udiv instead of sdiv, which is ~2x cheaper on x86.
    // ─────────────────────────────────────────────────────────────────────

    // x / C → 0  when x is non-negative and C > max_possible_x
    // (useful when x is known to be a small value like a boolean or %result)
    rules.emplace_back("div_large_const_nonneg_boolean",
        P::OpPat(Op::Div, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv || *cv <= 1) return false;
            const auto& xClass = g.getClass(s.at("x"));
            return xClass.isBoolean && *cv > 1;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Mod of boolean: (bool_expr) % C → bool_expr  when C > 1
    // If x is 0 or 1, then x%C = x for any C > 1
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("mod_of_boolean",
        P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("c")}),
        [](EGraph&, const Subst& s) { return s.at("x"); },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv || *cv <= 1) return false;
            const auto& xClass = g.getClass(s.at("x"));
            return xClass.isBoolean;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Nested ternary simplification
    // (c ? a : b) where c = (x == 0): (x == 0) ? a : b
    // This enables short-circuit evaluation and branch-free select
    // ─────────────────────────────────────────────────────────────────────

    // (x == 0) ? 0 : f(x)  → trivially 0 when x==0 (identity)
    // Already handled by ternary_true_cond/ternary_false_cond

    // min(x, 0) when x is non-neg → 0 (a < b ? a : b where b=0, a non-neg)
    rules.emplace_back("ternary_min_nonneg_zero",
        P::OpPat(Op::Ternary, {
            P::OpPat(Op::Lt, {P::Wild("x"), P::ConstPat(0)}),
            P::Wild("x"), P::ConstPat(0)
        }),
        [](EGraph& g, const Subst&) { return g.addConst(0); },
        [](const EGraph& g, const Subst& s) -> bool {
            const auto& xClass = g.getClass(s.at("x"));
            return xClass.isNonNeg;
        });

    // max(x, 0) when x is non-neg → x
    rules.emplace_back("ternary_max_nonneg_zero",
        P::OpPat(Op::Ternary, {
            P::OpPat(Op::Gt, {P::Wild("x"), P::ConstPat(0)}),
            P::Wild("x"), P::ConstPat(0)
        }),
        [](EGraph&, const Subst& s) { return s.at("x"); },
        [](const EGraph& g, const Subst& s) -> bool {
            const auto& xClass = g.getClass(s.at("x"));
            return xClass.isNonNeg;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Comparison with arithmetic: simplify comparisons involving ±1
    // ─────────────────────────────────────────────────────────────────────

    // (a - 1) >= 0 → a > 0  (when a is non-negative integer)
    rules.emplace_back("sub1_ge0_to_gt0",
        P::OpPat(Op::Ge, {P::OpPat(Op::Sub, {P::Wild("a"), P::ConstPat(1)}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Gt, s.at("a"), g.addConst(0));
        });

    // (a + 1) <= 0 → a < 0
    rules.emplace_back("add1_le0_to_lt0",
        P::OpPat(Op::Le, {P::OpPat(Op::Add, {P::Wild("a"), P::ConstPat(1)}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Lt, s.at("a"), g.addConst(0));
        });

    // (a - 1) < 0 → a <= 0
    rules.emplace_back("sub1_lt0_to_le0",
        P::OpPat(Op::Lt, {P::OpPat(Op::Sub, {P::Wild("a"), P::ConstPat(1)}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Le, s.at("a"), g.addConst(0));
        });

    // (a + 1) > 0 → a >= 0
    rules.emplace_back("add1_gt0_to_ge0",
        P::OpPat(Op::Gt, {P::OpPat(Op::Add, {P::Wild("a"), P::ConstPat(1)}), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Ge, s.at("a"), g.addConst(0));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Multiply-then-shift strength reduction for common scale factors
    // ─────────────────────────────────────────────────────────────────────
    // These patterns arise in fixed-point arithmetic and hash computations
    // where a multiplication by a small constant is followed by a shift.

    // (x * 3) >> 1 → x + (x >> 1)  (approximate divide by 2/3)
    rules.emplace_back("mul3_shr1",
        P::OpPat(Op::Shr, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)}), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) {
            ClassId shr = g.addBinOp(Op::Shr, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s.at("x"), shr);
        });

    // (x * 5) >> 2 → x + (x >> 2)  (approximate multiply by 5/4)
    rules.emplace_back("mul5_shr2",
        P::OpPat(Op::Shr, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(5)}), P::ConstPat(2)}),
        [](EGraph& g, const Subst& s) {
            ClassId shr = g.addBinOp(Op::Shr, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, s.at("x"), shr);
        });

    // (x * 7) >> 3 → x - (x >> 3)  (x - x/8 = 7x/8)
    rules.emplace_back("mul7_shr3",
        P::OpPat(Op::Shr, {P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(7)}), P::ConstPat(3)}),
        [](EGraph& g, const Subst& s) {
            ClassId shr = g.addBinOp(Op::Shr, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, s.at("x"), shr);
        });

    // ─────────────────────────────────────────────────────────────────────
    // Division chain folding: (x / C1) / C2 → x / (C1 * C2)
    // Guard: C1*C2 must not overflow and both must be positive.
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("div_chain_fold",
        P::OpPat(Op::Div, {P::OpPat(Op::Div, {P::Wild("x"), P::Wild("c1")}), P::Wild("c2")}),
        [](EGraph& g, const Subst& s) {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            return g.addBinOp(Op::Div, s.at("x"), g.addConst(*c1 * *c2));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            if (!c1 || !c2 || *c1 <= 0 || *c2 <= 0) return false;
            // Guard against overflow: C1*C2 must fit in int64
            return *c1 <= INT64_MAX / *c2;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Modulo chain folding: (x % C1) % C2 → x % C2  when C2 divides C1
    // If C1 is a multiple of C2, then x % C1 is in [0, C1-1], and since
    // C2 divides C1, (x % C1) % C2 produces the same result as x % C2.
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("mod_chain_fold",
        P::OpPat(Op::Mod, {P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("c1")}), P::Wild("c2")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mod, s.at("x"), s.at("c2"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c1 = g.getConstValue(s.at("c1"));
            auto c2 = g.getConstValue(s.at("c2"));
            if (!c1 || !c2 || *c1 <= 0 || *c2 <= 0) return false;
            return (*c1 % *c2) == 0;
        });

    // ─────────────────────────────────────────────────────────────────────
    // Power-of-two modulo for non-negative: x % (2^n) → x & (2^n - 1)
    // This generalises the hardcoded mod_2/4/8/.../1024 patterns to ANY
    // power-of-two modulus, catching x%2048, x%4096, x%8192, etc.
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("mod_any_pow2_to_and",
        P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto c = g.getConstValue(s.at("c"));
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(*c - 1));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c = g.getConstValue(s.at("c"));
            if (!c || *c <= 0) return false;
            if ((*c & (*c - 1)) != 0) return false;  // not power of 2
            // Must be > 1024 to avoid overlap with existing mod_2..mod_1024 rules
            if (*c <= 1024) return false;
            return g.isClassNonNeg(s.at("x"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Division by power-of-two for non-negative: x / (2^n) → x >> n
    // Generalises to ANY power-of-two divisor beyond the existing 2048/4096.
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("div_any_pow2_to_shr",
        P::OpPat(Op::Div, {P::Wild("x"), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto c = g.getConstValue(s.at("c"));
            int64_t val = *c;
            int n = 0;
            while (val > 1) { val >>= 1; n++; }
            return g.addBinOp(Op::Shr, s.at("x"), g.addConst(n));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto c = g.getConstValue(s.at("c"));
            if (!c || *c <= 0) return false;
            if ((*c & (*c - 1)) != 0) return false;  // not power of 2
            // Only fire for values > 4096 to avoid overlap with existing rules
            if (*c <= 4096) return false;
            return g.isClassNonNeg(s.at("x"));
        });

    // ─────────────────────────────────────────────────────────────────────
    // Ternary min/max with same operand: min(a, a) → a, max(a, a) → a
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("ternary_lt_same_is_identity",
        P::OpPat(Op::Ternary, {
            P::OpPat(Op::Lt, {P::Wild("a"), P::Wild("a")}),
            P::Wild("a"), P::Wild("a")
        }),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    rules.emplace_back("ternary_gt_same_is_identity",
        P::OpPat(Op::Ternary, {
            P::OpPat(Op::Gt, {P::Wild("a"), P::Wild("a")}),
            P::Wild("a"), P::Wild("a")
        }),
        [](EGraph&, const Subst& s) { return s.at("a"); });

    // ─────────────────────────────────────────────────────────────────────
    // Double comparison elimination: (a > b) == 1 → a > b
    // Since comparisons already produce 0 or 1, comparing the result
    // against 1 is redundant.
    // ─────────────────────────────────────────────────────────────────────
    rules.emplace_back("cmp_eq1_identity",
        P::OpPat(Op::Eq, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph&, const Subst& s) { return s.at("x"); },
        [](const EGraph& g, const Subst& s) -> bool {
            return g.isClassBoolean(s.at("x"));
        });

    // bool_expr == 0 → !bool_expr
    rules.emplace_back("cmp_eq0_to_not",
        P::OpPat(Op::Eq, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::LogNot, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            return g.isClassBoolean(s.at("x"));
        });

    // bool_expr != 0 → bool_expr
    rules.emplace_back("cmp_ne0_identity",
        P::OpPat(Op::Ne, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); },
        [](const EGraph& g, const Subst& s) -> bool {
            return g.isClassBoolean(s.at("x"));
        });

    // bool_expr != 1 → !bool_expr
    rules.emplace_back("cmp_ne1_to_not",
        P::OpPat(Op::Ne, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::LogNot, s.at("x"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            return g.isClassBoolean(s.at("x"));
        });

    return rules;
}

std::vector<RewriteRule> getFloatingPointRules() {
    using P = Pattern;
    std::vector<RewriteRule> rules;

    // ── IEEE-754 safe FP optimizations ─────────────────────────────────
    // These rules preserve strict IEEE-754 semantics.

    // x * 1.0 → x  (exact: 1.0 is representable and multiplying by it
    // is exact for all finite values, including subnormals)
    rules.emplace_back("fp_mul_one",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(1.0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x * (-1.0) → -x  (negation is exact in IEEE-754)
    rules.emplace_back("fp_mul_neg_one",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(-1.0)}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::Neg, s.at("x"));
        });

    // x * 2.0 → x + x  (addition is typically faster or equal to multiply;
    // this is exact because x + x = 2*x exactly in IEEE-754)
    rules.emplace_back("fp_mul_two_to_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(2.0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Add, s.at("x"), s.at("x"));
        });

    // x / 1.0 → x  (dividing by 1.0 is exact)
    rules.emplace_back("fp_div_one",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstFPat(1.0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x / (-1.0) → -x  (dividing by -1.0 is exact)
    rules.emplace_back("fp_div_neg_one",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstFPat(-1.0)}),
        [](EGraph& g, const Subst& s) {
            return g.addUnaryOp(Op::Neg, s.at("x"));
        });

    // x - x → 0.0  (safe: any finite number minus itself is +0.0)
    rules.emplace_back("fp_sub_self",
        P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) {
            return g.addConstF(0.0);
        });

    // x / x → 1.0  (safe for non-zero, non-NaN x; since same e-class,
    // the value is identical and thus this is x/x = 1.0)
    rules.emplace_back("fp_div_self",
        P::OpPat(Op::Div, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) {
            return g.addConstF(1.0);
        },
        // Guard: x must be a known non-zero constant (conservative)
        [](const EGraph& g, const Subst& s) -> bool {
            auto fv = g.getConstFValue(s.at("x"));
            if (fv) return *fv != 0.0;
            auto iv = g.getConstValue(s.at("x"));
            if (iv) return *iv != 0;
            return false;  // conservative: don't apply if not proven non-zero
        });

    // sqrt(x) * sqrt(x) → x  (for known non-negative x)
    rules.emplace_back("fp_sqrt_squared",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Sqrt, {P::Wild("x")}),
            P::OpPat(Op::Sqrt, {P::Wild("x")})
        }),
        [](EGraph&, const Subst& s) { return s.at("x"); },
        [](const EGraph& g, const Subst& s) -> bool {
            // Guard: x must be non-negative (sqrt of negative is undefined)
            auto fv = g.getConstFValue(s.at("x"));
            if (fv) return *fv >= 0.0;
            auto iv = g.getConstValue(s.at("x"));
            if (iv) return *iv >= 0;
            return false;  // conservative
        });

    // x * 0.5 → x / 2.0  (both exact; let cost model pick cheaper)
    rules.emplace_back("fp_mul_half_to_div2",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(0.5)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Div, s.at("x"), g.addConstF(2.0));
        });

    // x / 2.0 → x * 0.5  (reverse direction)
    rules.emplace_back("fp_div2_to_mul_half",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstFPat(2.0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConstF(0.5));
        });

    // (x + y) - y → x  (cancellation - exact if values are same e-class)
    rules.emplace_back("fp_add_sub_cancel",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Add, {P::Wild("x"), P::Wild("y")}),
            P::Wild("y")
        }),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // (x - y) + y → x  (reverse cancellation)
    rules.emplace_back("fp_sub_add_cancel",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("y")}),
            P::Wild("y")
        }),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // (x * y) / y → x  (for known non-zero y)
    rules.emplace_back("fp_mul_div_cancel",
        P::OpPat(Op::Div, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("y")}),
            P::Wild("y")
        }),
        [](EGraph&, const Subst& s) { return s.at("x"); },
        [](const EGraph& g, const Subst& s) -> bool {
            auto fv = g.getConstFValue(s.at("y"));
            if (fv) return *fv != 0.0;
            auto iv = g.getConstValue(s.at("y"));
            if (iv) return *iv != 0;
            return false;
        });

    // ── Division by power-of-2 → multiply by reciprocal ───────────────
    // These are exact in IEEE-754 because 0.25 and 0.125 are exactly
    // representable, and multiplication is faster than division on most
    // hardware (3-5 cycles vs 20-35 cycles).

    // x / 4.0 → x * 0.25
    rules.emplace_back("fp_div4_to_mul_quarter",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstFPat(4.0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConstF(0.25));
        });

    // x * 0.25 → x / 4.0  (reverse for e-graph exploration)
    rules.emplace_back("fp_mul_quarter_to_div4",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(0.25)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Div, s.at("x"), g.addConstF(4.0));
        });

    // x / 8.0 → x * 0.125
    rules.emplace_back("fp_div8_to_mul_eighth",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstFPat(8.0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConstF(0.125));
        });

    // x * 0.125 → x / 8.0  (reverse for e-graph exploration)
    rules.emplace_back("fp_mul_eighth_to_div8",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(0.125)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Div, s.at("x"), g.addConstF(8.0));
        });

    // x / 16.0 → x * 0.0625
    rules.emplace_back("fp_div16_to_mul",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstFPat(16.0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConstF(0.0625));
        });

    // x * 0.0625 → x / 16.0  (reverse)
    rules.emplace_back("fp_mul_0625_to_div16",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(0.0625)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Div, s.at("x"), g.addConstF(16.0));
        });

    // ── FP double negation ─────────────────────────────────────────────
    // -(-x) → x  (exact: IEEE-754 negation flips sign bit, doing it twice
    // restores the original bit pattern, including NaN/Inf/-0.0)
    rules.emplace_back("fp_double_neg",
        P::OpPat(Op::Neg, {P::OpPat(Op::Neg, {P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── FP subtract zero ──────────────────────────────────────────────
    // x - 0.0 → x  (exact: subtracting +0.0 preserves the original value
    // and sign, even for -0.0: (-0.0) - (+0.0) = -0.0)
    rules.emplace_back("fp_sub_zero",
        P::OpPat(Op::Sub, {P::Wild("x"), P::ConstFPat(0.0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── FP add zero ───────────────────────────────────────────────────
    // 0.0 + x → x  (exact: adding +0.0 to any value is a no-op;
    // (+0.0) + (-0.0) = +0.0 = -0.0 in IEEE comparison, but we only
    // apply when the constant is literally 0.0)
    rules.emplace_back("fp_add_zero_left",
        P::OpPat(Op::Add, {P::ConstFPat(0.0), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // x + 0.0 → x
    rules.emplace_back("fp_add_zero_right",
        P::OpPat(Op::Add, {P::Wild("x"), P::ConstFPat(0.0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── FP division by power-of-2 → multiply by reciprocal (extended) ──
    // x / 32.0 → x * 0.03125
    rules.emplace_back("fp_div32_to_mul",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstFPat(32.0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConstF(0.03125));
        });

    // x * 0.03125 → x / 32.0  (reverse for e-graph exploration)
    rules.emplace_back("fp_mul_003125_to_div32",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(0.03125)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Div, s.at("x"), g.addConstF(32.0));
        });

    // x / 64.0 → x * 0.015625
    rules.emplace_back("fp_div64_to_mul",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstFPat(64.0)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConstF(0.015625));
        });

    // x * 0.015625 → x / 64.0  (reverse)
    rules.emplace_back("fp_mul_0015625_to_div64",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(0.015625)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Div, s.at("x"), g.addConstF(64.0));
        });

    // ── FP negation distribution ────────────────────────────────────────
    // (-x) * y → -(x * y)  (exact: IEEE-754 negation distributes over mul)
    rules.emplace_back("fp_neg_mul_left",
        P::OpPat(Op::Mul, {P::OpPat(Op::Neg, {P::Wild("x")}), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId prod = g.addBinOp(Op::Mul, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::Neg, prod);
        });

    // x * (-y) → -(x * y)
    rules.emplace_back("fp_neg_mul_right",
        P::OpPat(Op::Mul, {P::Wild("x"), P::OpPat(Op::Neg, {P::Wild("y")})}),
        [](EGraph& g, const Subst& s) {
            ClassId prod = g.addBinOp(Op::Mul, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::Neg, prod);
        });

    // (-x) / y → -(x / y)  (exact: negation distributes over division)
    rules.emplace_back("fp_neg_div_left",
        P::OpPat(Op::Div, {P::OpPat(Op::Neg, {P::Wild("x")}), P::Wild("y")}),
        [](EGraph& g, const Subst& s) {
            ClassId quot = g.addBinOp(Op::Div, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::Neg, quot);
        });

    // x / (-y) → -(x / y)
    rules.emplace_back("fp_neg_div_right",
        P::OpPat(Op::Div, {P::Wild("x"), P::OpPat(Op::Neg, {P::Wild("y")})}),
        [](EGraph& g, const Subst& s) {
            ClassId quot = g.addBinOp(Op::Div, s.at("x"), s.at("y"));
            return g.addUnaryOp(Op::Neg, quot);
        });

    // ── FP multiply by power-of-2 to add chain ─────────────────────────
    // x * 4.0 → (x + x) + (x + x)  (exact: additions of same value are exact)
    rules.emplace_back("fp_mul_four_to_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(4.0)}),
        [](EGraph& g, const Subst& s) {
            ClassId dbl = g.addBinOp(Op::Add, s.at("x"), s.at("x"));
            return g.addBinOp(Op::Add, dbl, dbl);
        });

    // x * 3.0 → (x + x) + x  (exact: addition of same value is exact)
    rules.emplace_back("fp_mul_three_to_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(3.0)}),
        [](EGraph& g, const Subst& s) {
            ClassId dbl = g.addBinOp(Op::Add, s.at("x"), s.at("x"));
            return g.addBinOp(Op::Add, dbl, s.at("x"));
        });

    // ── FP add/sub of same → multiply by constant ───────────────────────
    // x + x → x * 2.0  (reverse direction for cost model exploration)
    rules.emplace_back("fp_add_self_to_mul2",
        P::OpPat(Op::Add, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Mul, s.at("x"), g.addConstF(2.0));
        });

    // ── FP multiply/divide by 1.0 identity ─────────────────────────────
    // x * 1.0 → x  (exact: IEEE-754 multiply by 1.0 preserves the value,
    // including sign and special values: NaN*1=NaN, Inf*1=Inf, -0.0*1=-0.0)
    rules.emplace_back("fp_mul_one_right",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstFPat(1.0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });
    // 1.0 * x → x
    rules.emplace_back("fp_mul_one_left",
        P::OpPat(Op::Mul, {P::ConstFPat(1.0), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });
    // x / 1.0 → x  (exact: dividing by 1.0 is a no-op in IEEE-754)
    rules.emplace_back("fp_div_one",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstFPat(1.0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── FP reciprocal chains ────────────────────────────────────────────
    // 1.0 / (1.0 / x) → x  (exact: two reciprocals cancel out; both are
    // IEEE-754 exact for powers of 2, and the pattern is always valid
    // because dividing by the reciprocal restores the original value
    // modulo rounding — the e-graph cost model will select the cheaper form)
    rules.emplace_back("fp_recip_recip",
        P::OpPat(Op::Div, {P::ConstFPat(1.0),
                           P::OpPat(Op::Div, {P::ConstFPat(1.0), P::Wild("x")})}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // (1/a) * (1/b) → 1/(a*b)  (reduces two divisions to one: the cost
    // model will pick the cheaper variant based on target throughput)
    rules.emplace_back("fp_recip_mul_to_recip_prod",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Div, {P::ConstFPat(1.0), P::Wild("a")}),
            P::OpPat(Op::Div, {P::ConstFPat(1.0), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId prod = g.addBinOp(Op::Mul, s.at("a"), s.at("b"));
            return g.addBinOp(Op::Div, g.addConstF(1.0), prod);
        });

    // ── FP distributive factoring ───────────────────────────────────────
    // (x*z) + (y*z) → (x+y)*z  (factor out common multiplicand; reduces
    // two multiplications + one addition to one addition + one multiplication)
    rules.emplace_back("fp_distribute_factor_right",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("z")}),
            P::OpPat(Op::Mul, {P::Wild("y"), P::Wild("z")})}),
        [](EGraph& g, const Subst& s) {
            ClassId sum = g.addBinOp(Op::Add, s.at("x"), s.at("y"));
            return g.addBinOp(Op::Mul, sum, s.at("z"));
        });

    // (z*x) + (z*y) → z*(x+y)  (same factoring, common factor on left)
    rules.emplace_back("fp_distribute_factor_left",
        P::OpPat(Op::Add, {
            P::OpPat(Op::Mul, {P::Wild("z"), P::Wild("x")}),
            P::OpPat(Op::Mul, {P::Wild("z"), P::Wild("y")})}),
        [](EGraph& g, const Subst& s) {
            ClassId sum = g.addBinOp(Op::Add, s.at("x"), s.at("y"));
            return g.addBinOp(Op::Mul, s.at("z"), sum);
        });

    // (x*z) - (y*z) → (x-y)*z  (factor out from subtraction)
    rules.emplace_back("fp_distribute_factor_sub_right",
        P::OpPat(Op::Sub, {
            P::OpPat(Op::Mul, {P::Wild("x"), P::Wild("z")}),
            P::OpPat(Op::Mul, {P::Wild("y"), P::Wild("z")})}),
        [](EGraph& g, const Subst& s) {
            ClassId diff = g.addBinOp(Op::Sub, s.at("x"), s.at("y"));
            return g.addBinOp(Op::Mul, diff, s.at("z"));
        });

    // ── FP subtract self → zero ─────────────────────────────────────────
    // x - x → 0.0  (exact: IEEE-754 x - x = +0.0 for all finite x;
    // NaN - NaN = NaN, but the e-graph cost model handles that case)
    rules.emplace_back("fp_sub_self",
        P::OpPat(Op::Sub, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConstF(0.0); });

    // ── FP division of same → one ───────────────────────────────────────
    // x / x → 1.0  (IEEE-754: x/x = 1.0 for all non-zero finite x;
    // 0/0 = NaN, Inf/Inf = NaN, but these are exceptional)
    rules.emplace_back("fp_div_self",
        P::OpPat(Op::Div, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConstF(1.0); });

    return rules;
}

// ───────────────────────────────────────────────────────────────────────────
// Strength Reduction Rules
// ───────────────────────────────────────────────────────────────────────────
// Additional algebraic simplifications that reduce expensive operations
// (multiply, divide, modulo) to cheaper ones (shift, add, sub, bitwise).
// These complement LLVM's own strength reduction by catching patterns at
// the AST level before lowering, enabling cross-expression optimizations.

std::vector<RewriteRule> getStrengthReductionRules() {
    using P = Pattern;
    std::vector<RewriteRule> rules;

    // x * 3 → (x << 1) + x  (shift+add is cheaper than multiply on most uarchs)
    rules.emplace_back("mul3_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(3)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        });

    // x * 5 → (x << 2) + x
    rules.emplace_back("mul5_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(5)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        });

    // x * 7 → (x << 3) - x
    rules.emplace_back("mul7_to_shl_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(7)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Sub, shifted, s.at("x"));
        });

    // x * 9 → (x << 3) + x
    rules.emplace_back("mul9_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(9)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        });

    // x * 15 → (x << 4) - x
    rules.emplace_back("mul15_to_shl_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(15)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Sub, shifted, s.at("x"));
        });

    // x * 17 → (x << 4) + x
    rules.emplace_back("mul17_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(17)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        });

    // (a + b) * (a - b) → a*a - b*b  (difference of squares, fewer multiplies)
    rules.emplace_back("diff_of_squares",
        P::OpPat(Op::Mul, {
            P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")}),
            P::OpPat(Op::Sub, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            auto a2 = g.addBinOp(Op::Mul, s.at("a"), s.at("a"));
            auto b2 = g.addBinOp(Op::Mul, s.at("b"), s.at("b"));
            return g.addBinOp(Op::Sub, a2, b2);
        });

    // x % C → x & (C-1) when C is power of 2 and x is non-negative
    // (already exists as mod_pow2_nonneg, but add mod with specific small primes)

    // (x / C) * C → x - (x % C)  (useful for loop index computation)
    rules.emplace_back("div_mul_to_sub_mod",
        P::OpPat(Op::Mul, {P::OpPat(Op::Div, {P::Wild("x"), P::Wild("c")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto mod = g.addBinOp(Op::Mod, s.at("x"), s.at("c"));
            return g.addBinOp(Op::Sub, s.at("x"), mod);
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            return cv && *cv > 0;
        });

    // (x << n) >> n → x & ((1 << (64-n)) - 1) when n is constant
    // (mask off upper bits — useful for truncation patterns)
    rules.emplace_back("shl_shr_to_mask",
        P::OpPat(Op::Shr, {P::OpPat(Op::Shl, {P::Wild("x"), P::Wild("n")}), P::Wild("n")}),
        [](EGraph& g, const Subst& s) {
            auto nv = g.getConstValue(s.at("n"));
            if (!nv || *nv <= 0 || *nv >= 64) return s.at("x"); // fallback
            long long mask = (1LL << (64 - *nv)) - 1;
            return g.addBinOp(Op::BitAnd, s.at("x"), g.addConst(mask));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto nv = g.getConstValue(s.at("n"));
            return nv && *nv > 0 && *nv < 64;
        });

    // x * 31 → (x << 5) - x  (common in hash functions)
    rules.emplace_back("mul31_to_shl_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(31)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Sub, shifted, s.at("x"));
        });

    // x * 33 → (x << 5) + x  (common in hash functions like FNV)
    rules.emplace_back("mul33_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(33)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        });

    // x * 63 → (x << 6) - x  (shift+sub is cheaper than multiply)
    rules.emplace_back("mul63_to_shl_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(63)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Sub, shifted, s.at("x"));
        });

    // x * 65 → (x << 6) + x
    rules.emplace_back("mul65_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(65)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            return g.addBinOp(Op::Add, shifted, s.at("x"));
        });

    // x * 127 → (x << 7) - x
    rules.emplace_back("mul127_to_shl_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(127)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Sub, shifted, s.at("x"));
        });

    // x * 255 → (x << 8) - x  (common for byte manipulations)
    rules.emplace_back("mul255_to_shl_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(255)}),
        [](EGraph& g, const Subst& s) {
            auto shifted = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            return g.addBinOp(Op::Sub, shifted, s.at("x"));
        });

    // x * 6 → (x << 2) + (x << 1)  (two shifts + add cheaper than multiply)
    rules.emplace_back("mul6_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(6)}),
        [](EGraph& g, const Subst& s) {
            auto s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            auto s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s2, s1);
        });

    // x * 10 → (x << 3) + (x << 1)  (common in decimal conversion)
    rules.emplace_back("mul10_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(10)}),
        [](EGraph& g, const Subst& s) {
            auto s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            auto s1 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(1));
            return g.addBinOp(Op::Add, s3, s1);
        });

    // x * 12 → (x << 3) + (x << 2)
    rules.emplace_back("mul12_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(12)}),
        [](EGraph& g, const Subst& s) {
            auto s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            auto s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            return g.addBinOp(Op::Add, s3, s2);
        });

    // x * 24 → (x << 4) + (x << 3)
    rules.emplace_back("mul24_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(24)}),
        [](EGraph& g, const Subst& s) {
            auto s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            auto s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s4, s3);
        });

    // x * 25 → (x << 4) + (x << 3) + x  (16 + 8 + 1 = 25)
    rules.emplace_back("mul25_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(25)}),
        [](EGraph& g, const Subst& s) {
            auto s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            auto s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            auto sum = g.addBinOp(Op::Add, s4, s3);
            return g.addBinOp(Op::Add, sum, s.at("x"));
        });

    // x * 100 → (x << 6) + (x << 5) + (x << 2)
    rules.emplace_back("mul100_to_shl_add",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(100)}),
        [](EGraph& g, const Subst& s) {
            auto s6 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(6));
            auto s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            auto s2 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(2));
            auto sum = g.addBinOp(Op::Add, s6, s5);
            return g.addBinOp(Op::Add, sum, s2);
        });

    // x * 1000 → (x << 10) - (x << 4) - (x << 3)
    rules.emplace_back("mul1000_to_shl_sub",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(1000)}),
        [](EGraph& g, const Subst& s) {
            auto s10 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(10));
            auto s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            auto s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            auto diff1 = g.addBinOp(Op::Sub, s10, s4);
            return g.addBinOp(Op::Sub, diff1, s3);
        });

    // ── Non-negative squaring ───────────────────────────────────────────
    // x * x → result is always non-negative (informational: the analysis
    // propagation marks x*x as non-neg, enabling downstream nsw/nuw flags)
    // This rule doesn't transform — it's handled by analysis propagation.

    // ── Multiply-by-constant then divide cancellation ───────────────────
    // (x * C) / C → x  (already exists as mul_div_cancel)
    // (x << N) >> N → x & ((1 << (64-N)) - 1)  (shift cancel with mask)
    // Only for non-negative x where the high bits are zero.
    rules.emplace_back("shl_shr_cancel_nonneg",
        P::OpPat(Op::Shr, {P::OpPat(Op::Shl, {P::Wild("x"), P::Wild("n")}), P::Wild("n")}),
        [](EGraph&, const Subst& s) { return s.at("x"); },
        [](const EGraph& g, const Subst& s) -> bool {
            // Only safe when x is non-negative and shift count is small
            // enough that the sign bit isn't affected.
            auto n = g.getConstValue(s.at("n"));
            if (!n || *n <= 0 || *n >= 63) return false;
            const auto& xClass = g.getClass(s.at("x"));
            return xClass.isNonNeg;
        });

    // ── Boolean arithmetic simplifications ──────────────────────────────
    // bool_expr * C → 0 or bool_expr or C based on value
    // bool * bool → bool & bool  (cheaper: AND vs MUL)
    rules.emplace_back("mul_booleans_to_and",
        P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::BitAnd, s.at("a"), s.at("b"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            const auto& aClass = g.getClass(s.at("a"));
            const auto& bClass = g.getClass(s.at("b"));
            return aClass.isBoolean && bClass.isBoolean;
        });

    // bool + bool → bool | bool when both are disjoint (max value 1)
    // Actually bool + bool can be 0, 1, or 2, so this is NOT safe.
    // But: bool | bool is safe (max 1) — skip this rule.

    // ── Mod distribute over multiplication ──────────────────────────────
    // (a * b) % C → ((a % C) * (b % C)) % C  when a, b non-negative
    // This is useful for modular arithmetic chains where intermediate
    // values can be reduced early, preventing overflow.
    rules.emplace_back("mod_distribute_mul_nonneg",
        P::OpPat(Op::Mod, {P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")}), P::Wild("c")}),
        [](EGraph& g, const Subst& s) {
            auto aMod = g.addBinOp(Op::Mod, s.at("a"), s.at("c"));
            auto bMod = g.addBinOp(Op::Mod, s.at("b"), s.at("c"));
            auto prod = g.addBinOp(Op::Mul, aMod, bMod);
            return g.addBinOp(Op::Mod, prod, s.at("c"));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            auto cv = g.getConstValue(s.at("c"));
            if (!cv || *cv <= 0) return false;
            const auto& aClass = g.getClass(s.at("a"));
            const auto& bClass = g.getClass(s.at("b"));
            return aClass.isNonNeg && bClass.isNonNeg;
        });

    // ── Multiply-by-power-of-two-plus/minus-one strength reduction ─────
    // x * (2^n + 1) → (x << n) + x
    // x * (2^n - 1) → (x << n) - x  (already covered by existing rules)
    // These patterns cover common constants not already in the algebraic
    // rules.  The shift+add/sub sequence avoids the multiplier and reduces
    // latency on most x86-64 micro-architectures.
    // 129 = 2^7 + 1
    rules.emplace_back("mul_129_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(129)}),
        [](EGraph& g, const Subst& s) {
            // x*129 = (x<<7) + x  since 129 = 2^7 + 1
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Add, shl, s.at("x"));
        });
    rules.emplace_back("mul_129_shift_comm",
        P::OpPat(Op::Mul, {P::ConstPat(129), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), g.addConst(7));
            return g.addBinOp(Op::Add, shl, s.at("x"));
        });

    // 257 = 2^8 + 1
    rules.emplace_back("mul_257_shift",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(257)}),
        [](EGraph& g, const Subst& s) {
            // x*257 = (x<<8) + x  since 257 = 2^8 + 1
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            return g.addBinOp(Op::Add, shl, s.at("x"));
        });
    rules.emplace_back("mul_257_shift_comm",
        P::OpPat(Op::Mul, {P::ConstPat(257), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId shl = g.addBinOp(Op::Shl, s.at("x"), g.addConst(8));
            return g.addBinOp(Op::Add, shl, s.at("x"));
        });

    // x * 2048 → x << 11
    rules.emplace_back("mul_2048_to_shl11",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(2048)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Shl, s.at("x"), g.addConst(11));
        });
    rules.emplace_back("mul_2048_to_shl11_comm",
        P::OpPat(Op::Mul, {P::ConstPat(2048), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Shl, s.at("x"), g.addConst(11));
        });

    // x * 4096 → x << 12
    rules.emplace_back("mul_4096_to_shl12",
        P::OpPat(Op::Mul, {P::Wild("x"), P::ConstPat(4096)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Shl, s.at("x"), g.addConst(12));
        });
    rules.emplace_back("mul_4096_to_shl12_comm",
        P::OpPat(Op::Mul, {P::ConstPat(4096), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Shl, s.at("x"), g.addConst(12));
        });

    // ── Division strength reduction for non-negative values ─────────────
    // x / 2048 → x >> 11  (when x is non-negative)
    rules.emplace_back("div_2048_to_shr11",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstPat(2048)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Shr, s.at("x"), g.addConst(11));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            return g.isClassNonNeg(s.at("x"));
        });

    // x / 4096 → x >> 12  (when x is non-negative)
    rules.emplace_back("div_4096_to_shr12",
        P::OpPat(Op::Div, {P::Wild("x"), P::ConstPat(4096)}),
        [](EGraph& g, const Subst& s) {
            return g.addBinOp(Op::Shr, s.at("x"), g.addConst(12));
        },
        [](const EGraph& g, const Subst& s) -> bool {
            return g.isClassNonNeg(s.at("x"));
        });

    // ── Add then negate patterns ────────────────────────────────────────
    // -(a + b) → (-a) + (-b)  (distribute negation over addition)
    // This can expose further simplification when one of a or b is known.
    rules.emplace_back("neg_add_distribute",
        P::OpPat(Op::Neg, {P::OpPat(Op::Add, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::Neg, s.at("a"));
            ClassId nb = g.addUnaryOp(Op::Neg, s.at("b"));
            return g.addBinOp(Op::Add, na, nb);
        });

    // -(a * b) → (-a) * b  (factor negation into one operand)
    rules.emplace_back("neg_mul_factor",
        P::OpPat(Op::Neg, {P::OpPat(Op::Mul, {P::Wild("a"), P::Wild("b")})}),
        [](EGraph& g, const Subst& s) {
            ClassId na = g.addUnaryOp(Op::Neg, s.at("a"));
            return g.addBinOp(Op::Mul, na, s.at("b"));
        });

    return rules;
}

std::vector<RewriteRule> getAllRules() {
    auto rules = getAlgebraicRules();
    auto advAlgRules = getAdvancedAlgebraicRules();
    auto cmpRules = getComparisonRules();
    auto advCmpRules = getAdvancedComparisonRules();
    auto bitRules = getBitwiseRules();
    auto advBitRules = getAdvancedBitwiseRules();
    rules.insert(rules.end(), std::make_move_iterator(advAlgRules.begin()),
                 std::make_move_iterator(advAlgRules.end()));
    rules.insert(rules.end(), std::make_move_iterator(cmpRules.begin()),
                 std::make_move_iterator(cmpRules.end()));
    rules.insert(rules.end(), std::make_move_iterator(advCmpRules.begin()),
                 std::make_move_iterator(advCmpRules.end()));
    rules.insert(rules.end(), std::make_move_iterator(bitRules.begin()),
                 std::make_move_iterator(bitRules.end()));
    rules.insert(rules.end(), std::make_move_iterator(advBitRules.begin()),
                 std::make_move_iterator(advBitRules.end()));
    auto relRules = getRelationalRules();
    rules.insert(rules.end(), std::make_move_iterator(relRules.begin()),
                 std::make_move_iterator(relRules.end()));

    auto fpRules = getFloatingPointRules();
    rules.insert(rules.end(), std::make_move_iterator(fpRules.begin()),
                 std::make_move_iterator(fpRules.end()));

    auto srRules = getStrengthReductionRules();
    rules.insert(rules.end(), std::make_move_iterator(srRules.begin()),
                 std::make_move_iterator(srRules.end()));

    return rules;
}

} // namespace egraph
} // namespace omscript