/// @file rlc_pass.cpp
/// @brief Region Lifetime Coalescing (RLC) pass implementation.
///
/// Algorithm overview for each function:
///   1. Scan the top-level block for region creation and invalidate events.
///   2. Verify that every region variable is invalidated (E013 check).
///   3. Detect use-after-invalidate violations (E014 check).
///   4. Find coalescing candidates: pairs (R1, R2) where R1 is invalidated
///      strictly before R2 is created, and R1 does not escape after invalidate.
///   5. Apply the transformation:
///        – Remove R2's creation statement (newRegion()).
///        – Remove R1's invalidate statement (the early one).
///        – Rename R2 → canonical(R1) throughout the remaining statements.
///        – R2's invalidate (now renamed to R1) becomes the single lifetime end.
///   Repeat until no more candidates are found (handles chains).

#include "rlc_pass.h"
#include "diagnostic.h"
#include "pass_utils.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: check whether an expression/statement is a specific region call
// ─────────────────────────────────────────────────────────────────────────────

/// True when @p expr is a call to newRegion() (zero arguments).
static bool isNewRegionCall(const Expression* expr) {
    if (!expr || expr->type != ASTNodeType::CALL_EXPR) return false;
    const auto* c = static_cast<const CallExpr*>(expr);
    return c->callee == "newRegion" && c->arguments.empty();
}

