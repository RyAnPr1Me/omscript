/// @file var_range_analysis.cpp
/// @brief Intra-function forward-dataflow integer range analysis.
///
/// See var_range_analysis.h for the full algorithm description.

#include "var_range_analysis.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <unordered_set>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// evalExprRange — compute the range of a single expression
// ─────────────────────────────────────────────────────────────────────────────

std::optional<ValueRange> evalExprRange(const Expression* expr,
                                         const VarRangeMap& env) {
    if (!expr) return {};

    // ── Integer literal → exact point range ──────────────────────────────────
    long long iv = 0;
    if (isIntLiteral(expr, &iv))
        return ValueRange{iv, iv};

    // ── @range[lo, hi] annotation → use the annotation directly ──────────────
    if (expr->type == ASTNodeType::RANGE_ANNOT_EXPR) {
        const auto* ra = static_cast<const RangeAnnotExpr*>(expr);
        return ValueRange{ra->lo, ra->hi};
    }

    // ── Identifier → look up in environment ──────────────────────────────────
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
        const auto* id = static_cast<const IdentifierExpr*>(expr);
        const auto it = env.find(id->name);
        if (it != env.end()) return it->second;
        return {};
    }

    // ── Unary negation: -x → [-hi(x), -lo(x)] ───────────────────────────────
    if (expr->type == ASTNodeType::UNARY_EXPR) {
        const auto* ue = static_cast<const UnaryExpr*>(expr);
        if (ue->op == "-") {
            auto r = evalExprRange(ue->operand.get(), env);
            if (r && r->lo != INT64_MIN && r->hi != INT64_MIN)
                return ValueRange{-r->hi, -r->lo};
        }
        return {};
    }

    // ── Binary operations ─────────────────────────────────────────────────────
    if (expr->type == ASTNodeType::BINARY_EXPR) {
        const auto* bin = static_cast<const BinaryExpr*>(expr);
        const std::string& op = bin->op;

        // x + y  (non-negative inputs only — avoids signed-overflow in the
        //         range arithmetic itself)
        if (op == "+") {
            auto lr = evalExprRange(bin->left.get(),  env);
            auto rr = evalExprRange(bin->right.get(), env);
            if (lr && rr && lr->lo >= 0 && rr->lo >= 0) {
                const int64_t lo = lr->lo + rr->lo;
                const int64_t hi = (rr->hi <= INT64_MAX - lr->hi)
                                       ? lr->hi + rr->hi
                                       : INT64_MAX;
                return ValueRange{lo, hi};
            }
        }

        // x - y  → [lo(x)-hi(y), hi(x)-lo(y)]  (with overflow guards)
        if (op == "-") {
            auto lr = evalExprRange(bin->left.get(),  env);
            auto rr = evalExprRange(bin->right.get(), env);
            if (lr && rr && rr->hi != INT64_MIN) {
                const int64_t lo = (lr->lo > INT64_MIN + rr->hi)
                                       ? lr->lo - rr->hi
                                       : INT64_MIN;
                const int64_t hi = (rr->lo > 0 && lr->hi < INT64_MAX + rr->lo)
                                       ? lr->hi - rr->lo
                                       : (rr->lo <= 0 ? INT64_MAX : lr->hi - rr->lo);
                if (lo <= hi) return ValueRange{lo, hi};
            }
        }

        // x * y  (both non-negative, overflow-guarded) → [lo*lo, hi*hi]
        if (op == "*") {
            auto lr = evalExprRange(bin->left.get(),  env);
            auto rr = evalExprRange(bin->right.get(), env);
            if (lr && rr && lr->lo >= 0 && rr->lo >= 0 &&
                lr->hi > 0 && rr->hi > 0 &&
                lr->hi <= INT64_MAX / rr->hi) {
                return ValueRange{lr->lo * rr->lo, lr->hi * rr->hi};
            }
        }

        // x & mask  (mask is a non-negative literal → result in [0, mask])
        if (op == "&") {
            long long mask = 0;
            if (isIntLiteral(bin->right.get(), &mask) && mask >= 0)
                return ValueRange{0, mask};
            if (isIntLiteral(bin->left.get(),  &mask) && mask >= 0)
                return ValueRange{0, mask};
        }

        // x % M  (M positive literal, x ≥ 0 → result in [0, M-1])
        if (op == "%") {
            long long m = 0;
            if (isIntLiteral(bin->right.get(), &m) && m > 0) {
                auto lr = evalExprRange(bin->left.get(), env);
                if (lr && lr->lo >= 0)
                    return ValueRange{0, m - 1};
            }
        }

        // x >> N  (logical shift right, OmScript uses LShr → always non-neg)
        if (op == ">>") {
            long long shift = 0;
            if (isIntLiteral(bin->right.get(), &shift) && shift > 0 && shift < 64) {
                auto lr = evalExprRange(bin->left.get(), env);
                const int64_t resultHi = lr ? (lr->hi >> shift) : (INT64_MAX >> shift);
                return ValueRange{0, resultHi};
            }
        }

        // x << N  (left shift by a literal; only safe bound: non-negative if x ≥ 0
        //          and shift is small — upper bound is hard without overflow proof)
        // We only return a non-negativity bound when x is known non-negative.
        if (op == "<<") {
            long long shift = 0;
            if (isIntLiteral(bin->right.get(), &shift) && shift >= 0 && shift < 63) {
                auto lr = evalExprRange(bin->left.get(), env);
                if (lr && lr->lo >= 0 && lr->hi <= (INT64_MAX >> shift))
                    return ValueRange{lr->lo << shift, lr->hi << shift};
            }
        }

        // comparison operators → boolean result in {0, 1}
        if (op == "==" || op == "!=" || op == "<" || op == "<=" ||
            op == ">"  || op == ">=" || op == "&&" || op == "||") {
            return ValueRange{0, 1};
        }

        return {};
    }

    // ── Call expressions ──────────────────────────────────────────────────────
    if (expr->type == ASTNodeType::CALL_EXPR) {
        const auto* call = static_cast<const CallExpr*>(expr);

        // len(x) → [0, INT64_MAX]
        if (call->callee == "len")
            return ValueRange{0, INT64_MAX};

        // abs(x) → [0, INT64_MAX]
        if (call->callee == "abs")
            return ValueRange{0, INT64_MAX};

        // clamp(x, lo, hi) with literal bounds
        if (call->callee == "clamp" && call->arguments.size() == 3) {
            long long lo = 0, hi = 0;
            if (isIntLiteral(call->arguments[1].get(), &lo) &&
                isIntLiteral(call->arguments[2].get(), &hi) && lo <= hi)
                return ValueRange{lo, hi};
        }

        // min(x, C) or min(C, x) → upper bound C
        if (call->callee == "min" && call->arguments.size() == 2) {
            long long c = 0;
            if (isIntLiteral(call->arguments[0].get(), &c))
                return ValueRange{INT64_MIN, c};
            if (isIntLiteral(call->arguments[1].get(), &c))
                return ValueRange{INT64_MIN, c};
        }

        // max(x, C) or max(C, x) → lower bound C
        if (call->callee == "max" && call->arguments.size() == 2) {
            long long c = 0;
            if (isIntLiteral(call->arguments[0].get(), &c))
                return ValueRange{c, INT64_MAX};
            if (isIntLiteral(call->arguments[1].get(), &c))
                return ValueRange{c, INT64_MAX};
        }

        // is_even / is_odd → {0,1}
        if (call->callee == "is_even" || call->callee == "is_odd")
            return ValueRange{0, 1};

        return {};
    }

    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Join two VarRangeMaps: for each variable present in both, join the ranges;
