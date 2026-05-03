/// @file cse_pass.cpp
/// @brief Common Subexpression Elimination (CSE) pass implementation.
///
/// Algorithm overview (per function body block):
///   1. Walk every statement in the block's statement list.
///   2. For each statement, collect all "atomic" pure subexpressions —
///      binary operations whose operands are identifiers or integer literals,
///      PLUS call expressions to ERSL-idempotent functions (canDuplicate=true).
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
///
///   An idempotent call `f(a, b)` is represented as "CALL:f:a:b".
///   Arguments must each be a leaf (identifier or integer literal) for the call
///   to be eligible; this keeps the canonicalisation simple and the key unique.

#include "cse_pass.h"
#include "opt_pass.h"   // isCommutativeOp, isPureBinaryOp
#include "pass_utils.h"

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

/// Return a string representation of a simple (leaf-level) expression,
/// or "" if the expression is not a hoistable atom.
static std::string leafRepr(const Expression* expr) {
    if (!expr) return "";
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR)
        return static_cast<const IdentifierExpr*>(expr)->name;
    long long v = 0;
    if (isIntLiteral(expr, &v))
        return std::to_string(v);
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
    if (isCommutativeOp(op) && l > r) std::swap(l, r);

    return op + ":" + l + ":" + r;
}

/// Return a canonical key for an idempotent call `callee(arg0, arg1, …)`, or ""
/// if any argument is not a leaf (same conservatism as binary expressions).
///
/// Format: "CALL:<callee>:<arg0>:<arg1>:…"
/// Arguments are NOT reordered (function calls are not commutative).
static std::string callKey(const std::string& callee,
                            const std::vector<std::unique_ptr<Expression>>& args) {
    std::string key = "CALL:" + callee;
    for (const auto& arg : args) {
        const std::string leaf = leafRepr(arg.get());
        if (leaf.empty()) return ""; // non-leaf arg → not eligible
        key += ":" + leaf;
    }
    return key;
}
// ─────────────────────────────────────────────────────────────────────────────
// Expression tree traversal — collect candidate subexpression keys
// ─────────────────────────────────────────────────────────────────────────────

static void collectKeys(const Expression* expr,
                        std::unordered_map<std::string, int>& freq,
                        const std::unordered_map<std::string, EffectSummary>* idempotent) {
    if (!expr) return;

    if (expr->type == ASTNodeType::BINARY_EXPR) {
        const auto* bin = static_cast<const BinaryExpr*>(expr);
        // Only consider operators that are pure integer/bitwise ops.
        if (isPureBinaryOp(bin->op)) {
            std::string key = binaryKey(bin->op, bin->left.get(), bin->right.get());
            if (!key.empty()) freq[key]++;
        }
        // Recurse.
        collectKeys(bin->left.get(), freq, idempotent);
        collectKeys(bin->right.get(), freq, idempotent);
    } else if (expr->type == ASTNodeType::CALL_EXPR && idempotent) {
        // ERSL extension: CSE calls to idempotent (canDuplicate) functions.
        const auto* call = static_cast<const CallExpr*>(expr);
        auto it = idempotent->find(call->callee);
        if (it != idempotent->end() && it->second.canDuplicate) {
            std::string key = callKey(call->callee, call->arguments);
            if (!key.empty()) freq[key]++;
        }
        // Recurse into arguments to catch inner binary CSE candidates
        // (e.g. `f(a+b, a+b)` — the `a+b` subexpressions are still pure).
        for (const auto& arg : call->arguments)
            collectKeys(arg.get(), freq, idempotent);
    } else if (expr->type == ASTNodeType::CALL_EXPR) {
        // Non-idempotent call: don't CSE the call itself, but still
        // recurse into arguments for inner binary CSE candidates.
        const auto* call = static_cast<const CallExpr*>(expr);
        for (const auto& arg : call->arguments)
            collectKeys(arg.get(), freq, idempotent);
    } else if (expr->type == ASTNodeType::UNARY_EXPR) {
        collectKeys(static_cast<const UnaryExpr*>(expr)->operand.get(), freq, idempotent);
    } else if (expr->type == ASTNodeType::TERNARY_EXPR) {
        const auto* tern = static_cast<const TernaryExpr*>(expr);
        collectKeys(tern->condition.get(), freq, idempotent);
        collectKeys(tern->thenExpr.get(), freq, idempotent);
        collectKeys(tern->elseExpr.get(), freq, idempotent);
    }
    // Do not descend into non-idempotent CallExpr (may have side effects).
}

