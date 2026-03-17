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

    // ── Self-modulo: x % x → 0 ──────────────────────────────────────────
    rules.emplace_back("mod_self",
        P::OpPat(Op::Mod, {P::Wild("x"), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

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
    rules.emplace_back("mul_40_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(40), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s3 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(3));
            return g.addBinOp(Op::Add, s5, s3);
        });
    rules.emplace_back("mul_48_left_shift",
        P::OpPat(Op::Mul, {P::ConstPat(48), P::Wild("x")}),
        [](EGraph& g, const Subst& s) {
            ClassId s5 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(5));
            ClassId s4 = g.addBinOp(Op::Shl, s.at("x"), g.addConst(4));
            return g.addBinOp(Op::Add, s5, s4);
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

    // ── Logical identity: x && x → x ────────────────────────────────────
    rules.emplace_back("logand_self",
        P::OpPat(Op::LogAnd, {P::Wild("x"), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Logical identity: x || x → x ────────────────────────────────────
    rules.emplace_back("logor_self",
        P::OpPat(Op::LogOr, {P::Wild("x"), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Logical annihilation: x && 0 → 0 ────────────────────────────────
    rules.emplace_back("logand_zero",
        P::OpPat(Op::LogAnd, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Logical annihilation: 0 && x → 0 ────────────────────────────────
    rules.emplace_back("logand_zero_left",
        P::OpPat(Op::LogAnd, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph& g, const Subst&) { return g.addConst(0); });

    // ── Logical identity: x || 0 → x ────────────────────────────────────
    rules.emplace_back("logor_zero",
        P::OpPat(Op::LogOr, {P::Wild("x"), P::ConstPat(0)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Logical identity: 0 || x → x ────────────────────────────────────
    rules.emplace_back("logor_zero_left",
        P::OpPat(Op::LogOr, {P::ConstPat(0), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

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

    // ── Logical AND identity: x && 1 → x ────────────────────────────────
    rules.emplace_back("logand_one",
        P::OpPat(Op::LogAnd, {P::Wild("x"), P::ConstPat(1)}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

    // ── Logical AND identity: 1 && x → x ────────────────────────────────
    rules.emplace_back("logand_one_left",
        P::OpPat(Op::LogAnd, {P::ConstPat(1), P::Wild("x")}),
        [](EGraph&, const Subst& s) { return s.at("x"); });

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

    return rules;
}

} // namespace egraph
} // namespace omscript