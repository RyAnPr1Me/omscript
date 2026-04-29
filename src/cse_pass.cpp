/// @file cse_pass.cpp
/// @brief Common Subexpression Elimination (CSE) pass implementation.
///
/// Algorithm overview (per function body block):
///   1. Walk every statement in the block's statement list.
///   2. For each statement, collect all "atomic" pure subexpressions —
///      binary operations whose operands are identifiers or integer literals.
///   3. Maintain a frequency map: canonical_repr → count.
///   4. For any expression that appears 2+ times in the same block:
///      a. Generate a new compiler-managed variable name (_cse_0, _cse_1, ...).
///      b. Insert `var _cse_N = <expr>` immediately before the first use.
///      c. Replace ALL subsequent occurrences of the expression (in the same
///         block's statement sequence) with `_cse_N`.
///   5. Recurse into nested blocks.
///
/// Canonicalisation:
///   A binary expression `a OP b` is represented as the string "OP:a:b".
///   Commutative operators (+, *, &, |, ^, ==, !=) are normalised so that
///   the lexicographically smaller operand comes first, making `a+b` and `b+a`
///   the same CSE key.

#include "cse_pass.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Expression canonicalisation helpers
// ─────────────────────────────────────────────────────────────────────────────

static const std::unordered_set<std::string> kCommutative = {
    "+", "*", "&", "|", "^", "==", "!="
};

/// Return a string representation of a simple (leaf-level) expression,
/// or "" if the expression is not a hoistable atom.
static std::string leafRepr(const Expression* expr) {
    if (!expr) return "";
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR)
        return static_cast<const IdentifierExpr*>(expr)->name;
    if (expr->type == ASTNodeType::LITERAL_EXPR) {
        const auto* lit = static_cast<const LiteralExpr*>(expr);
        if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
            return std::to_string(lit->intValue);
    }
    return "";
}