static void collectKeysFromStmt(const Statement* stmt,
                                std::unordered_map<std::string, int>& freq,
                                const std::unordered_map<std::string, EffectSummary>* idempotent) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::EXPR_STMT:
        collectKeys(static_cast<const ExprStmt*>(stmt)->expression.get(), freq, idempotent);
        break;
    case ASTNodeType::VAR_DECL:
        collectKeys(static_cast<const VarDecl*>(stmt)->initializer.get(), freq, idempotent);
        break;
    case ASTNodeType::RETURN_STMT:
        collectKeys(static_cast<const ReturnStmt*>(stmt)->value.get(), freq, idempotent);
        break;
    case ASTNodeType::IF_STMT: {
        const auto* ifS = static_cast<const IfStmt*>(stmt);
        collectKeys(ifS->condition.get(), freq, idempotent);
        // Do not recurse into branches — CSE is block-local.
        break;
    }
    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression rewriting — replace a matching expression with a var ref
// ─────────────────────────────────────────────────────────────────────────────

/// If @p expr matches @p key, replace it with an identifier reference to
/// @p varName and return true.  Otherwise recurse and return false.
static bool replaceInExpr(std::unique_ptr<Expression>& expr,
                          const std::string& key,
                          const std::string& varName,
                          const std::unordered_map<std::string, EffectSummary>* idempotent) {
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
        replaceInExpr(bin->left,  key, varName, idempotent);
        replaceInExpr(bin->right, key, varName, idempotent);
    } else if (expr->type == ASTNodeType::CALL_EXPR && idempotent) {
        // ERSL extension: replace idempotent call with the CSE variable.
        auto* call = static_cast<CallExpr*>(expr.get());
        const std::string myKey = callKey(call->callee, call->arguments);
        if (!myKey.empty() && myKey == key) {
            expr = std::make_unique<IdentifierExpr>(varName);
            return true;
        }
    } else if (expr->type == ASTNodeType::UNARY_EXPR) {
        replaceInExpr(static_cast<UnaryExpr*>(expr.get())->operand, key, varName, idempotent);
    } else if (expr->type == ASTNodeType::TERNARY_EXPR) {
        auto* tern = static_cast<TernaryExpr*>(expr.get());
        replaceInExpr(tern->condition, key, varName, idempotent);
        replaceInExpr(tern->thenExpr,  key, varName, idempotent);
        replaceInExpr(tern->elseExpr,  key, varName, idempotent);
    }
    return false;
}

static void replaceInStmt(Statement* stmt,
                          const std::string& key,
                          const std::string& varName,
                          const std::unordered_map<std::string, EffectSummary>* idempotent) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::EXPR_STMT:
        replaceInExpr(static_cast<ExprStmt*>(stmt)->expression, key, varName, idempotent);
        break;
    case ASTNodeType::VAR_DECL:
        replaceInExpr(static_cast<VarDecl*>(stmt)->initializer, key, varName, idempotent);
        break;
    case ASTNodeType::RETURN_STMT:
        replaceInExpr(static_cast<ReturnStmt*>(stmt)->value, key, varName, idempotent);
        break;
    case ASTNodeType::IF_STMT:
        replaceInExpr(static_cast<IfStmt*>(stmt)->condition, key, varName, idempotent);
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
            return makeIntLiteral(v);
    } catch (...) {}
    return std::make_unique<IdentifierExpr>(token);
}

