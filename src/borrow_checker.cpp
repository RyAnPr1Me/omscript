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
    std::string refVar;   ///< Name of the borrow alias
    std::string srcVar;   ///< Name of the source variable
    bool        isMut;    ///< True for mutable borrows
};

/// Make a DiagnosticError with source location extracted from an ASTNode.
static DiagnosticError makeBorrowError(ErrorCode code,
                                        const std::string& message,
                                        const ASTNode* node) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::Error;
    diag.code     = code;
    diag.message  = message;
    if (node) {
        diag.location.line   = node->line;
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
            // Mutable borrow in either branch.
            rs.mutBorrowed = rs.mutBorrowed || sb.mutBorrowed;
            // Frozen in either branch.
            rs.frozen = rs.frozen || sb.frozen;
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
    BorrowMap           states_;     ///< Per-variable borrow state
    /// Stack of borrow scopes.  Each scope holds records to release on exit.
    std::vector<std::vector<BorrowRecord>> scopeStack_;

    // ── State accessors ────────────────────────────────────────────────────

    BorrowState& stateOf(const std::string& name) {
        return states_[name];  // default-constructed if absent
    }
    const BorrowState* stateOfOpt(const std::string& name) const {
        auto it = states_.find(name);
        return it == states_.end() ? nullptr : &it->second;
    }

    // ── Scope management ──────────────────────────────────────────────────

    void pushScope() { scopeStack_.emplace_back(); }

    void popScope() {
        if (scopeStack_.empty()) return;
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
        if (it == states_.end()) return;
        BorrowState& s = it->second;
        if (rec.isMut) {
            s.mutBorrowed = false;
        } else {
            if (s.immutBorrows > 0) --s.immutBorrows;
        }
        // Remove the reference variable itself (it goes out of scope).
        states_.erase(rec.refVar);
    }

    // ── Borrow state saving/restoring for branches ────────────────────────

    BorrowMap saveState()     const { return states_; }
    void      restoreState(BorrowMap m) { states_ = std::move(m); }

    // ── Checker core ──────────────────────────────────────────────────────

    /// Check that @p name can be READ at @p site.
    void checkRead(const std::string& name, const ASTNode* site) {
        const auto* s = stateOfOpt(name);
        if (!s) return;  // never declared → let codegen handle
        if (s->moved) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                "use of moved variable '" + name + "'", site);
        }
        if (s->invalidated) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                "use of invalidated variable '" + name + "'", site);
        }
        if (s->mutBorrowed) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                "cannot read '" + name + "' — it has an active mutable borrow", site);
        }
    }

    /// Check that @p name can be WRITTEN at @p site.
    void checkWrite(const std::string& name, const ASTNode* site) {
        const auto* s = stateOfOpt(name);
        if (!s) return;
        if (s->moved) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                "write to moved variable '" + name + "'", site);
        }
        if (s->invalidated) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                "write to invalidated variable '" + name + "'", site);
        }
        if (s->mutBorrowed) {
            throw makeBorrowError(ErrorCode::E016_BORROW_WRITE_CONFLICT,
                "cannot write to '" + name + "' — it has an active mutable borrow", site);
        }
        if (s->immutBorrows > 0) {
            throw makeBorrowError(ErrorCode::E016_BORROW_WRITE_CONFLICT,
                "cannot write to '" + name + "' — it has " +
                std::to_string(s->immutBorrows) + " active immutable borrow(s)", site);
        }
        if (s->frozen) {
            // Frozen variables are const — but E005 handles that.
        }
    }

    /// Check that @p name can be MOVED at @p site.
    void checkMove(const std::string& name, const ASTNode* site) {
        const auto* s = stateOfOpt(name);
        if (!s) return;
        if (s->moved) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                "cannot move '" + name + "' — it was already moved", site);
        }
        if (s->invalidated) {
            throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                "cannot move invalidated variable '" + name + "'", site);
        }
        if (s->mutBorrowed || s->immutBorrows > 0) {
            throw makeBorrowError(ErrorCode::E018_MOVE_WHILE_BORROWED,
                "cannot move '" + name + "' — it has active borrow(s); "
                "end the borrow before moving", site);
        }
    }

    // ── Expression traversal ──────────────────────────────────────────────

    /// Recursively scan @p expr for reads, writes, moves, and borrows.
    /// Returns the "source variable" name if the expression is a simple
    /// identifier read (used to propagate borrow source info).
    void checkExpr(const Expression* expr) {
        if (!expr) return;
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
            s.moved      = false;
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
                if (ue->op == "&") srcExpr = ue->operand.get();
            }

            std::string srcName;
            if (srcExpr && srcExpr->type == ASTNodeType::IDENTIFIER_EXPR)
                srcName = static_cast<const IdentifierExpr*>(srcExpr)->name;

            if (!srcName.empty()) {
                BorrowState& src = stateOf(srcName);
                if (src.isDead()) {
                    throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                        "cannot borrow '" + srcName + "' — variable is " +
                        (src.moved ? "moved" : "invalidated"), vd);
                }
                if (bw->isMut) {
                    if (src.immutBorrows > 0) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                            "cannot create mutable borrow of '" + srcName +
                            "' — it already has " +
                            std::to_string(src.immutBorrows) +
                            " active immutable borrow(s)", vd);
                    }
                    if (src.mutBorrowed) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                            "cannot create mutable borrow of '" + srcName +
                            "' — it already has an active mutable borrow", vd);
                    }
                    src.mutBorrowed = true;
                    registerBorrow({vd->name, srcName, true});
                } else {
                    if (src.mutBorrowed) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                            "cannot create immutable borrow of '" + srcName +
                            "' — it already has an active mutable borrow", vd);
                    }
                    ++src.immutBorrows;
                    registerBorrow({vd->name, srcName, false});
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
                    throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                        "cannot reborrow '" + srcName + "' — variable is " +
                        (src.moved ? "moved" : "invalidated"), vd);
                }
                if (rb->isMut) {
                    if (src.mutBorrowed || src.immutBorrows > 0) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                            "cannot create mutable reborrow of '" + srcName +
                            "' — it already has active borrow(s)", vd);
                    }
                    src.mutBorrowed = true;
                    registerBorrow({vd->name, srcName, true});
                } else {
                    if (src.mutBorrowed) {
                        throw makeBorrowError(ErrorCode::E017_DOUBLE_MUT_BORROW,
                            "cannot create immutable reborrow of '" + srcName +
                            "' — it already has an active mutable borrow", vd);
                    }
                    ++src.immutBorrows;
                    registerBorrow({vd->name, srcName, false});
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
        if (!stmt) return;
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
        case ASTNodeType::EXPR_STMT: {
            const auto* es = static_cast<const ExprStmt*>(stmt);
            checkExpr(es->expression.get());
            break;
        }
        case ASTNodeType::RETURN_STMT: {
            const auto* rs = static_cast<const ReturnStmt*>(stmt);
            if (rs->value) checkExpr(rs->value.get());
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
            if (fs->step) checkExpr(fs->step.get());
            // Loop variable is a fresh integer — always Owned.
            stateOf(fs->varName) = BorrowState{};
            auto preLoop = saveState();
            pushScope();
            checkStmt(fs->body.get());
            popScope();
            restoreState(joinMaps(preLoop, states_));
            break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            const auto* fes = static_cast<const ForEachStmt*>(stmt);
            checkExpr(fes->iterable.get());
            stateOf(fes->varName) = BorrowState{};
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
                if (first) { joinedAfter = states_; first = false; }
                else        joinedAfter = joinMaps(joinedAfter, states_);
            }
            restoreState(joinedAfter);
            break;
        }
        case ASTNodeType::INVALIDATE_STMT: {
            const auto* iv = static_cast<const InvalidateStmt*>(stmt);
            auto& s = stateOf(iv->varName);
            s.invalidated = true;
            s.moved = false;
            break;
        }
        case ASTNodeType::FREEZE_STMT: {
            const auto* fz = static_cast<const FreezeStmt*>(stmt);
            const auto* s = stateOfOpt(fz->varName);
            if (s) {
                if (s->isDead()) {
                    throw makeBorrowError(ErrorCode::E015_USE_AFTER_MOVE,
                        "cannot freeze '" + fz->varName + "' — variable is " +
                        (s->moved ? "moved" : "invalidated"), stmt);
                }
                if (s->mutBorrowed) {
                    throw makeBorrowError(ErrorCode::E016_BORROW_WRITE_CONFLICT,
                        "cannot freeze '" + fz->varName +
                        "' — it has an active mutable borrow", stmt);
                }
            }
            stateOf(fz->varName).frozen = true;
            break;
        }
        case ASTNodeType::THROW_STMT: {
            const auto* ts = static_cast<const ThrowStmt*>(stmt);
            if (ts->value) checkExpr(ts->value.get());
            break;
        }
        case ASTNodeType::DEFER_STMT: {
            const auto* ds = static_cast<const DeferStmt*>(stmt);
            if (ds->body) checkStmt(ds->body.get());
            break;
        }
        default:
            // Break, Continue, Assume, Prefetch, etc. — no ownership effects.
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

BorrowCheckResult runBorrowCheck(const Program& program, bool verbose) {
    BorrowCheckResult result;
    for (const auto& fn : program.functions) {
        if (!fn || !fn->body) continue;
        if (verbose)
            std::cerr << "[borrow-check] checking function '" << fn->name << "'\n";
        BorrowChecker checker(*fn);
        checker.run();  // throws DiagnosticError on first violation
    }
    return result;
}

} // namespace omscript
