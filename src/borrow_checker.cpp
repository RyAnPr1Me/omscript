/// @file borrow_checker.cpp
/// @brief Standalone AST-level borrow checker for OmScript — implementation.
///
/// See borrow_checker.h for the full algorithm description.

#include "borrow_checker.h"

#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

using BorrowMap = std::unordered_map<std::string, BorrowState>;

/// A borrow record: the alias variable that holds the borrow, and the source
/// variable being borrowed.
struct BorrowRecord {
    std::string refVar; ///< Name of the borrow alias
    std::string srcVar; ///< Name of the source variable
    bool isMut;         ///< True for mutable borrows
    bool isReborrow;    ///< True when created via `reborrow` (not `borrow`)
};

/// Make a DiagnosticError with source location extracted from an ASTNode.
static DiagnosticError makeBorrowError(ErrorCode code, const std::string& message, const ASTNode* node) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::Error;
    diag.code = code;
    diag.message = message;
    if (node) {
        diag.location.line = node->line;
        diag.location.column = node->column;
    }
    return DiagnosticError(diag);
}

/// Join two BorrowMaps conservatively: the resulting state is the most
/// restrictive of the two (moved/invalidated/borrowed in either → same in join).
static BorrowMap joinMaps(const BorrowMap& a, const BorrowMap& b) {
    BorrowMap result;
    // Start from map `a` and for each key in `b`, update conservatively.
    for (const auto& [name, sa] : a) {
        result[name] = sa;
    }
    for (const auto& [name, sb] : b) {
        auto it = result.find(name);
        if (it == result.end()) {
            result[name] = sb;
        } else {
            BorrowState& rs = it->second;
            // Moved in either branch → moved in join.
            rs.moved = rs.moved || sb.moved;
            // Invalidated in either branch → invalidated in join.
            rs.invalidated = rs.invalidated || sb.invalidated;
            // Take maximum borrow count (conservative).
            rs.immutBorrows = std::max(rs.immutBorrows, sb.immutBorrows);
            rs.reborrows = std::max(rs.reborrows, sb.reborrows);
            // Mutable borrow in either branch.
            rs.mutBorrowed = rs.mutBorrowed || sb.mutBorrowed;
            // Frozen in either branch.
            rs.frozen = rs.frozen || sb.frozen;
            // Shared in either branch.
            rs.shared = rs.shared || sb.shared;
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// BorrowChecker — stateful per-function checker
// ─────────────────────────────────────────────────────────────────────────────

class BorrowChecker {
  public:
    explicit BorrowChecker(const FunctionDecl& fn) : fn_(fn) {}

    /// Run the checker.  Throws DiagnosticError on the first violation.
    void run();

  private:
    const FunctionDecl& fn_;
    BorrowMap states_; ///< Per-variable borrow state
    /// Stack of borrow scopes.  Each scope holds records to release on exit.
    std::vector<std::vector<BorrowRecord>> scopeStack_;
    /// Deferred statement bodies per scope — run at scope exit (LIFO), in
    /// reverse order to match the "last defer runs first" semantics.
    std::vector<std::vector<const Statement*>> deferredBodies_;

    // ── State accessors ────────────────────────────────────────────────────

    BorrowState& stateOf(const std::string& name) {
        return states_[name]; // default-constructed if absent
    }
    const BorrowState* stateOfOpt(const std::string& name) const {
        auto it = states_.find(name);
        return it == states_.end() ? nullptr : &it->second;
    }

    // ── Scope management ──────────────────────────────────────────────────

    void pushScope() {
        scopeStack_.emplace_back();
        deferredBodies_.emplace_back();
    }

    void popScope() {
        if (scopeStack_.empty())
            return;
        // Run deferred bodies in reverse order (last defer fires first).
        if (!deferredBodies_.empty()) {
            for (auto it = deferredBodies_.back().rbegin(); it != deferredBodies_.back().rend(); ++it) {
                if (*it)
                    checkStmt(*it);
            }
            deferredBodies_.pop_back();
        }
        // Release all borrows registered in the innermost scope.
        for (const auto& rec : scopeStack_.back())
            releaseBorrow(rec);
        scopeStack_.pop_back();
    }

    void registerBorrow(const BorrowRecord& rec) {
        if (!scopeStack_.empty())
            scopeStack_.back().push_back(rec);
    }

    void releaseBorrow(const BorrowRecord& rec) {
        auto it = states_.find(rec.srcVar);
        if (it == states_.end())
            return;
        BorrowState& s = it->second;
        if (rec.isMut) {
            s.mutBorrowed = false;
        } else if (rec.isReborrow) {
            if (s.reborrows > 0)
                --s.reborrows;
        } else {
            if (s.immutBorrows > 0)
                --s.immutBorrows;
        }
        // Remove the reference variable itself (it goes out of scope).
        states_.erase(rec.refVar);
    }

    // ── Borrow state saving/restoring for branches ────────────────────────

    BorrowMap saveState() const {
        return states_;
    }
    void restoreState(BorrowMap m) {
        states_ = std::move(m);
    }

    // ── Checker core ──────────────────────────────────────────────────────

    /// Check that @p name can be READ at @p site.
    void checkRead(const std::string& name, const ASTNode* site) {
        const auto* s = stateOfOpt(name);
        if (!s)
            return; // never declared → let codegen handle
        if (s->moved) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE, "use of moved variable '" + name + "'", site);
        }
        if (s->invalidated) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE, "use of invalidated variable '" + name + "'", site);
        }
        if (s->mutBorrowed) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                                  "cannot read '" + name + "' — it has an active mutable borrow", site);
        }
    }

    /// Check that @p name can be WRITTEN at @p site.
    void checkWrite(const std::string& name, const ASTNode* site) {
        const auto* s = stateOfOpt(name);
        if (!s)
            return;
        if (s->moved) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE, "write to moved variable '" + name + "'", site);
        }
        if (s->invalidated) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE, "write to invalidated variable '" + name + "'", site);
        }
        if (s->mutBorrowed) {
            throw makeBorrowError(ErrorCode::E016_BORROW_WRITE_CONFLICT,
                                  "cannot write to '" + name + "' — it has an active mutable borrow", site);
        }
        if (s->immutBorrows > 0) {
            throw makeBorrowError(ErrorCode::E016_BORROW_WRITE_CONFLICT,
                                  "cannot write to '" + name + "' — it has " + std::to_string(s->immutBorrows) +
                                      " active immutable borrow(s)",
                                  site);
        }
        if (s->shared) {
            throw makeBorrowError(ErrorCode::E020_WRITE_TO_SHARED,
                                  "cannot write to '" + name +
                                      "' — it is in shared ownership state "
                                      "(Ω spec §3.1); use 'own " +
                                      name + ";' to restore unique ownership first",
                                  site);
        }
        if (s->frozen) {
            // Frozen variables are const — but E005 handles that.
        }
    }

    /// Check that @p name can be MOVED at @p site.
    void checkMove(const std::string& name, const ASTNode* site) {
        const auto* s = stateOfOpt(name);
        if (!s)
            return;
        if (s->moved) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE, "cannot move '" + name + "' — it was already moved",
                                  site);
        }
        if (s->invalidated) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE, "cannot move invalidated variable '" + name + "'",
                                  site);
        }
        // Full `borrow` aliases always block moves.
        if (s->mutBorrowed || s->immutBorrows > 0) {
            throw makeBorrowError(ErrorCode::E018_MOVE_WHILE_BORROWED,
                                  "cannot move '" + name +
                                      "' — it has active borrow(s); "
                                      "end the borrow before moving",
                                  site);
        }
        // `reborrow` aliases block moves *unless* the variable is frozen or shared.
        // A frozen/shared variable with only reborrow aliases is movable: `reborrow`
        // is explicitly a short-lived, non-owning alias and the programmer
        // promises the alias won't be used after the move.
        if (s->reborrows > 0 && !s->frozen && !s->shared) {
            throw makeBorrowError(ErrorCode::E018_MOVE_WHILE_BORROWED,
                                  "cannot move '" + name +
                                      "' — it has active reborrow alias(es); "
                                      "end the reborrow scope or freeze/share '" +
                                      name + "' before moving",
                                  site);
        }
    }

    // ── Expression traversal ──────────────────────────────────────────────

    /// Recursively scan @p expr for reads, writes, moves, and borrows.
    /// Returns the "source variable" name if the expression is a simple
    /// identifier read (used to propagate borrow source info).
    void checkExpr(const Expression* expr) {
        if (!expr)
            return;
        switch (expr->type) {
        case ASTNodeType::IDENTIFIER_EXPR: {
            const auto* id = static_cast<const IdentifierExpr*>(expr);
            checkRead(id->name, expr);
            break;
        }
        case ASTNodeType::ASSIGN_EXPR: {
            const auto* ae = static_cast<const AssignExpr*>(expr);
            checkWrite(ae->name, expr);
            checkExpr(ae->value.get());
            // After assignment the variable is live again (not dead).
            auto& s = stateOf(ae->name);
            s.moved = false;
            s.invalidated = false;
            break;
        }
        case ASTNodeType::INDEX_ASSIGN_EXPR: {
            const auto* ia = static_cast<const IndexAssignExpr*>(expr);
            checkExpr(ia->array.get());
            checkExpr(ia->index.get());
            checkExpr(ia->value.get());
            break;
        }
        case ASTNodeType::FIELD_ASSIGN_EXPR: {
            const auto* fa = static_cast<const FieldAssignExpr*>(expr);
            checkExpr(fa->object.get());
            checkExpr(fa->value.get());
            break;
        }
        case ASTNodeType::MOVE_EXPR: {
            const auto* me = static_cast<const MoveExpr*>(expr);
            if (me->source && me->source->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto* id = static_cast<const IdentifierExpr*>(me->source.get());
                checkMove(id->name, expr);
                stateOf(id->name).moved = true;
            }
            break;
        }
        case ASTNodeType::BORROW_EXPR: {
            // BorrowExpr appears only as the initializer of a VarDecl —
            // handled in checkVarDecl; ignore here to avoid double-checking.
            break;
        }
        case ASTNodeType::REBORROW_EXPR: {
            // Similarly handled in checkVarDecl.
            break;
        }
        case ASTNodeType::BINARY_EXPR: {
            const auto* bin = static_cast<const BinaryExpr*>(expr);
            checkExpr(bin->left.get());
            checkExpr(bin->right.get());
            break;
        }
        case ASTNodeType::UNARY_EXPR: {
            const auto* un = static_cast<const UnaryExpr*>(expr);
            checkExpr(un->operand.get());
            break;
        }
        case ASTNodeType::TERNARY_EXPR: {
            const auto* tern = static_cast<const TernaryExpr*>(expr);
            checkExpr(tern->condition.get());
            auto snap = saveState();
            checkExpr(tern->thenExpr.get());
            auto afterThen = saveState();
            restoreState(snap);
            checkExpr(tern->elseExpr.get());
            restoreState(joinMaps(afterThen, states_));
            break;
        }
        case ASTNodeType::CALL_EXPR: {
            const auto* call = static_cast<const CallExpr*>(expr);
            for (const auto& arg : call->arguments)
                checkExpr(arg.get());
            break;
        }
        case ASTNodeType::INDEX_EXPR: {
            const auto* ie = static_cast<const IndexExpr*>(expr);
            checkExpr(ie->array.get());
            checkExpr(ie->index.get());
            break;
        }
        case ASTNodeType::FIELD_ACCESS_EXPR: {
            const auto* fa = static_cast<const FieldAccessExpr*>(expr);
            checkExpr(fa->object.get());
            break;
        }
        default:
            // Literals, type-cast exprs, comptime exprs — no ownership
            break;
        }
    }

    // ── Statement traversal ───────────────────────────────────────────────

    void checkVarDecl(const VarDecl* vd) {
        if (!vd->initializer) {
            // Default initialised: variable is Owned.
            stateOf(vd->name) = BorrowState{};
            return;
        }

        // ── Borrow declaration ────────────────────────────────────────────
        if (vd->initializer->type == ASTNodeType::BORROW_EXPR) {
            const auto* bw = static_cast<const BorrowExpr*>(vd->initializer.get());
            // Find the source identifier (may be wrapped in &expr).
            const Expression* srcExpr = bw->source.get();
            if (srcExpr && srcExpr->type == ASTNodeType::UNARY_EXPR) {
                const auto* ue = static_cast<const UnaryExpr*>(srcExpr);
                if (ue->op == "&")
                    srcExpr = ue->operand.get();
            }

            std::string srcName;
            if (srcExpr && srcExpr->type == ASTNodeType::IDENTIFIER_EXPR)
                srcName = static_cast<const IdentifierExpr*>(srcExpr)->name;

            if (!srcName.empty()) {
                BorrowState& src = stateOf(srcName);
                if (src.isDead()) {
                    throw makeBorrowError(
                        ErrorCode::E015_USE_AFTER_MOVE,
                        "cannot borrow '" + srcName + "' — variable is " + (src.moved ? "moved" : "invalidated"), vd);
                }
                if (bw->isMut) {
                    if (src.immutBorrows > 0) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                                              "cannot create mutable borrow of '" + srcName + "' — it already has " +
                                                  std::to_string(src.immutBorrows) + " active immutable borrow(s)",
                                              vd);
                    }
                    if (src.mutBorrowed) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                                              "cannot create mutable borrow of '" + srcName +
                                                  "' — it already has an active mutable borrow",
                                              vd);
                    }
                    src.mutBorrowed = true;
                    registerBorrow({vd->name, srcName, true, /*isReborrow=*/false});
                } else {
                    if (src.mutBorrowed) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                                              "cannot create immutable borrow of '" + srcName +
                                                  "' — it already has an active mutable borrow",
                                              vd);
                    }
                    ++src.immutBorrows;
                    registerBorrow({vd->name, srcName, false, /*isReborrow=*/false});
                }
                // The borrow alias itself starts as Owned (it's a reference var).
                stateOf(vd->name) = BorrowState{};
            }
            return;
        }

        // ── Reborrow declaration ──────────────────────────────────────────
        if (vd->initializer->type == ASTNodeType::REBORROW_EXPR) {
            const auto* rb = static_cast<const ReborrowExpr*>(vd->initializer.get());
            const Expression* srcExpr = rb->source.get();
            std::string srcName;
            if (srcExpr && srcExpr->type == ASTNodeType::IDENTIFIER_EXPR)
                srcName = static_cast<const IdentifierExpr*>(srcExpr)->name;
            if (!srcName.empty()) {
                BorrowState& src = stateOf(srcName);
                if (src.isDead()) {
                    throw makeBorrowError(
                        ErrorCode::E015_USE_AFTER_MOVE,
                        "cannot reborrow '" + srcName + "' — variable is " + (src.moved ? "moved" : "invalidated"), vd);
                }
                if (rb->isMut) {
                    if (src.mutBorrowed || src.immutBorrows > 0 || src.reborrows > 0) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                                              "cannot create mutable reborrow of '" + srcName +
                                                  "' — it already has active borrow(s)",
                                              vd);
                    }
                    src.mutBorrowed = true;
                    registerBorrow({vd->name, srcName, true, /*isReborrow=*/true});
                } else {
                    if (src.mutBorrowed) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                                              "cannot create immutable reborrow of '" + srcName +
                                                  "' — it already has an active mutable borrow",
                                              vd);
                    }
                    ++src.reborrows;
                    registerBorrow({vd->name, srcName, false, /*isReborrow=*/true});
                }
                stateOf(vd->name) = BorrowState{};
            }
            return;
        }

        // ── Move declaration ──────────────────────────────────────────────
        if (vd->initializer->type == ASTNodeType::MOVE_EXPR) {
            const auto* me = static_cast<const MoveExpr*>(vd->initializer.get());
            if (me->source && me->source->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto* id = static_cast<const IdentifierExpr*>(me->source.get());
                checkMove(id->name, vd);
                stateOf(id->name).moved = true;
            }
            stateOf(vd->name) = BorrowState{};
            return;
        }

        // ── Normal initialiser ────────────────────────────────────────────
        checkExpr(vd->initializer.get());
        stateOf(vd->name) = BorrowState{};
    }

    void checkStmt(const Statement* stmt) {
        if (!stmt)
            return;
        switch (stmt->type) {
        case ASTNodeType::BLOCK: {
            const auto* blk = static_cast<const BlockStmt*>(stmt);
            pushScope();
            for (const auto& s : blk->statements)
                checkStmt(s.get());
            popScope();
            break;
        }
        case ASTNodeType::VAR_DECL: {
            checkVarDecl(static_cast<const VarDecl*>(stmt));
            break;
        }
        case ASTNodeType::MOVE_DECL: {
            const auto* md = static_cast<const MoveDecl*>(stmt);
            if (md->initializer && md->initializer->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto* id = static_cast<const IdentifierExpr*>(md->initializer.get());
                checkMove(id->name, stmt);
                stateOf(id->name).moved = true;
            } else if (md->initializer) {
                checkExpr(md->initializer.get());
            }
            stateOf(md->name) = BorrowState{};
            break;
        }
        case ASTNodeType::PREFETCH_STMT: {
            const auto* ps = static_cast<const PrefetchStmt*>(stmt);
            if (ps->varDecl) {
                // prefetch var x = expr — treat like VAR_DECL.
                checkVarDecl(ps->varDecl.get());
            } else if (ps->addrExpr) {
                checkExpr(ps->addrExpr.get());
            }
            break;
        }
        case ASTNodeType::EXPR_STMT: {
            const auto* es = static_cast<const ExprStmt*>(stmt);
            checkExpr(es->expression.get());
            break;
        }
        case ASTNodeType::RETURN_STMT: {
            const auto* rs = static_cast<const ReturnStmt*>(stmt);
            if (rs->value)
                checkExpr(rs->value.get());
            break;
        }
        case ASTNodeType::IF_STMT: {
            const auto* ifc = static_cast<const IfStmt*>(stmt);
            checkExpr(ifc->condition.get());
            auto snap = saveState();
            // Check then-branch.
            pushScope();
            checkStmt(ifc->thenBranch.get());
            popScope();
            auto afterThen = saveState();
            // Check else-branch (if present).
            restoreState(snap);
            if (ifc->elseBranch) {
                pushScope();
                checkStmt(ifc->elseBranch.get());
                popScope();
            }
            // Join the two states conservatively.
            restoreState(joinMaps(afterThen, states_));
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            const auto* ws = static_cast<const WhileStmt*>(stmt);
            checkExpr(ws->condition.get());
            auto preLoop = saveState();
            pushScope();
            checkStmt(ws->body.get());
            popScope();
            // Merge pre-loop and post-body: conservative join.
            restoreState(joinMaps(preLoop, states_));
            break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            const auto* dw = static_cast<const DoWhileStmt*>(stmt);
            auto preLoop = saveState();
            pushScope();
            checkStmt(dw->body.get());
            popScope();
            checkExpr(dw->condition.get());
            restoreState(joinMaps(preLoop, states_));
            break;
        }
        case ASTNodeType::FOR_STMT: {
            const auto* fs = static_cast<const ForStmt*>(stmt);
            checkExpr(fs->start.get());
            checkExpr(fs->end.get());
            if (fs->step)
                checkExpr(fs->step.get());
            // Loop variable is a fresh integer — always Owned.
            stateOf(fs->iteratorVar) = BorrowState{};
            auto preLoop = saveState();
            pushScope();
            checkStmt(fs->body.get());
            popScope();
            restoreState(joinMaps(preLoop, states_));
            break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            const auto* fes = static_cast<const ForEachStmt*>(stmt);
            checkExpr(fes->collection.get());
            stateOf(fes->iteratorVar) = BorrowState{};
            auto preLoop = saveState();
            pushScope();
            checkStmt(fes->body.get());
            popScope();
            restoreState(joinMaps(preLoop, states_));
            break;
        }
        case ASTNodeType::SWITCH_STMT: {
            const auto* sw = static_cast<const SwitchStmt*>(stmt);
            checkExpr(sw->condition.get());
            auto snap = saveState();
            BorrowMap joinedAfter = states_;
            bool first = true;
            for (const auto& c : sw->cases) {
                restoreState(snap);
                pushScope();
                for (const auto& s : c.body)
                    checkStmt(s.get());
                popScope();
                if (first) {
                    joinedAfter = states_;
                    first = false;
                } else
                    joinedAfter = joinMaps(joinedAfter, states_);
            }
            restoreState(joinedAfter);
            break;
        }
        case ASTNodeType::INVALIDATE_STMT: {
            const auto* iv = static_cast<const InvalidateStmt*>(stmt);
            const auto* s = stateOfOpt(iv->varName);
            if (s) {
                if (s->invalidated) {
                    // E019 — double-invalidate: variable already freed at compile time.
                    throw makeBorrowError(
                        ErrorCode::E019_DOUBLE_INVALIDATE,
                        "double invalidation of '" + iv->varName + "' — variable was already invalidated", stmt);
                }
                if (s->moved) {
                    throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                                          "cannot invalidate '" + iv->varName + "' — variable was already moved", stmt);
                }
                // E022 — invalidate while borrowed: active borrows still reference
                // this variable; freeing the memory now would dangle the alias.
                if (s->mutBorrowed || s->immutBorrows > 0 || s->reborrows > 0) {
                    std::string detail;
                    if (s->mutBorrowed)
                        detail = "a mutable borrow";
                    else if (s->immutBorrows > 0)
                        detail = std::to_string(s->immutBorrows) + " immutable borrow(s)";
                    else
                        detail = std::to_string(s->reborrows) + " reborrow alias(es)";
                    throw makeBorrowError(ErrorCode::E022_INVALIDATE_WHILE_BORROWED,
                                          "cannot invalidate '" + iv->varName + "' — it still has " + detail +
                                              " active; end all borrows before invalidating (Ω spec §6.2)",
                                          stmt);
                }
            }
            // Use stateOf() to upsert the entry once — if s was non-null above
            // the entry already exists; if s was null we create a fresh entry.
            // Calling stateOf() twice would insert two default entries if the
            // map reallocates between the two calls.
            auto& ms = stateOf(iv->varName);
            ms.invalidated = true;
            ms.moved = false;
            break;
        }
        case ASTNodeType::FREEZE_STMT: {
            const auto* fz = static_cast<const FreezeStmt*>(stmt);
            const auto* s = stateOfOpt(fz->varName);
            if (s) {
                if (s->isDead()) {
                    throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                                          "cannot freeze '" + fz->varName + "' — variable is " +
                                              (s->moved ? "moved" : "invalidated"),
                                          stmt);
                }
                if (s->mutBorrowed) {
                    throw makeBorrowError(ErrorCode::E016_BORROW_WRITE_CONFLICT,
                                          "cannot freeze '" + fz->varName + "' — it has an active mutable borrow",
                                          stmt);
                }
            }
            stateOf(fz->varName).frozen = true;
            break;
        }
        case ASTNodeType::SHARED_STMT: {
            // `shared x;` — transition x to shared ownership (Ω spec §3.1).
            // Read-only aliasable: multiple immutable borrows allowed,
            // mutable borrows and mutations are henceforth errors.
            const auto* sh = static_cast<const SharedStmt*>(stmt);
            const auto* s = stateOfOpt(sh->varName);
            if (s) {
                if (s->isDead()) {
                    throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                                          "cannot mark '" + sh->varName + "' as shared — variable is " +
                                              (s->moved ? "moved" : "invalidated"),
                                          stmt);
                }
                if (s->mutBorrowed) {
                    throw makeBorrowError(
                        ErrorCode::E016_BORROW_WRITE_CONFLICT,
                        "cannot mark '" + sh->varName + "' as shared — it has an active mutable borrow", stmt);
                }
            }
            stateOf(sh->varName).shared = true;
            break;
        }
        case ASTNodeType::OWN_STMT: {
            // `own x;` — explicitly assert unique ownership (Ω spec §3.1).
            // Clears shared state; verifies no active borrows remain.
            const auto* ow = static_cast<const OwnStmt*>(stmt);
            const auto* s = stateOfOpt(ow->varName);
            if (s) {
                if (s->isDead()) {
                    throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                                          "cannot assert ownership of '" + ow->varName + "' — variable is " +
                                              (s->moved ? "moved" : "invalidated"),
                                          stmt);
                }
                // E021 — own on frozen: freeze is irreversible; 'own' cannot
                // downgrade a frozen variable back to a writable owned state.
                if (s->frozen) {
                    throw makeBorrowError(ErrorCode::E021_OWN_ON_FROZEN,
                                          "cannot assert unique ownership of '" + ow->varName +
                                              "' — it is frozen (freeze is irreversible; use a new variable if "
                                              "you need a mutable copy)",
                                          stmt);
                }
                if (s->mutBorrowed || s->immutBorrows > 0 || s->reborrows > 0) {
                    throw makeBorrowError(ErrorCode::E018_MOVE_WHILE_BORROWED,
                                          "cannot assert unique ownership of '" + ow->varName +
                                              "' — it has active borrow(s); end all borrows first",
                                          stmt);
                }
            }
            // Clear shared state; frozen remains (freeze is stronger than shared).
            stateOf(ow->varName).shared = false;
            break;
        }
        case ASTNodeType::THROW_STMT: {
            const auto* ts = static_cast<const ThrowStmt*>(stmt);
            if (ts->value)
                checkExpr(ts->value.get());
            break;
        }
        case ASTNodeType::CATCH_STMT: {
            const auto* cs = static_cast<const CatchStmt*>(stmt);
            // The catch handler runs in its own borrow scope: borrows made
            // inside the handler must be released before the handler exits.
            if (cs->body) {
                pushScope();
                for (const auto& s : cs->body->statements)
                    checkStmt(s.get());
                popScope();
            }
            break;
        }
        case ASTNodeType::DEFER_STMT: {
            const auto* ds = static_cast<const DeferStmt*>(stmt);
            // The deferred body runs at the END of the enclosing scope, not
            // at the point the `defer` statement appears.  Register it to be
            // checked when the scope is popped so that variables declared
            // after the defer can still be used between the defer and scope
            // exit without a false "use of invalidated variable" error.
            if (ds->body && !deferredBodies_.empty())
                deferredBodies_.back().push_back(ds->body.get());
            break;
        }
        case ASTNodeType::PIPELINE_STMT: {
            const auto* pl = static_cast<const PipelineStmt*>(stmt);
            if (pl->count)
                checkExpr(pl->count.get());
            // Each stage is a separate scope.
            for (const auto& stage : pl->stages) {
                if (!stage.body)
                    continue;
                pushScope();
                for (const auto& s : stage.body->statements)
                    checkStmt(s.get());
                popScope();
            }
            break;
        }
        case ASTNodeType::ASSUME_STMT: {
            const auto* as = static_cast<const AssumeStmt*>(stmt);
            checkExpr(as->condition.get());
            if (as->deoptBody)
                checkStmt(as->deoptBody.get());
            break;
        }
        default:
            // Break, Continue, etc. — no ownership effects.
            break;
        }
    }
};

