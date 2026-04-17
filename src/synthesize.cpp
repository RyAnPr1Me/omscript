/// @file synthesize.cpp
/// @brief std::synthesize — OmScript compile-time program synthesis engine.
///
/// Architecture:
///
///   1. SynthesisEngine::synthesize()
///        → resolveOps()            picks allowed SynthOp set
///        → enumerate()             BFS over expression trees, depth 0..maxDepth
///        → verifyAll()             reject any candidate that fails an example
///        → nodeCost()              score survivors
///        → best candidate stored in SynthResult
///
///   2. lowerToAST()
///        → recursive SynthNode → unique_ptr<Expression> conversion
///
///   3. runSynthesisPass()
///        → scans every FunctionDecl in the Program
///        → detects: body = { return std__synthesize(examples_literal[, ...]); }
///        → extracts examples from ARRAY_EXPR literals
///        → calls synthesize()
///        → on success: replaces body with { return <synthesized_expr>; }
///        → marks function @pure + @const_eval

#include "synthesize.h"
#include "ast.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace omscript {

// ═══════════════════════════════════════════════════════════════════════════
// SynthOp helpers
// ═══════════════════════════════════════════════════════════════════════════

static const std::vector<std::string>& allOpNames() {
    static std::vector<std::string> v = {
        "+","-","*","/","%","&","|","^","<<",">>","**","min","max","abs","neg","~"
    };
    return v;
}

static SynthOp opFromName(const std::string& s) {
    if (s == "+")   return SynthOp::ADD;
    if (s == "-")   return SynthOp::SUB;
    if (s == "*")   return SynthOp::MUL;
    if (s == "/")   return SynthOp::DIV;
    if (s == "%")   return SynthOp::MOD;
    if (s == "&")   return SynthOp::AND;
    if (s == "|")   return SynthOp::OR;
    if (s == "^")   return SynthOp::XOR;
    if (s == "<<")  return SynthOp::SHL;
    if (s == ">>")  return SynthOp::SHR;
    if (s == "**")  return SynthOp::POW;
    if (s == "min") return SynthOp::MIN2;
    if (s == "max") return SynthOp::MAX2;
    if (s == "abs") return SynthOp::ABS;
    if (s == "neg" || s == "-u") return SynthOp::NEG;
    if (s == "~")   return SynthOp::NOT;
    return SynthOp::ADD; // fallback
}

static bool isBinaryOp(SynthOp op) {
    switch (op) {
        case SynthOp::ADD: case SynthOp::SUB: case SynthOp::MUL:
        case SynthOp::DIV: case SynthOp::MOD: case SynthOp::AND:
        case SynthOp::OR:  case SynthOp::XOR: case SynthOp::SHL:
        case SynthOp::SHR: case SynthOp::POW:
        case SynthOp::MIN2: case SynthOp::MAX2:
            return true;
        default: return false;
    }
}

static bool isUnaryOp(SynthOp op) {
    return op == SynthOp::NEG || op == SynthOp::ABS || op == SynthOp::NOT;
}

static const std::string& opToString(SynthOp op) {
    static const std::string names[] = {
        "PARAM","CONST","neg","abs","~",
        "+","-","*","/","%","&","|","^","<<",">>","**","min","max"
    };
    return names[static_cast<int>(op)];
}

// ═══════════════════════════════════════════════════════════════════════════
// SynthNode clone helper
// ═══════════════════════════════════════════════════════════════════════════

static std::unique_ptr<SynthNode> cloneNode(const SynthNode* n) {
    if (!n) return nullptr;
    auto c = std::make_unique<SynthNode>(n->op, n->value);
    c->left  = cloneNode(n->left.get());
    c->right = cloneNode(n->right.get());
    return c;
}

// ═══════════════════════════════════════════════════════════════════════════
// eval — safe evaluation of a SynthNode on concrete inputs
// ═══════════════════════════════════════════════════════════════════════════