/// for variables only in one map, use the full range (conservative).
static VarRangeMap joinMaps(const VarRangeMap& a, const VarRangeMap& b) {
    VarRangeMap result;
    for (const auto& [name, ra] : a) {
        const auto it = b.find(name);
        if (it != b.end()) {
            const ValueRange joined = ValueRange::join(ra, it->second);
            if (joined.isNarrowed())
                result[name] = joined;
            // else: range widened to full → drop from map (treated as unknown)
        }
        // Variable only in `a` → unknown after branch join (could take else path)
    }
    // Variables only in `b` are also unknown after branch join.
    return result;
}

/// Narrow a single variable's range in @p env based on a binary comparison
/// condition being *taken* (if @p taken == true) or *not taken* (else branch).
///
/// Handles: `id OP literal` and `literal OP id` patterns.
static void narrowFromBinary(const BinaryExpr* bin, VarRangeMap& env, bool taken) {
    // We need: identifier on one side, integer literal on the other.
    const IdentifierExpr* id  = nullptr;
    long long              lit = 0;
    std::string            op  = bin->op;

    if (asIdentifier(bin->left.get()) && isIntLiteral(bin->right.get(), &lit)) {
        id = static_cast<const IdentifierExpr*>(bin->left.get());
    } else if (asIdentifier(bin->right.get()) && isIntLiteral(bin->left.get(), &lit)) {
        id = static_cast<const IdentifierExpr*>(bin->right.get());
        // Flip the operator: `lit OP id` becomes `id FLIP(OP) lit`.
        if      (op == "<")  op = ">";
        else if (op == "<=") op = ">=";
        else if (op == ">")  op = "<";
        else if (op == ">=") op = "<=";
    }
    if (!id) return;

    // Get or default to full range.
    ValueRange cur{INT64_MIN, INT64_MAX};
    const auto it = env.find(id->name);
    if (it != env.end()) cur = it->second;

    ValueRange narrowed = cur;
    if (taken) {
        // Condition is true in this branch.
        if      (op == "<")  narrowed.hi = std::min(cur.hi, static_cast<int64_t>(lit) - 1);
        else if (op == "<=") narrowed.hi = std::min(cur.hi, static_cast<int64_t>(lit));
        else if (op == ">")  narrowed.lo = std::max(cur.lo, static_cast<int64_t>(lit) + 1);
        else if (op == ">=") narrowed.lo = std::max(cur.lo, static_cast<int64_t>(lit));
        else if (op == "==") narrowed     = {static_cast<int64_t>(lit), static_cast<int64_t>(lit)};
        // !=: can't usefully narrow a single exclusion
    } else {
        // Condition is false in this branch (we are in the else arm).
        if      (op == "<")  narrowed.lo = std::max(cur.lo, static_cast<int64_t>(lit));
        else if (op == "<=") narrowed.lo = std::max(cur.lo, static_cast<int64_t>(lit) + 1);
        else if (op == ">")  narrowed.hi = std::min(cur.hi, static_cast<int64_t>(lit));
        else if (op == ">=") narrowed.hi = std::min(cur.hi, static_cast<int64_t>(lit) - 1);
        else if (op == "!=") { /* can't narrow usefully */ }
        else if (op == "==") { /* we know id != lit; can't represent easily */ }
    }

    if (narrowed.isEmpty()) return; // Sanity guard: don't insert empty range.
    if (narrowed.lo > cur.lo || narrowed.hi < cur.hi)
        env[id->name] = narrowed;
}