/// If @p expr is `alloc(r, ...)`, sets *regionVar to the first argument name
/// and returns true.
static bool isAllocCall(const Expression* expr, std::string* regionVar = nullptr) {
    if (!expr || expr->type != ASTNodeType::CALL_EXPR) return false;
    const auto* c = static_cast<const CallExpr*>(expr);
    if (c->callee != "alloc" || c->arguments.empty()) return false;
    if (regionVar && c->arguments[0]->type == ASTNodeType::IDENTIFIER_EXPR) {
        *regionVar = static_cast<const IdentifierExpr*>(c->arguments[0].get())->name;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: check whether an expression / statement references a variable name
// ─────────────────────────────────────────────────────────────────────────────

static bool exprUsesVar(const Expression* expr, const std::string& name);
static bool stmtUsesVar(const Statement* stmt, const std::string& name);

static bool exprUsesVar(const Expression* expr, const std::string& name) {
    if (!expr) return false;
    switch (expr->type) {
        case ASTNodeType::IDENTIFIER_EXPR:
            return static_cast<const IdentifierExpr*>(expr)->name == name;
        case ASTNodeType::BINARY_EXPR: {
            const auto* b = static_cast<const BinaryExpr*>(expr);
            return exprUsesVar(b->left.get(), name) || exprUsesVar(b->right.get(), name);
        }
        case ASTNodeType::UNARY_EXPR:
            return exprUsesVar(static_cast<const UnaryExpr*>(expr)->operand.get(), name);
        case ASTNodeType::POSTFIX_EXPR:
            return exprUsesVar(static_cast<const PostfixExpr*>(expr)->operand.get(), name);
        case ASTNodeType::PREFIX_EXPR:
            return exprUsesVar(static_cast<const PrefixExpr*>(expr)->operand.get(), name);
        case ASTNodeType::CALL_EXPR: {
            const auto* c = static_cast<const CallExpr*>(expr);
            for (const auto& arg : c->arguments)
                if (exprUsesVar(arg.get(), name)) return true;
            return false;
        }
        case ASTNodeType::ASSIGN_EXPR: {
            const auto* a = static_cast<const AssignExpr*>(expr);
            return a->name == name || exprUsesVar(a->value.get(), name);
        }
        case ASTNodeType::TERNARY_EXPR: {
            const auto* t = static_cast<const TernaryExpr*>(expr);
            return exprUsesVar(t->condition.get(), name) ||
                   exprUsesVar(t->thenExpr.get(), name) ||
                   exprUsesVar(t->elseExpr.get(), name);
        }
        case ASTNodeType::ARRAY_EXPR: {
            const auto* a = static_cast<const ArrayExpr*>(expr);
            for (const auto& e : a->elements)
                if (exprUsesVar(e.get(), name)) return true;
            return false;
        }
        case ASTNodeType::INDEX_EXPR: {
            const auto* ix = static_cast<const IndexExpr*>(expr);
            return exprUsesVar(ix->array.get(), name) || exprUsesVar(ix->index.get(), name);
        }
        case ASTNodeType::INDEX_ASSIGN_EXPR: {
            const auto* ia = static_cast<const IndexAssignExpr*>(expr);
            return exprUsesVar(ia->array.get(), name) ||
                   exprUsesVar(ia->index.get(), name) ||
                   exprUsesVar(ia->value.get(), name);
        }
        case ASTNodeType::FIELD_ACCESS_EXPR:
            return exprUsesVar(
                static_cast<const FieldAccessExpr*>(expr)->object.get(), name);
        case ASTNodeType::FIELD_ASSIGN_EXPR: {
            const auto* fa = static_cast<const FieldAssignExpr*>(expr);
            return exprUsesVar(fa->object.get(), name) || exprUsesVar(fa->value.get(), name);
        }
        case ASTNodeType::MOVE_EXPR:
            return exprUsesVar(static_cast<const MoveExpr*>(expr)->source.get(), name);
        case ASTNodeType::BORROW_EXPR:
            return exprUsesVar(static_cast<const BorrowExpr*>(expr)->source.get(), name);
        case ASTNodeType::REBORROW_EXPR:
            return exprUsesVar(
                static_cast<const ReborrowExpr*>(expr)->source.get(), name);
        case ASTNodeType::SPREAD_EXPR:
            return exprUsesVar(
                static_cast<const SpreadExpr*>(expr)->operand.get(), name);
        case ASTNodeType::PIPE_EXPR:
            return exprUsesVar(
                static_cast<const PipeExpr*>(expr)->left.get(), name);
        case ASTNodeType::STRUCT_LITERAL_EXPR: {
            const auto* sl = static_cast<const StructLiteralExpr*>(expr);
            for (const auto& fv : sl->fieldValues)
                if (exprUsesVar(fv.second.get(), name)) return true;
            return false;
        }
        case ASTNodeType::DICT_EXPR: {
            const auto* d = static_cast<const DictExpr*>(expr);
            for (const auto& p : d->pairs)
                if (exprUsesVar(p.first.get(), name) ||
                    exprUsesVar(p.second.get(), name))
                    return true;
            return false;
        }
        case ASTNodeType::COMPTIME_EXPR:
            // Conservative: treat comptime blocks as potentially using any var.
            return true;
        case ASTNodeType::RANGE_ANNOT_EXPR:
            return exprUsesVar(
                static_cast<const RangeAnnotExpr*>(expr)->inner.get(), name);
        default:
            return false;
    }
}

static bool stmtUsesVar(const Statement* stmt, const std::string& name) {
    if (!stmt) return false;
    switch (stmt->type) {
        case ASTNodeType::EXPR_STMT:
            return exprUsesVar(
                static_cast<const ExprStmt*>(stmt)->expression.get(), name);
        case ASTNodeType::VAR_DECL:
            return exprUsesVar(
                static_cast<const VarDecl*>(stmt)->initializer.get(), name);
        case ASTNodeType::RETURN_STMT:
            return exprUsesVar(
                static_cast<const ReturnStmt*>(stmt)->value.get(), name);
        case ASTNodeType::IF_STMT: {
            const auto* is = static_cast<const IfStmt*>(stmt);
            return exprUsesVar(is->condition.get(), name) ||
                   stmtUsesVar(is->thenBranch.get(), name) ||
                   stmtUsesVar(is->elseBranch.get(), name);
        }
        case ASTNodeType::WHILE_STMT: {
            const auto* ws = static_cast<const WhileStmt*>(stmt);
            return exprUsesVar(ws->condition.get(), name) ||
                   stmtUsesVar(ws->body.get(), name);
        }
        case ASTNodeType::DO_WHILE_STMT: {
            const auto* dw = static_cast<const DoWhileStmt*>(stmt);
            return stmtUsesVar(dw->body.get(), name) ||
                   exprUsesVar(dw->condition.get(), name);
        }
        case ASTNodeType::FOR_STMT: {
            const auto* fs = static_cast<const ForStmt*>(stmt);
            return exprUsesVar(fs->start.get(), name) ||
                   exprUsesVar(fs->end.get(), name) ||
                   exprUsesVar(fs->step.get(), name) ||
                   stmtUsesVar(fs->body.get(), name);
        }
        case ASTNodeType::FOR_EACH_STMT: {
            const auto* fe = static_cast<const ForEachStmt*>(stmt);
            return exprUsesVar(fe->collection.get(), name) ||
                   stmtUsesVar(fe->body.get(), name);
        }
        case ASTNodeType::BLOCK: {
            const auto* b = static_cast<const BlockStmt*>(stmt);
            for (const auto& s : b->statements)
                if (stmtUsesVar(s.get(), name)) return true;
            return false;
        }
        case ASTNodeType::SWITCH_STMT: {
            const auto* sw = static_cast<const SwitchStmt*>(stmt);
            if (exprUsesVar(sw->condition.get(), name)) return true;
            for (const auto& cas : sw->cases) {
                if (exprUsesVar(cas.value.get(), name)) return true;
                for (const auto& v : cas.values)
                    if (exprUsesVar(v.get(), name)) return true;
                for (const auto& s : cas.body)
                    if (stmtUsesVar(s.get(), name)) return true;
            }
            return false;
        }
        case ASTNodeType::THROW_STMT:
            return exprUsesVar(
                static_cast<const ThrowStmt*>(stmt)->value.get(), name);
        case ASTNodeType::CATCH_STMT:
            return stmtUsesVar(
                static_cast<const CatchStmt*>(stmt)->body.get(), name);
        case ASTNodeType::DEFER_STMT:
            return stmtUsesVar(
                static_cast<const DeferStmt*>(stmt)->body.get(), name);
        case ASTNodeType::ASSUME_STMT: {
            const auto* as = static_cast<const AssumeStmt*>(stmt);
            return exprUsesVar(as->condition.get(), name) ||
                   stmtUsesVar(as->deoptBody.get(), name);
        }
        case ASTNodeType::INVALIDATE_STMT:
            return static_cast<const InvalidateStmt*>(stmt)->varName == name;
        case ASTNodeType::MOVE_DECL:
            return exprUsesVar(
                static_cast<const MoveDecl*>(stmt)->initializer.get(), name);
        case ASTNodeType::FREEZE_STMT:
            return static_cast<const FreezeStmt*>(stmt)->varName == name;
        case ASTNodeType::PREFETCH_STMT: {
            const auto* ps = static_cast<const PrefetchStmt*>(stmt);
            if (!ps->varName.empty()) return ps->varName == name;
            if (ps->varDecl)
                return exprUsesVar(ps->varDecl->initializer.get(), name);
            return false;
        }
        case ASTNodeType::PIPELINE_STMT: {
            const auto* pl = static_cast<const PipelineStmt*>(stmt);
            if (exprUsesVar(pl->count.get(), name)) return true;
            for (const auto& stage : pl->stages)
                if (stmtUsesVar(stage.body.get(), name)) return true;
            return false;
        }
        default:
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: rename a variable in-place throughout an expression / statement
// ─────────────────────────────────────────────────────────────────────────────

static void renameInExpr(Expression* expr,
                         const std::unordered_map<std::string, std::string>& rmap);
static void renameInStmt(Statement* stmt,
                         const std::unordered_map<std::string, std::string>& rmap);

static void renameInExpr(Expression* expr,
                         const std::unordered_map<std::string, std::string>& rmap) {
    if (!expr) return;
    auto rename = [&](std::string& n) {
        auto it = rmap.find(n);
        if (it != rmap.end()) n = it->second;
    };
    switch (expr->type) {
        case ASTNodeType::IDENTIFIER_EXPR:
            rename(static_cast<IdentifierExpr*>(expr)->name);
            break;
        case ASTNodeType::BINARY_EXPR: {
            auto* b = static_cast<BinaryExpr*>(expr);
            renameInExpr(b->left.get(), rmap);
            renameInExpr(b->right.get(), rmap);
            break;
        }
        case ASTNodeType::UNARY_EXPR:
            renameInExpr(static_cast<UnaryExpr*>(expr)->operand.get(), rmap);
            break;
        case ASTNodeType::POSTFIX_EXPR:
            renameInExpr(static_cast<PostfixExpr*>(expr)->operand.get(), rmap);
            break;
        case ASTNodeType::PREFIX_EXPR:
            renameInExpr(static_cast<PrefixExpr*>(expr)->operand.get(), rmap);
            break;
        case ASTNodeType::CALL_EXPR: {
            auto* c = static_cast<CallExpr*>(expr);
            for (auto& arg : c->arguments) renameInExpr(arg.get(), rmap);
            break;
        }
        case ASTNodeType::ASSIGN_EXPR: {
            auto* a = static_cast<AssignExpr*>(expr);
            rename(a->name);
            renameInExpr(a->value.get(), rmap);
            break;
        }
        case ASTNodeType::TERNARY_EXPR: {
            auto* t = static_cast<TernaryExpr*>(expr);
            renameInExpr(t->condition.get(), rmap);
            renameInExpr(t->thenExpr.get(), rmap);
            renameInExpr(t->elseExpr.get(), rmap);
            break;
        }
        case ASTNodeType::ARRAY_EXPR: {
            auto* a = static_cast<ArrayExpr*>(expr);
            for (auto& e : a->elements) renameInExpr(e.get(), rmap);
            break;
        }
        case ASTNodeType::INDEX_EXPR: {
            auto* ix = static_cast<IndexExpr*>(expr);
            renameInExpr(ix->array.get(), rmap);
            renameInExpr(ix->index.get(), rmap);
            break;
        }
        case ASTNodeType::INDEX_ASSIGN_EXPR: {
            auto* ia = static_cast<IndexAssignExpr*>(expr);
            renameInExpr(ia->array.get(), rmap);
            renameInExpr(ia->index.get(), rmap);
            renameInExpr(ia->value.get(), rmap);
            break;
        }
        case ASTNodeType::FIELD_ACCESS_EXPR:
            renameInExpr(
                static_cast<FieldAccessExpr*>(expr)->object.get(), rmap);
            break;
        case ASTNodeType::FIELD_ASSIGN_EXPR: {
            auto* fa = static_cast<FieldAssignExpr*>(expr);
            renameInExpr(fa->object.get(), rmap);
            renameInExpr(fa->value.get(), rmap);
            break;
        }
        case ASTNodeType::MOVE_EXPR:
            renameInExpr(static_cast<MoveExpr*>(expr)->source.get(), rmap);
            break;
        case ASTNodeType::BORROW_EXPR:
            renameInExpr(static_cast<BorrowExpr*>(expr)->source.get(), rmap);
            break;
        case ASTNodeType::REBORROW_EXPR:
            renameInExpr(
                static_cast<ReborrowExpr*>(expr)->source.get(), rmap);
            break;
        case ASTNodeType::SPREAD_EXPR:
            renameInExpr(
                static_cast<SpreadExpr*>(expr)->operand.get(), rmap);
            break;
        case ASTNodeType::PIPE_EXPR:
            renameInExpr(
                static_cast<PipeExpr*>(expr)->left.get(), rmap);
            break;
        case ASTNodeType::RANGE_ANNOT_EXPR:
            renameInExpr(
                static_cast<RangeAnnotExpr*>(expr)->inner.get(), rmap);
            break;
        case ASTNodeType::STRUCT_LITERAL_EXPR: {
            auto* sl = static_cast<StructLiteralExpr*>(expr);
            for (auto& fv : sl->fieldValues)
                renameInExpr(fv.second.get(), rmap);
            break;
        }
        case ASTNodeType::DICT_EXPR: {
            auto* d = static_cast<DictExpr*>(expr);
            for (auto& p : d->pairs) {
                renameInExpr(p.first.get(), rmap);
                renameInExpr(p.second.get(), rmap);
            }
            break;
        }
        default:
            break;
    }
}

static void renameInStmt(Statement* stmt,
                         const std::unordered_map<std::string, std::string>& rmap) {
    if (!stmt) return;
    auto rename = [&](std::string& n) {
        auto it = rmap.find(n);
        if (it != rmap.end()) n = it->second;
    };
    switch (stmt->type) {
        case ASTNodeType::EXPR_STMT:
            renameInExpr(
                static_cast<ExprStmt*>(stmt)->expression.get(), rmap);
            break;
        case ASTNodeType::VAR_DECL: {
            auto* vd = static_cast<VarDecl*>(stmt);
            renameInExpr(vd->initializer.get(), rmap);
            break;
        }
        case ASTNodeType::RETURN_STMT:
            renameInExpr(
                static_cast<ReturnStmt*>(stmt)->value.get(), rmap);
            break;
        case ASTNodeType::IF_STMT: {
            auto* is = static_cast<IfStmt*>(stmt);
            renameInExpr(is->condition.get(), rmap);
            renameInStmt(is->thenBranch.get(), rmap);
            renameInStmt(is->elseBranch.get(), rmap);
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            auto* ws = static_cast<WhileStmt*>(stmt);
            renameInExpr(ws->condition.get(), rmap);
            renameInStmt(ws->body.get(), rmap);
            break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            auto* dw = static_cast<DoWhileStmt*>(stmt);
            renameInStmt(dw->body.get(), rmap);
            renameInExpr(dw->condition.get(), rmap);
            break;
        }
        case ASTNodeType::FOR_STMT: {
            auto* fs = static_cast<ForStmt*>(stmt);
            renameInExpr(fs->start.get(), rmap);
            renameInExpr(fs->end.get(), rmap);
            renameInExpr(fs->step.get(), rmap);
            renameInStmt(fs->body.get(), rmap);
            break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            auto* fe = static_cast<ForEachStmt*>(stmt);
            renameInExpr(fe->collection.get(), rmap);
            renameInStmt(fe->body.get(), rmap);
            break;
        }
        case ASTNodeType::BLOCK: {
            auto* b = static_cast<BlockStmt*>(stmt);
            for (auto& s : b->statements) renameInStmt(s.get(), rmap);
            break;
        }
        case ASTNodeType::SWITCH_STMT: {
            auto* sw = static_cast<SwitchStmt*>(stmt);
            renameInExpr(sw->condition.get(), rmap);
            for (auto& cas : sw->cases) {
                renameInExpr(cas.value.get(), rmap);
                for (auto& v : cas.values) renameInExpr(v.get(), rmap);
                for (auto& s : cas.body) renameInStmt(s.get(), rmap);
            }
            break;
        }
        case ASTNodeType::THROW_STMT:
            renameInExpr(
                static_cast<ThrowStmt*>(stmt)->value.get(), rmap);
            break;
        case ASTNodeType::CATCH_STMT:
            renameInStmt(
                static_cast<CatchStmt*>(stmt)->body.get(), rmap);
            break;
        case ASTNodeType::DEFER_STMT:
            renameInStmt(
                static_cast<DeferStmt*>(stmt)->body.get(), rmap);
            break;
        case ASTNodeType::ASSUME_STMT: {
            auto* as = static_cast<AssumeStmt*>(stmt);
            renameInExpr(as->condition.get(), rmap);
            renameInStmt(as->deoptBody.get(), rmap);
            break;
        }
        case ASTNodeType::INVALIDATE_STMT:
            rename(static_cast<InvalidateStmt*>(stmt)->varName);
            break;
        case ASTNodeType::MOVE_DECL:
            renameInExpr(
                static_cast<MoveDecl*>(stmt)->initializer.get(), rmap);
            break;
        case ASTNodeType::FREEZE_STMT:
            rename(static_cast<FreezeStmt*>(stmt)->varName);
            break;
        case ASTNodeType::PREFETCH_STMT: {
            auto* ps = static_cast<PrefetchStmt*>(stmt);
            rename(ps->varName);
            if (ps->varDecl) renameInExpr(ps->varDecl->initializer.get(), rmap);
            break;
        }
        case ASTNodeType::PIPELINE_STMT: {
            auto* pl = static_cast<PipelineStmt*>(stmt);
            renameInExpr(pl->count.get(), rmap);
            for (auto& stage : pl->stages) renameInStmt(stage.body.get(), rmap);
            break;
        }
        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Region event record
// ─────────────────────────────────────────────────────────────────────────────

struct RegionInfo {
    std::string name;
    int createIdx     = -1; ///< Index in stmt list of `var name = newRegion()`
    int invalidateIdx = -1; ///< Index in stmt list of `invalidate name`
    bool isGlobal     = false; ///< true → exempt from E013
};

// ─────────────────────────────────────────────────────────────────────────────
// Per-function analysis + transformation
// ─────────────────────────────────────────────────────────────────────────────

/// Compute the canonical (root) name after all coalescing renames.
static constexpr int kMaxRenameDepth = 32;
static std::string canonical(
    const std::string& name,
    const std::unordered_map<std::string, std::string>& rmap)
{
    const std::string* cur = &name;
    for (int depth = 0; depth < kMaxRenameDepth; ++depth) {
        auto it = rmap.find(*cur);
        if (it == rmap.end()) return *cur;
        cur = &it->second;
    }
    return *cur; // cycle guard
}

/// Process one flat statement list (the top-level block of a function).
/// Modifies @p stmts in place and updates @p stats.
static void processBlock(
    std::vector<std::unique_ptr<Statement>>& stmts,
    const std::string& funcName,
    RLCStats& stats,
    bool verbose)
{
    const int n = static_cast<int>(stmts.size());

    // ── Phase 1: collect region creation and invalidate events ────────────────
    std::vector<RegionInfo> regions;
    std::unordered_map<std::string, int> regionIdx; // name → index in regions[]

    for (int i = 0; i < n; ++i) {
        const Statement* s = stmts[i].get();
        if (!s) continue;

        // var r = newRegion();
        if (s->type == ASTNodeType::VAR_DECL) {
            const auto* vd = static_cast<const VarDecl*>(s);
            if (isNewRegionCall(vd->initializer.get())) {
                RegionInfo ri;
                ri.name       = vd->name;
                ri.createIdx  = i;
                ri.isGlobal   = vd->isGlobal;
                regionIdx[vd->name] = static_cast<int>(regions.size());
                regions.push_back(std::move(ri));
            }
        }

        // invalidate r;
        if (s->type == ASTNodeType::INVALIDATE_STMT) {
            const auto* inv = static_cast<const InvalidateStmt*>(s);
            auto it = regionIdx.find(inv->varName);
            if (it != regionIdx.end() && regions[it->second].invalidateIdx < 0) {
                regions[it->second].invalidateIdx = i;
            }
        }
    }

    // ── Phase 2: diagnostics ──────────────────────────────────────────────────

    // E013: region not invalidated (and not returned, and not global)
    std::unordered_set<std::string> returnedVars;
    for (const auto& sp : stmts) {
        if (!sp || sp->type != ASTNodeType::RETURN_STMT) continue;
        const auto* ret = static_cast<const ReturnStmt*>(sp.get());
        if (ret->value && ret->value->type == ASTNodeType::IDENTIFIER_EXPR) {
            returnedVars.insert(
                static_cast<const IdentifierExpr*>(ret->value.get())->name);
        }
    }

    for (const auto& ri : regions) {
        if (ri.isGlobal) continue; // globals are exempt
        if (returnedVars.count(ri.name)) continue; // returned → exempt
        if (ri.invalidateIdx < 0) {
            throw DiagnosticError(Diagnostic{
                DiagnosticSeverity::Error, {"", 0, 0},
                "Region variable '" + ri.name + "' in function '" + funcName +
                "' is never invalidated. Every region variable created with "
                "newRegion() must be explicitly `invalidate`d before the "
                "function returns (or declared `global`).",
                ErrorCode::E013_REGION_NOT_INVALIDATED});
        }
    }

    // E014: use after invalidate
    for (const auto& ri : regions) {
        if (ri.invalidateIdx < 0) continue;
        for (int j = ri.invalidateIdx + 1; j < n; ++j) {
            if (!stmts[j]) continue;
            // Skip if j is the creation of a new region that shadows the same name.
            if (stmts[j]->type == ASTNodeType::VAR_DECL) {
                const auto* vd = static_cast<const VarDecl*>(stmts[j].get());
                if (vd->name == ri.name) break; // shadowed → stop scanning
            }
            if (stmtUsesVar(stmts[j].get(), ri.name)) {
                throw DiagnosticError(Diagnostic{
                    DiagnosticSeverity::Error, {"", 0, 0},
                    "Region variable '" + ri.name + "' in function '" + funcName +
                    "' is used after it was invalidated.",
                    ErrorCode::E014_REGION_USE_AFTER_INVALIDATE});
            }
        }
    }

    // ── Phase 3: greedy coalescing ────────────────────────────────────────────
    std::unordered_map<std::string, std::string> renameMap; // R2 → canonical(R1)
    std::unordered_set<int> removedStmts;

    // Track the effective invalidateIdx of the canonical representative.
    std::unordered_map<std::string, int> effectiveInvalidateIdx;
    for (const auto& ri : regions) {
        effectiveInvalidateIdx[ri.name] = ri.invalidateIdx;
    }

    for (int i = 0; i < static_cast<int>(regions.size()); ++i) {
        const RegionInfo& r2 = regions[i];
        if (r2.invalidateIdx < 0) continue;

        // Find the best available R1: most recently invalidated region whose
        // effective invalidateIdx < r2.createIdx.
        int bestJ = -1;
        int bestInvIdx = -1;
        for (int j = 0; j < i; ++j) {
            const RegionInfo& r1 = regions[j];
            if (r1.invalidateIdx < 0) continue;
            const std::string r1Canon = canonical(r1.name, renameMap);
            auto effIt = effectiveInvalidateIdx.find(r1Canon);
            if (effIt == effectiveInvalidateIdx.end()) continue;
            const int r1EffInv = effIt->second;
            if (r1EffInv < 0 || r1EffInv >= r2.createIdx) continue;
            if (r1EffInv > bestInvIdx) {
                bestInvIdx = r1EffInv;
                bestJ      = j;
            }
        }

        if (bestJ < 0) continue;

        const RegionInfo& r1 = regions[bestJ];
        const std::string r1Canon = canonical(r1.name, renameMap);
        const std::string r2Canon = canonical(r2.name, renameMap);
        if (r1Canon == r2Canon) continue;

        renameMap[r2.name] = r1Canon;
        removedStmts.insert(r2.createIdx);

        const int r1InvToRemove = effectiveInvalidateIdx[r1Canon];
        removedStmts.insert(r1InvToRemove);

        effectiveInvalidateIdx[r1Canon] = r2.invalidateIdx;

        ++stats.regionsCoalesced;

        if (verbose) {
            std::cout << "  [rlc] " << funcName << ": coalesced '"
                      << r2.name << "' into '" << r1Canon << "'\n";
        }
    }

    if (renameMap.empty()) return;

    // ── Phase 4: apply transformation ────────────────────────────────────────
    for (int i = 0; i < n; ++i) {
        if (removedStmts.count(i)) continue;
        const Statement* s = stmts[i].get();
        if (!s || s->type != ASTNodeType::EXPR_STMT) continue;
        const auto* es = static_cast<const ExprStmt*>(s);
        std::string rv;
        if (isAllocCall(es->expression.get(), &rv)) {
            if (renameMap.count(rv)) ++stats.allocsRedirected;
        }
    }
    stats.invalidatesRemoved += static_cast<unsigned>(removedStmts.size());

    std::vector<std::unique_ptr<Statement>> newStmts;
    newStmts.reserve(static_cast<size_t>(n) - removedStmts.size());
    for (int i = 0; i < n; ++i) {
        if (removedStmts.count(i)) continue;
        renameInStmt(stmts[i].get(), renameMap);
        newStmts.push_back(std::move(stmts[i]));
    }
    stmts = std::move(newStmts);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int kMaxCoalescingIterations = 8;

RLCStats runRLCPass(Program* program, bool verbose) {
    RLCStats stats;

    forEachFunction(program, [&](FunctionDecl* func) {
        // Run multiple iterations until fixpoint.
        for (int iter = 0; iter < kMaxCoalescingIterations; ++iter) {
            const unsigned before = stats.regionsCoalesced;
            processBlock(func->body->statements, func->name, stats, verbose);
            if (stats.regionsCoalesced == before) break;
        }
    });

    if (verbose && stats.regionsCoalesced > 0) {
        std::cout << "[rlc] Total: " << stats.regionsCoalesced
                  << " region pair(s) coalesced, "
                  << stats.allocsRedirected << " alloc(s) redirected, "
                  << stats.invalidatesRemoved << " invalidate(s) removed\n";
    }
    return stats;
}

} // namespace omscript