// ── BorrowChecker::run ────────────────────────────────────────────────────────

void BorrowChecker::run() {
    // Initialise parameters as Owned (callers pass ownership into function).
    for (const auto& param : fn_.parameters)
        stateOf(param.name) = BorrowState{};

    // Check function body inside a single top-level scope.
    if (fn_.body) {
        pushScope();
        checkStmt(fn_.body.get());
        popScope();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runBorrowCheck — public entry point
// ─────────────────────────────────────────────────────────────────────────────

// Forward declaration for the mem-sanitizer helper.
static void runMemSanitizer(const Program& program, BorrowCheckResult& result);

BorrowCheckResult runBorrowCheck(const Program& program, bool verbose, bool noOwnershipChecks, bool memSanitize) {
    BorrowCheckResult result;

    if (!noOwnershipChecks) {
        for (const auto& fn : program.functions) {
            if (!fn || !fn->body)
                continue;
            if (verbose)
                std::cerr << "[borrow-check] checking function '" << fn->name << "'\n";
            BorrowChecker checker(*fn);
            checker.run(); // throws DiagnosticError on first violation
        }
    } else if (verbose) {
        std::cerr << "[borrow-check] ownership checks disabled (--no-ownership-checks)\n";
    }

    if (memSanitize) {
        runMemSanitizer(program, result);
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// MemSanitizer — compile-time path-sensitive diagnostics (Ω spec §7)
// ─────────────────────────────────────────────────────────────────────────────

// Walk the AST to collect: (a) invalidation/move events with their location,
// (b) uses of the same variable after its death point.
// This is a simplified CFG-aware scan: we track death events sequentially,
// and for each subsequent use of the same variable we report a diagnostic.

struct MemEvent {
    enum class Kind { Invalidate, Move, NullAssign };
    std::string varName;
    Kind kind;
    int line;
    std::string desc; // human-readable description
};

/// Collect memory events from a statement list (simplified sequential scan).
static void collectMemEvents(const Statement* stmt, std::vector<MemEvent>& events,
                             std::unordered_map<std::string, MemEvent>& deathMap, BorrowCheckResult& result,
                             const std::string& file) {
    if (!stmt)
        return;

    auto reportUse = [&](const std::string& varName, int useLine, const std::string& useDesc) {
        auto it = deathMap.find(varName);
        if (it == deathMap.end())
            return;
        const auto& ev = it->second;
        MemSanitizerDiag diag;
        diag.file = file;
        diag.varName = varName;
        diag.causeLine = ev.line;
        diag.useLine = useLine;
        diag.useDesc = useDesc;
        if (ev.kind == MemEvent::Kind::Invalidate) {
            diag.kind = "use-after-invalidate";
            diag.causeDesc = "invalidate " + varName;
        } else if (ev.kind == MemEvent::Kind::Move) {
            diag.kind = "use-after-move";
            diag.causeDesc = "move " + varName;
        } else {
            diag.kind = "null-deref";
            diag.causeDesc = varName + " = null";
        }
        result.memSanitizerDiags.push_back(std::move(diag));
    };

    // Scan for dereferences of dead pointers in an expression.
    std::function<void(const Expression*, int)> scanExpr = [&](const Expression* expr, int parentLine) {
        if (!expr)
            return;
        const int eline = expr->line ? expr->line : parentLine;

        if (expr->type == ASTNodeType::UNARY_EXPR) {
            const auto* ue = static_cast<const UnaryExpr*>(expr);
            if (ue->op == "deref" && ue->operand && ue->operand->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto* id = static_cast<const IdentifierExpr*>(ue->operand.get());
                reportUse(id->name, eline, "*" + id->name + " (invalid use)");
            }
            scanExpr(ue->operand.get(), eline);
        } else if (expr->type == ASTNodeType::BINARY_EXPR) {
            const auto* be = static_cast<const BinaryExpr*>(expr);
            scanExpr(be->left.get(), eline);
            scanExpr(be->right.get(), eline);
        } else if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
            const auto* id = static_cast<const IdentifierExpr*>(expr);
            reportUse(id->name, eline, id->name + " (use of dead variable)");
        } else if (expr->type == ASTNodeType::CALL_EXPR) {
            const auto* ce = static_cast<const CallExpr*>(expr);
            for (const auto& arg : ce->arguments)
                scanExpr(arg.get(), eline);
        } else if (expr->type == ASTNodeType::INDEX_EXPR) {
            const auto* ie = static_cast<const IndexExpr*>(expr);
            scanExpr(ie->array.get(), eline);
            scanExpr(ie->index.get(), eline);
        }
    };

    switch (stmt->type) {
    case ASTNodeType::INVALIDATE_STMT: {
        const auto* iv = static_cast<const InvalidateStmt*>(stmt);
        deathMap[iv->varName] = {iv->varName, MemEvent::Kind::Invalidate, stmt->line, "invalidate " + iv->varName};
        break;
    }
    case ASTNodeType::MOVE_DECL:
    case ASTNodeType::MOVE_EXPR: {
        // No direct variable name here in all cases, skip for now.
        break;
    }
    case ASTNodeType::VAR_DECL: {
        const auto* vd = static_cast<const VarDecl*>(stmt);
        // If a previously dead variable is re-declared, clear death record.
        deathMap.erase(vd->name);
        if (vd->initializer)
            scanExpr(vd->initializer.get(), stmt->line);
        break;
    }
    case ASTNodeType::EXPR_STMT: {
        const auto* es = static_cast<const ExprStmt*>(stmt);
        if (es->expression)
            scanExpr(es->expression.get(), stmt->line);
        break;
    }
    case ASTNodeType::RETURN_STMT: {
        const auto* rs = static_cast<const ReturnStmt*>(stmt);
        if (rs->value)
            scanExpr(rs->value.get(), stmt->line);
        break;
    }
    case ASTNodeType::BLOCK: {
        const auto* bs = static_cast<const BlockStmt*>(stmt);
        for (const auto& s : bs->statements)
            collectMemEvents(s.get(), events, deathMap, result, file);
        break;
    }
    case ASTNodeType::IF_STMT: {
        const auto* is = static_cast<const IfStmt*>(stmt);
        if (is->condition)
            scanExpr(is->condition.get(), stmt->line);
        if (is->thenBranch)
            collectMemEvents(is->thenBranch.get(), events, deathMap, result, file);
        if (is->elseBranch)
            collectMemEvents(is->elseBranch.get(), events, deathMap, result, file);
        break;
    }
    case ASTNodeType::WHILE_STMT: {
        const auto* ws = static_cast<const WhileStmt*>(stmt);
        if (ws->condition)
            scanExpr(ws->condition.get(), stmt->line);
        if (ws->body)
            collectMemEvents(ws->body.get(), events, deathMap, result, file);
        break;
    }
    default:
        break;
    }
}

static void runMemSanitizer(const Program& program, BorrowCheckResult& result) {
    for (const auto& fn : program.functions) {
        if (!fn || !fn->body)
            continue;
        std::vector<MemEvent> events;
        std::unordered_map<std::string, MemEvent> deathMap;
        collectMemEvents(fn->body.get(), events, deathMap, result, fn->name);
    }

    // Emit diagnostics to stderr in the spec format.
    for (const auto& d : result.memSanitizerDiags) {
        std::cerr << "MEM-SANITIZER ERROR\n"
                  << d.file << ":" << d.useLine << "\n\n"
                  << d.kind << " detected\n"
                  << "variable: " << d.varName << "\n"
                  << "control path:\n"
                  << "  line " << d.causeLine << " -> " << d.causeDesc << "\n"
                  << "  line " << d.useLine << " -> " << d.useDesc << "\n\n";
    }
}

} // namespace omscript