/// Narrow @p env using the condition @p cond being taken or not-taken.
/// Only handles simple binary comparisons for now.
static void narrowFromCondition(const Expression* cond, VarRangeMap& env, bool taken) {
    if (!cond) return;
    if (cond->type == ASTNodeType::BINARY_EXPR) {
        const auto* bin = static_cast<const BinaryExpr*>(cond);
        narrowFromBinary(bin, env, taken);
        return;
    }
    // `!(cond)` → flip taken
    if (cond->type == ASTNodeType::UNARY_EXPR) {
        const auto* ue = static_cast<const UnaryExpr*>(cond);
        if (ue->op == "!")
            narrowFromCondition(ue->operand.get(), env, !taken);
    }
}

/// Collect the names of all variables that are *written* anywhere inside
/// @p stmt (including nested blocks / loop bodies).  Used to conservatively
/// invalidate loop-body variables in the range map.
static void collectWritten(const Statement* stmt,
                            std::unordered_set<std::string>& out) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::VAR_DECL:
        out.insert(static_cast<const VarDecl*>(stmt)->name);
        break;
    case ASTNodeType::EXPR_STMT: {
        std::function<void(const Expression*)> scan = [&](const Expression* e) {
            if (!e) return;
            if (e->type == ASTNodeType::ASSIGN_EXPR)
                out.insert(static_cast<const AssignExpr*>(e)->name);
            if (e->type == ASTNodeType::POSTFIX_EXPR) {
                const auto* pe = static_cast<const PostfixExpr*>(e);
                if (const auto* id = asIdentifier(pe->operand.get()))
                    out.insert(id->name);
            }
            if (e->type == ASTNodeType::PREFIX_EXPR) {
                const auto* pe = static_cast<const PrefixExpr*>(e);
                if (const auto* id = asIdentifier(pe->operand.get()))
                    out.insert(id->name);
            }
            if (e->type == ASTNodeType::BINARY_EXPR) {
                const auto* b = static_cast<const BinaryExpr*>(e);
                scan(b->left.get()); scan(b->right.get());
            }
            if (e->type == ASTNodeType::CALL_EXPR) {
                const auto* c = static_cast<const CallExpr*>(e);
                for (const auto& arg : c->arguments) scan(arg.get());
            }
        };
        scan(static_cast<const ExprStmt*>(stmt)->expression.get());
        break;
    }
    case ASTNodeType::FOR_STMT: {
        const auto* fs = static_cast<const ForStmt*>(stmt);
        out.insert(fs->iteratorVar);
        collectWritten(fs->body.get(), out);
        break;
    }
    case ASTNodeType::FOR_EACH_STMT: {
        const auto* fe = static_cast<const ForEachStmt*>(stmt);
        out.insert(fe->iteratorVar);
        collectWritten(fe->body.get(), out);
        break;
    }
    case ASTNodeType::WHILE_STMT:
        collectWritten(static_cast<const WhileStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::DO_WHILE_STMT:
        collectWritten(static_cast<const DoWhileStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::IF_STMT: {
        const auto* is = static_cast<const IfStmt*>(stmt);
        collectWritten(is->thenBranch.get(), out);
        collectWritten(is->elseBranch.get(), out);
        break;
    }
    case ASTNodeType::BLOCK: {
        const auto* blk = static_cast<const BlockStmt*>(stmt);
        for (const auto& s : blk->statements)
            collectWritten(s.get(), out);
        break;
    }
    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations for mutual recursion
// ─────────────────────────────────────────────────────────────────────────────

static void scanBlock(const BlockStmt* block, VarRangeMap& env);
static void scanStmt (const Statement*  stmt,  VarRangeMap& env);

// ─────────────────────────────────────────────────────────────────────────────
// scanStmt — update @p env with the ranges implied by @p stmt
// ─────────────────────────────────────────────────────────────────────────────

static void scanStmt(const Statement* stmt, VarRangeMap& env) {
    if (!stmt) return;
    switch (stmt->type) {

    // ── Variable declaration ─────────────────────────────────────────────────
    case ASTNodeType::VAR_DECL: {
        const auto* vd = static_cast<const VarDecl*>(stmt);
        auto r = evalExprRange(vd->initializer.get(), env);
        if (r && r->isNarrowed())
            env[vd->name] = *r;
        else
            env.erase(vd->name); // unknown range: remove stale entry
        break;
    }

    // ── If statement — branch-sensitive narrowing then join ──────────────────
    case ASTNodeType::IF_STMT: {
        const auto* ifs = static_cast<const IfStmt*>(stmt);

        // Then-branch: narrow from condition being true.
        VarRangeMap thenEnv = env;
        narrowFromCondition(ifs->condition.get(), thenEnv, /*taken=*/true);
        if (ifs->thenBranch) scanStmt(ifs->thenBranch.get(), thenEnv);

        // Else-branch: narrow from condition being false.
        VarRangeMap elseEnv = env;
        narrowFromCondition(ifs->condition.get(), elseEnv, /*taken=*/false);
        if (ifs->elseBranch) scanStmt(ifs->elseBranch.get(), elseEnv);

        // Join results back into env.
        if (ifs->thenBranch && ifs->elseBranch) {
            env = joinMaps(thenEnv, elseEnv);
        } else if (ifs->thenBranch) {
            // No else: after the if, env is either the pre-if state (else path)
            // or the thenEnv (then path) → join original env with thenEnv.
            env = joinMaps(env, thenEnv);
        } else if (ifs->elseBranch) {
            env = joinMaps(env, elseEnv);
        }
        break;
    }

    // ── For-range loop ────────────────────────────────────────────────────────
    case ASTNodeType::FOR_STMT: {
        const auto* fs = static_cast<const ForStmt*>(stmt);

        // Determine iterator range from start/end expressions.
        auto sr = evalExprRange(fs->start.get(), env);
        auto er = evalExprRange(fs->end.get(),   env);

        if (sr && er) {
            // Induction variable: [min(start,end), max(start,end) - 1]
            // (OmScript for-loops are exclusive of end, like Python range)
            const int64_t lo = std::min(sr->lo, er->lo);
            const int64_t hi = std::max(sr->hi, er->hi) - 1;
            if (lo <= hi)
                env[fs->iteratorVar] = ValueRange{lo, hi};
            else
                env.erase(fs->iteratorVar);
        } else if (sr && sr->lo >= 0) {
            // At least know the iterator is non-negative.
            env[fs->iteratorVar] = ValueRange{0, INT64_MAX};
        } else {
            env.erase(fs->iteratorVar);
        }

        // Invalidate all variables written in the loop body (conservative).
        std::unordered_set<std::string> bodyWrites;
        collectWritten(fs->body.get(), bodyWrites);
        for (const auto& w : bodyWrites) env.erase(w);

        // Recurse into body so nested declarations are seen.
        if (fs->body) scanStmt(fs->body.get(), env);
        break;
    }

    // ── While / do-while — invalidate body-written vars, then recurse ────────
    case ASTNodeType::WHILE_STMT: {
        const auto* ws = static_cast<const WhileStmt*>(stmt);
        // Kill all variables written in the loop (may iterate ≥ 0 times).
        std::unordered_set<std::string> writes;
        collectWritten(ws->body.get(), writes);
        for (const auto& w : writes) env.erase(w);
        if (ws->body) scanStmt(ws->body.get(), env);
        break;
    }
    case ASTNodeType::DO_WHILE_STMT: {
        const auto* dw = static_cast<const DoWhileStmt*>(stmt);
        std::unordered_set<std::string> writes;
        collectWritten(dw->body.get(), writes);
        for (const auto& w : writes) env.erase(w);
        if (dw->body) scanStmt(dw->body.get(), env);
        break;
    }

    // ── Block ─────────────────────────────────────────────────────────────────
    case ASTNodeType::BLOCK:
        scanBlock(static_cast<const BlockStmt*>(stmt), env);
        break;

    default:
        break;
    }
}

static void scanBlock(const BlockStmt* block, VarRangeMap& env) {
    if (!block) return;
    for (const auto& s : block->statements)
        scanStmt(s.get(), env);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

VarRangeMap computeVarRanges(const FunctionDecl& fn) {
    if (!fn.body) return {};

    VarRangeMap env;

    // Seed parameter ranges from @range annotations, if present.
    // OmScript allows: `fn foo(x: @range[0, 100])` — future extension.
    // For now, parameters have no known range at function entry.

    scanBlock(fn.body.get(), env);
    return env;
}

} // namespace omscript