/// Reconstruct an Expression from a key string.
///
/// Binary key format : "OP:lhs:rhs"
/// Call key format   : "CALL:name:arg0:arg1:…"
///
/// Returns nullptr if the key cannot be parsed.
static std::unique_ptr<Expression> exprFromKey(const std::string& key) {
    const size_t first = key.find(':');
    if (first == std::string::npos) return nullptr;

    // Check whether this is a call key.
    const std::string prefix = key.substr(0, first);
    if (prefix == "CALL") {
        // "CALL:name:arg0:arg1:..."
        const size_t nameEnd = key.find(':', first + 1);
        std::string callee;
        std::vector<std::unique_ptr<Expression>> args;
        if (nameEnd == std::string::npos) {
            // No arguments: "CALL:name"
            callee = key.substr(first + 1);
        } else {
            callee = key.substr(first + 1, nameEnd - first - 1);
            // Parse remaining `:arg0:arg1:…`
            size_t pos = nameEnd + 1;
            while (pos < key.size()) {
                size_t next = key.find(':', pos);
                const std::string tok = (next == std::string::npos)
                                            ? key.substr(pos)
                                            : key.substr(pos, next - pos);
                args.push_back(makeLeaf(tok));
                if (next == std::string::npos) break;
                pos = next + 1;
            }
        }
        return std::make_unique<CallExpr>(callee, std::move(args));
    }

    // Binary key: "OP:lhs:rhs"
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
static CSEStats processBlock(BlockStmt* block, unsigned& nextId,
                             const std::unordered_map<std::string, EffectSummary>* idempotent);

static CSEStats processBlock(BlockStmt* block, unsigned& nextId,
                             const std::unordered_map<std::string, EffectSummary>* idempotent) {
    CSEStats stats;
    if (!block || block->statements.empty()) return stats;

    // ── Step 1: collect frequency of CSE-eligible subexpressions ───────────
    std::unordered_map<std::string, int> freq;
    for (const auto& stmt : block->statements)
        collectKeysFromStmt(stmt.get(), freq, idempotent);

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
                collectKeysFromStmt(block->statements[i].get(), probe, idempotent);
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
            collectKeysFromStmt(block->statements[i].get(), probe, idempotent);
            if (probe.count(key)) {
                replaceInStmt(block->statements[i].get(), key, varName, idempotent);
                ++replacements;
            }
        }
        stats.expressionsHoisted += replacements;
    }

    // ── Step 3: recurse into nested blocks ──────────────────────────────────
    for (auto& stmt : block->statements) {
        if (!stmt) continue;
        if (stmt->type == ASTNodeType::BLOCK) {
            auto sub = processBlock(static_cast<BlockStmt*>(stmt.get()), nextId, idempotent);
            stats.expressionsHoisted += sub.expressionsHoisted;
            stats.tempVarsIntroduced += sub.tempVarsIntroduced;
        } else if (stmt->type == ASTNodeType::IF_STMT) {
            auto* ifS = static_cast<IfStmt*>(stmt.get());
            if (ifS->thenBranch && ifS->thenBranch->type == ASTNodeType::BLOCK) {
                auto sub = processBlock(static_cast<BlockStmt*>(ifS->thenBranch.get()), nextId, idempotent);
                stats.expressionsHoisted += sub.expressionsHoisted;
                stats.tempVarsIntroduced += sub.tempVarsIntroduced;
            }
            if (ifS->elseBranch && ifS->elseBranch->type == ASTNodeType::BLOCK) {
                auto sub = processBlock(static_cast<BlockStmt*>(ifS->elseBranch.get()), nextId, idempotent);
                stats.expressionsHoisted += sub.expressionsHoisted;
                stats.tempVarsIntroduced += sub.tempVarsIntroduced;
            }
        } else if (stmt->type == ASTNodeType::WHILE_STMT) {
            auto* ws = static_cast<WhileStmt*>(stmt.get());
            if (ws->body && ws->body->type == ASTNodeType::BLOCK) {
                auto sub = processBlock(static_cast<BlockStmt*>(ws->body.get()), nextId, idempotent);
                stats.expressionsHoisted += sub.expressionsHoisted;
                stats.tempVarsIntroduced += sub.tempVarsIntroduced;
            }
        } else if (stmt->type == ASTNodeType::FOR_STMT) {
            auto* fs = static_cast<ForStmt*>(stmt.get());
            if (fs->body && fs->body->type == ASTNodeType::BLOCK) {
                auto sub = processBlock(static_cast<BlockStmt*>(fs->body.get()), nextId, idempotent);
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

CSEStats runCSEPass(Program* program,
                    bool verbose,
                    const std::unordered_map<std::string, EffectSummary>* idempotentFuncs) {
    CSEStats total;

    // Each function gets its own counter to keep temp-var names short.
    forEachFunction(program, [&](FunctionDecl* fn) {
        unsigned nextId = 0;
        CSEStats fnStats = processBlock(fn->body.get(), nextId, idempotentFuncs);

        total.expressionsHoisted += fnStats.expressionsHoisted;
        total.tempVarsIntroduced += fnStats.tempVarsIntroduced;

        if (verbose && fnStats.tempVarsIntroduced > 0) {
            std::cerr << "[CSE] " << fn->name
                      << ": " << fnStats.tempVarsIntroduced
                      << " temp var(s) introduced, "
                      << fnStats.expressionsHoisted
                      << " occurrence(s) replaced\n";
        }
    });

    return total;
}

} // namespace omscript