int64_t SynthesisEngine::eval(const SynthNode* node,
                               const std::vector<int64_t>& inputs) {
    if (!node) return 0;
    switch (node->op) {
    case SynthOp::PARAM:
        if (node->value >= 0 && static_cast<size_t>(node->value) < inputs.size())
            return inputs[static_cast<size_t>(node->value)];
        return 0;
    case SynthOp::CONST:
        return static_cast<int64_t>(node->value);
    case SynthOp::NEG:
        return -eval(node->left.get(), inputs);
    case SynthOp::ABS: {
        int64_t v = eval(node->left.get(), inputs);
        return v < 0 ? -v : v;
    }
    case SynthOp::NOT:
        return ~eval(node->left.get(), inputs);
    case SynthOp::ADD:
        return eval(node->left.get(), inputs) + eval(node->right.get(), inputs);
    case SynthOp::SUB:
        return eval(node->left.get(), inputs) - eval(node->right.get(), inputs);
    case SynthOp::MUL:
        return eval(node->left.get(), inputs) * eval(node->right.get(), inputs);
    case SynthOp::DIV: {
        int64_t rhs = eval(node->right.get(), inputs);
        if (rhs == 0) return 0; // safe division
        int64_t lhs = eval(node->left.get(), inputs);
        // Guard against INT64_MIN / -1 (undefined behaviour → SIGFPE on x86-64).
        if (lhs == INT64_MIN && rhs == -1) return INT64_MIN;
        return lhs / rhs;
    }
    case SynthOp::MOD: {
        int64_t rhs = eval(node->right.get(), inputs);
        if (rhs == 0) return 0; // safe modulo
        int64_t lhs = eval(node->left.get(), inputs);
        // Guard against INT64_MIN % -1 (undefined behaviour → SIGFPE on x86-64).
        if (lhs == INT64_MIN && rhs == -1) return 0;
        return lhs % rhs;
    }
    case SynthOp::AND:
        return eval(node->left.get(), inputs) & eval(node->right.get(), inputs);
    case SynthOp::OR:
        return eval(node->left.get(), inputs) | eval(node->right.get(), inputs);
    case SynthOp::XOR:
        return eval(node->left.get(), inputs) ^ eval(node->right.get(), inputs);
    case SynthOp::SHL: {
        int64_t shift = eval(node->right.get(), inputs) & 62;
        if (shift < 0) shift = 0;
        return static_cast<int64_t>(
            static_cast<uint64_t>(eval(node->left.get(), inputs)) << static_cast<unsigned>(shift));
    }
    case SynthOp::SHR: {
        int64_t shift = eval(node->right.get(), inputs) & 62;
        if (shift < 0) shift = 0;
        return eval(node->left.get(), inputs) >> static_cast<int>(shift);
    }
    case SynthOp::POW: {
        int64_t base = eval(node->left.get(), inputs);
        int64_t exp  = eval(node->right.get(), inputs);
        if (exp < 0) return (base == 1) ? 1 : 0;
        if (exp > 30) exp = 30; // guard against huge exponents
        int64_t result = 1;
        while (exp > 0) {
            if (exp & 1) result *= base;
            base *= base;
            exp >>= 1;
        }
        return result;
    }
    case SynthOp::MIN2: {
        int64_t a = eval(node->left.get(), inputs);
        int64_t b = eval(node->right.get(), inputs);
        return a < b ? a : b;
    }
    case SynthOp::MAX2: {
        int64_t a = eval(node->left.get(), inputs);
        int64_t b = eval(node->right.get(), inputs);
        return a > b ? a : b;
    }
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// verifyAll — check a candidate against every example
// ═══════════════════════════════════════════════════════════════════════════

bool SynthesisEngine::verifyAll(const SynthNode* node,
                                 const std::vector<SynthExample>& examples) {
    for (const auto& ex : examples) {
        if (eval(node, ex.inputs) != ex.output)
            return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// nodeCount / nodeDepth / nodeCost
// ═══════════════════════════════════════════════════════════════════════════

int SynthesisEngine::nodeCount(const SynthNode* n) {
    if (!n) return 0;
    return 1 + nodeCount(n->left.get()) + nodeCount(n->right.get());
}

int SynthesisEngine::nodeDepth(const SynthNode* n) {
    if (!n) return 0;
    return 1 + std::max(nodeDepth(n->left.get()), nodeDepth(n->right.get()));
}

double SynthesisEngine::nodeCost(const SynthNode* n, bool preferSize) {
    int nc = nodeCount(n);
    int nd = nodeDepth(n);
    // Size mode: minimise node count (code size proxy).
    // Speed mode: minimise depth (critical-path latency proxy) + small node penalty.
    if (preferSize)
        return static_cast<double>(nc);
    return static_cast<double>(nd) + static_cast<double>(nc) * 0.2;
}

// ═══════════════════════════════════════════════════════════════════════════
// resolveOps — parse op name list into SynthOp set
// ═══════════════════════════════════════════════════════════════════════════

std::vector<SynthOp> SynthesisEngine::resolveOps(const SynthConfig& cfg) {
    // Default: all ops.
    const std::vector<std::string>& src = cfg.ops.empty() ? allOpNames() : cfg.ops;
    std::vector<SynthOp> ops;
    ops.reserve(src.size());
    for (const auto& s : src)
        ops.push_back(opFromName(s));
    // Deduplicate.
    std::sort(ops.begin(), ops.end());
    ops.erase(std::unique(ops.begin(), ops.end()), ops.end());
    return ops;
}

// ═══════════════════════════════════════════════════════════════════════════
// enumerate — BFS bottom-up enumeration of expression trees
//
// Strategy:
//   - depth 0: PARAM (0..nParams-1) + small constants (-1, 0, 1, 2)
//   - depth d: combine depth-d-1 subtrees with each allowed op
//              (prune dominated expressions using output fingerprint)
// ═══════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════
// enumerate — BFS bottom-up enumeration of expression trees
//
// Strategy:
//   - depth 0: PARAM (0..nParams-1) + small constants
//   - depth d: combine depth-(d-1) subtrees with each allowed op
//   - Hard cap of kMaxPerLevel distinct (fingerprinted) nodes per depth level
//     to prevent combinatorial explosion at depth ≥ 2.
//   - Global cap of `maxCandidates` total generated nodes.
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int kMaxPerLevel = 512;  ///< Max distinct expressions per depth level

void SynthesisEngine::enumerate(
    int                              maxDepth,
    int                              nParams,
    const std::vector<SynthOp>&      allowedOps,
    std::vector<std::unique_ptr<SynthNode>>& out) {

    // We use TWO separate fingerprint probe sets to reduce hash collisions.
    // The inputs for param i are varied across both probes.
    auto makeProbe = [&](int seed) -> std::vector<int64_t> {
        std::vector<int64_t> p(static_cast<size_t>(nParams));
        for (int i = 0; i < nParams; ++i)
            p[static_cast<size_t>(i)] = static_cast<int64_t>(seed * 7 + i * 13 + 1);
        return p;
    };
    const auto probe1 = makeProbe(1);
    const auto probe2 = makeProbe(5);

    std::unordered_set<std::string> seen;
    seen.reserve(kMaxPerLevel * (maxDepth + 1) * 2);

    auto fingerprint = [&](const SynthNode* n) -> std::string {
        int64_t r1 = eval(n, probe1);
        int64_t r2 = eval(n, probe2);
        // Pack both into a short string key.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld|%lld",
                      static_cast<long long>(r1), static_cast<long long>(r2));
        return buf;
    };

    // `byDepth[d]` holds raw pointers into `out` at depth exactly d.
    std::vector<std::vector<const SynthNode*>> byDepth(static_cast<size_t>(maxDepth + 1));

    auto addIfNew = [&](std::unique_ptr<SynthNode> node, int depth) -> bool {
        if (static_cast<int>(byDepth[static_cast<size_t>(depth)].size()) >= kMaxPerLevel)
            return false;
        std::string fp = fingerprint(node.get());
        if (!seen.insert(fp).second) return false;
        const SynthNode* raw = node.get();
        out.push_back(std::move(node));
        byDepth[static_cast<size_t>(depth)].push_back(raw);
        return true;
    };

    // ── Depth-0 leaves ──────────────────────────────────────────────────
    // Parameters
    for (int i = 0; i < nParams; ++i)
        addIfNew(std::make_unique<SynthNode>(SynthOp::PARAM, i), 0);
    // Small constants
    for (int c : {-1, 0, 1, 2, 3, 4, 8, 16})
        addIfNew(std::make_unique<SynthNode>(SynthOp::CONST, c), 0);

    if (maxDepth <= 0) return;

    // ── Bottom-up enumeration ───────────────────────────────────────────
    for (int d = 1; d <= maxDepth; ++d) {
        for (SynthOp op : allowedOps) {
            if (isUnaryOp(op)) {
                for (const SynthNode* child : byDepth[static_cast<size_t>(d - 1)]) {
                    if (static_cast<int>(byDepth[static_cast<size_t>(d)].size()) >= kMaxPerLevel)
                        break;
                    auto n = std::make_unique<SynthNode>(op);
                    n->left = cloneNode(child);
                    addIfNew(std::move(n), d);
                }
            } else if (isBinaryOp(op)) {
                // Combine subtrees from depths d1 and d2 where max(d1,d2) == d-1.
                for (int d1 = 0; d1 < d && static_cast<int>(byDepth[static_cast<size_t>(d)].size()) < kMaxPerLevel; ++d1) {
                    for (int d2 = 0; d2 < d && static_cast<int>(byDepth[static_cast<size_t>(d)].size()) < kMaxPerLevel; ++d2) {
                        if (std::max(d1, d2) != d - 1) continue;
                        const auto& lhsVec = byDepth[static_cast<size_t>(d1)];
                        const auto& rhsVec = byDepth[static_cast<size_t>(d2)];
                        for (size_t li = 0; li < lhsVec.size() && static_cast<int>(byDepth[static_cast<size_t>(d)].size()) < kMaxPerLevel; ++li) {
                            for (size_t ri = 0; ri < rhsVec.size() && static_cast<int>(byDepth[static_cast<size_t>(d)].size()) < kMaxPerLevel; ++ri) {
                                const SynthNode* lhs = lhsVec[li];
                                const SynthNode* rhs = rhsVec[ri];
                                // Prune symmetric duplicates for commutative ops.
                                const bool commutative =
                                    (op == SynthOp::ADD || op == SynthOp::MUL ||
                                     op == SynthOp::AND || op == SynthOp::OR  ||
                                     op == SynthOp::XOR || op == SynthOp::MIN2 ||
                                     op == SynthOp::MAX2);
                                if (commutative && d1 == d2 && li > ri) continue;
                                auto n = std::make_unique<SynthNode>(op);
                                n->left  = cloneNode(lhs);
                                n->right = cloneNode(rhs);
                                addIfNew(std::move(n), d);
                            }
                        }
                    }
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// synthesize — main entry point
// ═══════════════════════════════════════════════════════════════════════════

std::optional<SynthResult> SynthesisEngine::synthesize(
    int                              nParams,
    const std::vector<SynthExample>& examples,
    const SynthConfig&               cfg) const {

    if (examples.empty() || nParams < 0) return std::nullopt;

    const int maxD = std::min(std::max(cfg.maxDepth, 1), 8);
    std::vector<SynthOp> ops = resolveOps(cfg);

    // Generate candidates.
    std::vector<std::unique_ptr<SynthNode>> candidates;
    candidates.reserve(std::min(cfg.maxCandidates, 200000));
    enumerate(maxD, nParams, ops, candidates);

    // Verify + score.
    SynthResult best;
    best.cost = std::numeric_limits<double>::infinity();
    bool found = false;
    const bool preferSize = cfg.preferSize;

    int checked = 0;
    for (auto& cand : candidates) {
        if (checked >= cfg.maxCandidates) break;
        ++checked;

        if (!verifyAll(cand.get(), examples)) continue;

        double cost = nodeCost(cand.get(), preferSize);
        if (!found || cost < best.cost) {
            best.cost  = cost;
            best.depth = nodeDepth(cand.get());
            best.nodes = nodeCount(cand.get());
            best.firstOutput = eval(cand.get(), examples[0].inputs);
            best.expr  = cloneNode(cand.get());
            found = true;
        }
    }

    if (!found) return std::nullopt;
    return std::optional<SynthResult>(std::move(best));
}

// ═══════════════════════════════════════════════════════════════════════════
// render — print SynthNode as OmScript expression string
// ═══════════════════════════════════════════════════════════════════════════

std::string SynthesisEngine::render(const SynthNode* node,
                                     const std::vector<std::string>& paramNames) {
    if (!node) return "0";
    switch (node->op) {
    case SynthOp::PARAM: {
        int idx = node->value;
        if (!paramNames.empty() && idx >= 0 && static_cast<size_t>(idx) < paramNames.size())
            return paramNames[static_cast<size_t>(idx)];
        return "p" + std::to_string(idx);
    }
    case SynthOp::CONST:
        return std::to_string(node->value);
    case SynthOp::NEG:
        return "(-" + render(node->left.get(), paramNames) + ")";
    case SynthOp::ABS:
        return "abs(" + render(node->left.get(), paramNames) + ")";
    case SynthOp::NOT:
        return "(~" + render(node->left.get(), paramNames) + ")";
    case SynthOp::ADD:
        return "(" + render(node->left.get(), paramNames) + " + "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::SUB:
        return "(" + render(node->left.get(), paramNames) + " - "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::MUL:
        return "(" + render(node->left.get(), paramNames) + " * "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::DIV:
        return "(" + render(node->left.get(), paramNames) + " / "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::MOD:
        return "(" + render(node->left.get(), paramNames) + " % "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::AND:
        return "(" + render(node->left.get(), paramNames) + " & "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::OR:
        return "(" + render(node->left.get(), paramNames) + " | "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::XOR:
        return "(" + render(node->left.get(), paramNames) + " ^ "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::SHL:
        return "(" + render(node->left.get(), paramNames) + " << "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::SHR:
        return "(" + render(node->left.get(), paramNames) + " >> "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::POW:
        return "(" + render(node->left.get(), paramNames) + " ** "
                   + render(node->right.get(), paramNames) + ")";
    case SynthOp::MIN2:
        return "min(" + render(node->left.get(), paramNames) + ", "
                      + render(node->right.get(), paramNames) + ")";
    case SynthOp::MAX2:
        return "max(" + render(node->left.get(), paramNames) + ", "
                      + render(node->right.get(), paramNames) + ")";
    }
    return "0";
}

// ═══════════════════════════════════════════════════════════════════════════
// lowerToAST — convert SynthNode tree → unique_ptr<Expression>
// ═══════════════════════════════════════════════════════════════════════════

std::unique_ptr<Expression> SynthesisEngine::lowerToAST(
    const SynthNode*                node,
    const std::vector<std::string>& paramNames) {

    if (!node) {
        return std::make_unique<LiteralExpr>(static_cast<long long>(0));
    }

    switch (node->op) {
    case SynthOp::PARAM: {
        const int idx = node->value;
        if (!paramNames.empty() && idx >= 0 && static_cast<size_t>(idx) < paramNames.size())
            return std::make_unique<IdentifierExpr>(paramNames[static_cast<size_t>(idx)]);
        return std::make_unique<IdentifierExpr>("p" + std::to_string(idx));
    }
    case SynthOp::CONST:
        return std::make_unique<LiteralExpr>(static_cast<long long>(node->value));

    case SynthOp::NEG:
        return std::make_unique<UnaryExpr>("-", lowerToAST(node->left.get(), paramNames));
    case SynthOp::ABS: {
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(lowerToAST(node->left.get(), paramNames));
        return std::make_unique<CallExpr>("abs", std::move(args));
    }
    case SynthOp::NOT:
        return std::make_unique<UnaryExpr>("~", lowerToAST(node->left.get(), paramNames));

    case SynthOp::ADD:
        return std::make_unique<BinaryExpr>("+",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::SUB:
        return std::make_unique<BinaryExpr>("-",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::MUL:
        return std::make_unique<BinaryExpr>("*",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::DIV:
        return std::make_unique<BinaryExpr>("/",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::MOD:
        return std::make_unique<BinaryExpr>("%",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::AND:
        return std::make_unique<BinaryExpr>("&",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::OR:
        return std::make_unique<BinaryExpr>("|",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::XOR:
        return std::make_unique<BinaryExpr>("^",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::SHL:
        return std::make_unique<BinaryExpr>("<<",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::SHR:
        return std::make_unique<BinaryExpr>(">>",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::POW:
        return std::make_unique<BinaryExpr>("**",
            lowerToAST(node->left.get(),  paramNames),
            lowerToAST(node->right.get(), paramNames));
    case SynthOp::MIN2: {
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(lowerToAST(node->left.get(),  paramNames));
        args.push_back(lowerToAST(node->right.get(), paramNames));
        return std::make_unique<CallExpr>("min", std::move(args));
    }
    case SynthOp::MAX2: {
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(lowerToAST(node->left.get(),  paramNames));
        args.push_back(lowerToAST(node->right.get(), paramNames));
        return std::make_unique<CallExpr>("max", std::move(args));
    }
    }
    return std::make_unique<LiteralExpr>(static_cast<long long>(0));
}

// ═══════════════════════════════════════════════════════════════════════════
// runSynthesisPass — whole-program body synthesis
// ═══════════════════════════════════════════════════════════════════════════

/// Try to extract I/O examples from a CallExpr that is
///   std__synthesize(examples_array [, ops_array [, max_depth [, cost_hint]]])
/// or its scope-resolved form std::synthesize(...).
///
/// Returns true and populates `examples`, `cfg` if successful.
static bool extractSynthesisArgs(
    const CallExpr*               call,
    std::vector<SynthExample>&    examples,
    SynthConfig&                  cfg) {

    if (!call) return false;
    // Accept both "std__synthesize" (scope-resolved flat name) and
    // "std_synthesize" (legacy flat name).
    const std::string& callee = call->callee;
    if (callee != "std__synthesize" && callee != "std_synthesize")
        return false;
    if (call->arguments.empty()) return false;

    // Helper: extract an integer value from a literal or unary-negated literal.
    auto tryExtractInt = [](const Expression* expr, int64_t& out) -> bool {
        if (const auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
            if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
                out = static_cast<int64_t>(lit->intValue);
                return true;
            }
        }
        // Handle -N parsed as UnaryExpr("-", LiteralExpr(N)).
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
            if (unary->op == "-") {
                if (const auto* lit = dynamic_cast<const LiteralExpr*>(unary->operand.get())) {
                    if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
                        out = -static_cast<int64_t>(lit->intValue);
                        return true;
                    }
                }
            }
        }
        return false;
    };

    // ── Argument 0: examples — must be ARRAY_EXPR of ARRAY_EXPR of literals ──
    const auto* examplesArg = dynamic_cast<const ArrayExpr*>(call->arguments[0].get());
    if (!examplesArg) return false;

    for (const auto& innerExpr : examplesArg->elements) {
        const auto* innerArr = dynamic_cast<const ArrayExpr*>(innerExpr.get());
        if (!innerArr || innerArr->elements.size() < 2) return false;

        SynthExample ex;
        ex.inputs.reserve(innerArr->elements.size() - 1);
        for (size_t i = 0; i < innerArr->elements.size(); ++i) {
            int64_t val = 0;
            if (!tryExtractInt(innerArr->elements[i].get(), val))
                return false;
            if (i + 1 < innerArr->elements.size())
                ex.inputs.push_back(val);
            else
                ex.output = val;
        }
        examples.push_back(std::move(ex));
    }
    if (examples.empty()) return false;

    // Validate: all examples must have the same number of inputs.
    const size_t nInputs = examples[0].inputs.size();
    for (const auto& ex : examples) {
        if (ex.inputs.size() != nInputs) return false;
    }

    // ── Argument 1 (optional): ops — ARRAY_EXPR of string literals ────────
    if (call->arguments.size() >= 2) {
        const auto* opsArg = dynamic_cast<const ArrayExpr*>(call->arguments[1].get());
        if (opsArg) {
            for (const auto& opExpr : opsArg->elements) {
                const auto* lit = dynamic_cast<const LiteralExpr*>(opExpr.get());
                if (lit && lit->literalType == LiteralExpr::LiteralType::STRING)
                    cfg.ops.push_back(lit->stringValue);
            }
        }
    }

    // ── Argument 2 (optional): max_depth — integer literal ────────────────
    if (call->arguments.size() >= 3) {
        int64_t d = 0;
        if (tryExtractInt(call->arguments[2].get(), d))
            cfg.maxDepth = static_cast<int>(std::min(std::max(d, INT64_C(1)), INT64_C(8)));
    }

    // ── Argument 3 (optional): cost_hint — "size" | "speed" ────────────────
    if (call->arguments.size() >= 4) {
        const auto* hintLit = dynamic_cast<const LiteralExpr*>(call->arguments[3].get());
        if (hintLit && hintLit->literalType == LiteralExpr::LiteralType::STRING)
            cfg.preferSize = (hintLit->stringValue == "size");
    }

    return true;
}

/// Check whether a function body is exactly:
///   { return std__synthesize(...); }
/// Returns the inner CallExpr if true, nullptr otherwise.
static const CallExpr* detectSynthesisBody(const FunctionDecl* fn) {
    if (!fn || !fn->body) return nullptr;
    const auto& stmts = fn->body->statements;
    if (stmts.size() != 1) return nullptr;

    const auto* ret = dynamic_cast<const ReturnStmt*>(stmts[0].get());
    if (!ret || !ret->value) return nullptr;

    const auto* call = dynamic_cast<const CallExpr*>(ret->value.get());
    if (!call) return nullptr;

    if (call->callee != "std__synthesize" && call->callee != "std_synthesize")
        return nullptr;
    return call;
}

void runSynthesisPass(Program* program, bool verbose) {
    if (!program) return;

    SynthesisEngine engine;
    int synthesized = 0;
    int failed = 0;

    for (auto& fn : program->functions) {
        const CallExpr* synthCall = detectSynthesisBody(fn.get());
        if (!synthCall) continue;

        std::vector<SynthExample> examples;
        SynthConfig cfg;
        cfg.maxCandidates = 200000;

        if (!extractSynthesisArgs(synthCall, examples, cfg)) {
            if (verbose)
                std::cerr << "  [synthesize] Warning: could not extract examples from "
                          << fn->name << "()\n";
            ++failed;
            continue;
        }

        const int nParams = static_cast<int>(fn->parameters.size());
        // Validate example input widths match parameter count.
        if (!examples.empty() && static_cast<size_t>(nParams) != examples[0].inputs.size()) {
            if (verbose)
                std::cerr << "  [synthesize] Warning: example input count ("
                          << examples[0].inputs.size()
                          << ") doesn't match parameter count ("
                          << nParams << ") in " << fn->name << "()\n";
            ++failed;
            continue;
        }

        if (verbose)
            std::cout << "  [synthesize] Synthesizing body for "
                      << fn->name << "() ("
                      << examples.size() << " examples, "
                      << nParams << " params, depth≤" << cfg.maxDepth << ")...\n";

        auto result = engine.synthesize(nParams, examples, cfg);
        if (!result) {
            if (verbose)
                std::cerr << "  [synthesize] Warning: no expression found for "
                          << fn->name << "() — body left unchanged\n";
            ++failed;
            continue;
        }

        // Collect parameter names for rendering.
        std::vector<std::string> paramNames;
        paramNames.reserve(static_cast<size_t>(nParams));
        for (const auto& p : fn->parameters)
            paramNames.push_back(p.name);

        if (verbose)
            std::cout << "  [synthesize] Found: " << fn->name << "("
                      << [&]() {
                             std::string s;
                             for (size_t i = 0; i < paramNames.size(); ++i) {
                                 if (i) s += ", ";
                                 s += paramNames[i];
                             }
                             return s;
                         }()
                      << ") = "
                      << SynthesisEngine::render(result->expr.get(), paramNames)
                      << "  [cost=" << result->cost
                      << ", nodes=" << result->nodes
                      << ", depth=" << result->depth << "]\n";

        // Lower the synthesized expression to an AST and replace the function body.
        auto synthesizedExpr = SynthesisEngine::lowerToAST(result->expr.get(), paramNames);

        auto retStmt = std::make_unique<ReturnStmt>(std::move(synthesizedExpr));
        retStmt->line   = fn->body->line;
        retStmt->column = fn->body->column;

        std::vector<std::unique_ptr<Statement>> newStmts;
        newStmts.push_back(std::move(retStmt));
        fn->body = std::make_unique<BlockStmt>(std::move(newStmts));
        fn->body->line   = fn->line;
        fn->body->column = fn->column;

        // Mark the synthesized function pure + const_eval so CF-CTRE and the
        // codegen constant-folding machinery can collapse calls to it.
        fn->hintPure      = true;
        fn->hintConstEval = true;

        ++synthesized;
    }

    if (verbose && (synthesized + failed) > 0) {
        std::cout << "  [synthesize] Pass complete: "
                  << synthesized << " synthesized, "
                  << failed << " failed\n";
    }
}

} // namespace omscript
