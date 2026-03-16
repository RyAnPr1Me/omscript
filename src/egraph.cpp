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
        return 1.0;

    // Shifts — 1 cycle but slightly prefer over multiply
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
    ClassId id = static_cast<ClassId>(classes_.size());
    parent_.push_back(id);

    EClass cls;
    cls.id = id;
    cls.nodes.push_back(node);
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

std::vector<std::pair<ClassId, Subst>> EGraph::match(const Pattern& pat) const {
    std::vector<std::pair<ClassId, Subst>> results;
    for (ClassId i = 0; i < classes_.size(); ++i) {
        ClassId canonical = const_cast<EGraph*>(this)->find(i);
        if (canonical != i) continue; // Skip non-canonical

        Subst subst;
        if (matchClass(pat, i, subst)) {
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
                case Op::Shl:    result = lv << rv; break;
                case Op::Shr:    result = lv >> rv; break;
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

    // Iterative cost computation: repeat until stable.
    // We use a simple fixed-point iteration (like Bellman-Ford).
    // Each iteration, for each class, we pick the node with the lowest
    // total cost (node cost + sum of children's best costs).
    bool changed = true;
    for (int pass = 0; pass < 100 && changed; ++pass) {
        changed = false;
        for (ClassId i = 0; i < classes_.size(); ++i) {
            ClassId cls = find(i);
            if (cls != i) continue;

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

    return rules;
}

std::vector<RewriteRule> getAllRules() {
    auto rules = getAlgebraicRules();
    auto cmpRules = getComparisonRules();
    auto bitRules = getBitwiseRules();
    rules.insert(rules.end(), std::make_move_iterator(cmpRules.begin()),
                 std::make_move_iterator(cmpRules.end()));
    rules.insert(rules.end(), std::make_move_iterator(bitRules.begin()),
                 std::make_move_iterator(bitRules.end()));
    return rules;
}

} // namespace egraph
} // namespace omscript