/// Return a canonical key for a binary expression `left OP right`, or ""
/// if either operand is not a leaf (we only CSE one level deep at each pass;
/// deeper nesting is handled by running the pass multiple times or by the
/// e-graph).
static std::string binaryKey(const std::string& op,
                              const Expression* left,
                              const Expression* right) {
    std::string l = leafRepr(left);
    std::string r = leafRepr(right);
    if (l.empty() || r.empty()) return "";

    // Normalise commutative operators so a+b and b+a share the same key.
    if (kCommutative.count(op) && l > r) std::swap(l, r);

    return op + ":" + l + ":" + r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression tree traversal — collect candidate subexpression keys
// ─────────────────────────────────────────────────────────────────────────────

static void collectKeys(const Expression* expr,
                        std::unordered_map<std::string, int>& freq) {
    if (!expr) return;

    if (expr->type == ASTNodeType::BINARY_EXPR) {
        const auto* bin = static_cast<const BinaryExpr*>(expr);
        // Only consider operators that are pure integer/bitwise ops.
        static const std::unordered_set<std::string> kPureOps = {
            "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>",
            "==", "!=", "<", "<=", ">", ">="
        };
        if (kPureOps.count(bin->op)) {
            std::string key = binaryKey(bin->op, bin->left.get(), bin->right.get());
            if (!key.empty()) freq[key]++;
        }
        // Recurse.
        collectKeys(bin->left.get(), freq);
        collectKeys(bin->right.get(), freq);
    } else if (expr->type == ASTNodeType::UNARY_EXPR) {
        collectKeys(static_cast<const UnaryExpr*>(expr)->operand.get(), freq);
    } else if (expr->type == ASTNodeType::TERNARY_EXPR) {
        const auto* tern = static_cast<const TernaryExpr*>(expr);
        collectKeys(tern->condition.get(), freq);
        collectKeys(tern->thenExpr.get(), freq);
        collectKeys(tern->elseExpr.get(), freq);
    }
    // Do not descend into CallExpr (may have side effects).
}

static void collectKeysFromStmt(const Statement* stmt,
                                std::unordered_map<std::string, int>& freq) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::EXPR_STMT:
        collectKeys(static_cast<const ExprStmt*>(stmt)->expression.get(), freq);
        break;
    case ASTNodeType::VAR_DECL:
        collectKeys(static_cast<const VarDecl*>(stmt)->initializer.get(), freq);
        break;
    case ASTNodeType::RETURN_STMT:
        collectKeys(static_cast<const ReturnStmt*>(stmt)->value.get(), freq);
        break;
    case ASTNodeType::IF_STMT: {
        const auto* ifS = static_cast<const IfStmt*>(stmt);
        collectKeys(ifS->condition.get(), freq);
        // Do not recurse into branches — CSE is block-local.
        break;
    }
    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression rewriting — replace a matching binary expression with a var ref
// ─────────────────────────────────────────────────────────────────────────────

/// If @p expr matches @p key, replace it with an identifier reference to
/// @p varName and return true.  Otherwise recurse and return false.
static bool replaceInExpr(std::unique_ptr<Expression>& expr,
                          const std::string& key,
                          const std::string& varName) {
    if (!expr) return false;

    if (expr->type == ASTNodeType::BINARY_EXPR) {
        auto* bin = static_cast<BinaryExpr*>(expr.get());
        const std::string myKey = binaryKey(bin->op, bin->left.get(), bin->right.get());
        if (myKey == key) {
            expr = std::make_unique<IdentifierExpr>(varName);
            return true;
        }
        // Recurse into children (stop at the first replacement to avoid
        // double-counting — the next pass iteration handles deeper matches).
        replaceInExpr(bin->left, key, varName);
        replaceInExpr(bin->right, key, varName);
    } else if (expr->type == ASTNodeType::UNARY_EXPR) {
        replaceInExpr(static_cast<UnaryExpr*>(expr.get())->operand, key, varName);
    } else if (expr->type == ASTNodeType::TERNARY_EXPR) {
        auto* tern = static_cast<TernaryExpr*>(expr.get());
        replaceInExpr(tern->condition, key, varName);
        replaceInExpr(tern->thenExpr, key, varName);
        replaceInExpr(tern->elseExpr, key, varName);
    }
    return false;
}

static void replaceInStmt(Statement* stmt,
                          const std::string& key,
                          const std::string& varName) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::EXPR_STMT:
        replaceInExpr(static_cast<ExprStmt*>(stmt)->expression, key, varName);
        break;
    case ASTNodeType::VAR_DECL:
        replaceInExpr(static_cast<VarDecl*>(stmt)->initializer, key, varName);
        break;
    case ASTNodeType::RETURN_STMT:
        replaceInExpr(static_cast<ReturnStmt*>(stmt)->value, key, varName);
        break;
    case ASTNodeType::IF_STMT:
        replaceInExpr(static_cast<IfStmt*>(stmt)->condition, key, varName);
        break;
    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Build an Expression node from a canonical key
// ─────────────────────────────────────────────────────────────────────────────

/// Parse a leaf token back into an Expression (identifier or integer literal).
static std::unique_ptr<Expression> makeLeaf(const std::string& token) {
    // Try to parse as integer first.
    try {
        size_t pos = 0;
        long long v = std::stoll(token, &pos);
        if (pos == token.size())
            return std::make_unique<LiteralExpr>(v);
    } catch (...) {}
    return std::make_unique<IdentifierExpr>(token);
}

/// Reconstruct an Expression from a binary key "OP:lhs:rhs".
/// Returns nullptr if the key cannot be parsed.
static std::unique_ptr<Expression> exprFromKey(const std::string& key) {
    const size_t first = key.find(':');
    if (first == std::string::npos) return nullptr;
    const size_t second = key.find(':', first + 1);
    if (second == std::string::npos) return nullptr;

    std::string op  = key.substr(0, first);
    std::string lhs = key.substr(first + 1, second - first - 1);
    std::string rhs = key.substr(second + 1);

    return std::make_unique<BinaryExpr>(op, makeLeaf(lhs), makeLeaf(rhs));
}

// ─────────────────────────────────────────────────────────────────────────────
// Block-level CSE
// ─────────────────────────────────────────────────────────────────────────────

// Forward declaration for mutual recursion.
static CSEStats processBlock(BlockStmt* block, unsigned& nextId);

static CSEStats processBlock(BlockStmt* block, unsigned& nextId) {
    CSEStats stats;
    if (!block || block->statements.empty()) return stats;

    // ── Step 1: collect frequency of atomic binary subexpressions ──────────
    std::unordered_map<std::string, int> freq;
    for (const auto& stmt : block->statements)
        collectKeysFromStmt(stmt.get(), freq);

    // ── Step 2: for each key with freq >= 2, hoist it ──────────────────────
    // Process candidates in deterministic order (sort by key).
    std::vector<std::string> candidates;
    for (const auto& [k, cnt] : freq)
        if (cnt >= 2) candidates.push_back(k);
    std::sort(candidates.begin(), candidates.end());

    for (const std::string& key : candidates) {
        auto initExpr = exprFromKey(key);
        if (!initExpr) continue;

        const std::string varName = "_cse_" + std::to_string(nextId++);

        // Find the first statement that contains this expression.
        size_t insertPos = block->statements.size(); // sentinel
        {
            std::unordered_map<std::string, int> probe;
            for (size_t i = 0; i < block->statements.size(); ++i) {
                probe.clear();
                collectKeysFromStmt(block->statements[i].get(), probe);
                if (probe.count(key)) { insertPos = i; break; }
            }
        }
        if (insertPos == block->statements.size()) continue;

        // Insert `var _cse_N = <expr>` before insertPos.
        auto decl = std::make_unique<VarDecl>(varName, std::move(initExpr),
                                              /*isConst=*/true,
                                              /*type=*/"");
        decl->isCompilerGenerated = true;
        block->statements.insert(block->statements.begin() +
                                     static_cast<ptrdiff_t>(insertPos),
                                 std::move(decl));
        ++stats.tempVarsIntroduced;

        // Replace all occurrences of key in statements AFTER the declaration
        // (i.e., from insertPos+1 onward — the decl itself must stay intact).
        unsigned replacements = 0;
        for (size_t i = insertPos + 1; i < block->statements.size(); ++i) {
            std::unordered_map<std::string, int> probe;
            collectKeysFromStmt(block->statements[i].get(), probe);
            if (probe.count(key)) {
                replaceInStmt(block->statements[i].get(), key, varName);
                ++replacements;
            }
        }
        stats.expressionsHoisted += replacements;
    }

    // ── Step 3: recurse into nested blocks ──────────────────────────────────
    for (auto& stmt : block->statements) {
        if (!stmt) continue;
        if (stmt->type == ASTNodeType::BLOCK) {
            auto sub = processBlock(static_cast<BlockStmt*>(stmt.get()), nextId);
            stats.expressionsHoisted += sub.expressionsHoisted;
            stats.tempVarsIntroduced += sub.tempVarsIntroduced;
        } else if (stmt->type == ASTNodeType::IF_STMT) {
            auto* ifS = static_cast<IfStmt*>(stmt.get());
            if (ifS->thenBranch && ifS->thenBranch->type == ASTNodeType::BLOCK) {
                auto sub = processBlock(static_cast<BlockStmt*>(ifS->thenBranch.get()), nextId);
                stats.expressionsHoisted += sub.expressionsHoisted;
                stats.tempVarsIntroduced += sub.tempVarsIntroduced;
            }
            if (ifS->elseBranch && ifS->elseBranch->type == ASTNodeType::BLOCK) {
                auto sub = processBlock(static_cast<BlockStmt*>(ifS->elseBranch.get()), nextId);
                stats.expressionsHoisted += sub.expressionsHoisted;
                stats.tempVarsIntroduced += sub.tempVarsIntroduced;
            }
        } else if (stmt->type == ASTNodeType::WHILE_STMT) {
            auto* ws = static_cast<WhileStmt*>(stmt.get());
            if (ws->body && ws->body->type == ASTNodeType::BLOCK) {
                auto sub = processBlock(static_cast<BlockStmt*>(ws->body.get()), nextId);
                stats.expressionsHoisted += sub.expressionsHoisted;
                stats.tempVarsIntroduced += sub.tempVarsIntroduced;
            }
        } else if (stmt->type == ASTNodeType::FOR_STMT) {
            auto* fs = static_cast<ForStmt*>(stmt.get());
            if (fs->body && fs->body->type == ASTNodeType::BLOCK) {
                auto sub = processBlock(static_cast<BlockStmt*>(fs->body.get()), nextId);
                stats.expressionsHoisted += sub.expressionsHoisted;
                stats.tempVarsIntroduced += sub.tempVarsIntroduced;
            }
        }
    }

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

CSEStats runCSEPass(Program* program, bool verbose) {
    CSEStats total;
    if (!program) return total;

    // Each function gets its own counter to keep temp-var names short.
    for (auto& node : program->functions) {
        auto* fn = static_cast<FunctionDecl*>(node.get());
        if (!fn || !fn->body) continue;

        unsigned nextId = 0;
        CSEStats fnStats = processBlock(fn->body.get(), nextId);

        total.expressionsHoisted += fnStats.expressionsHoisted;
        total.tempVarsIntroduced += fnStats.tempVarsIntroduced;

        if (verbose && fnStats.tempVarsIntroduced > 0) {
            std::cerr << "[CSE] " << fn->name
                      << ": " << fnStats.tempVarsIntroduced
                      << " temp var(s) introduced, "
                      << fnStats.expressionsHoisted
                      << " occurrence(s) replaced\n";
        }
    }

    return total;
}

} // namespace omscript
