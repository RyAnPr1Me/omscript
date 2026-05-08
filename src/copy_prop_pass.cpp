/// @file copy_prop_pass.cpp
/// @brief Copy Propagation (CopyProp) pass implementation.
///
/// See copy_prop_pass.h for a full algorithmic description.
///
/// Implementation notes
/// ====================
/// The pass is implemented as two mutually-recursive walkers:
///
///   propagateInExpr  — given a copy map, substitute every IdentifierExpr
///                      whose name is a key in the map with a fresh clone
///                      of the mapped source.
///   propagateInBlock — walk a statement list, maintaining the copy map as
///                      definitions and kills are encountered.
///
/// We never propagate across control-flow boundaries (if/while/for bodies
/// are visited with a *fresh* copy map derived by killing everything that
/// could have been written inside the branch).  This is conservative but
/// sound: we rely on subsequent passes (CFCTRE, AlgSimp, e-graph) to do
/// more aggressive cross-branch optimisation.
///
/// Transitive propagation is achieved by eagerly composing the copy map:
/// when we see `var y = x` and `x → w` is already in the map, we record
/// `y → w` directly (rather than `y → x`).  This means a single forward
/// pass is sufficient for chains of copies.

#include "copy_prop_pass.h"
#include "pass_utils.h"

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// CopyMap — mapping from alias name → canonical source name
// ─────────────────────────────────────────────────────────────────────────────

using CopyMap = std::unordered_map<std::string, std::string>;

/// Resolve `name` through the copy map.
///
/// Cycles cannot arise in practice: `killName()` removes any entry `b → a`
/// before we insert `a → b`, so the map is always a forest.  The single-level
/// lookup is therefore sufficient and cannot loop.
static const std::string& resolve(const CopyMap& map,
                                  const std::string& name) noexcept {
    const auto it = map.find(name);
    return (it != map.end()) ? it->second : name;
}

/// Kill all copies whose destination or source equals @p name.
static void killName(CopyMap& map, const std::string& name) {
    // Remove direct entry.
    map.erase(name);
    // Remove any entry whose source is `name` (it would now observe the
    // newly written value, not the old copy).
    for (auto it = map.begin(); it != map.end(); ) {
        if (it->second == name)
            it = map.erase(it);
        else
            ++it;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

using OpaqueSet = std::unordered_set<std::string>;

static unsigned propagateInExpr(std::unique_ptr<Expression>& expr,
                                 const CopyMap& map,
                                 const OpaqueSet& opaque);
static unsigned propagateInBlock(BlockStmt* block, CopyMap map,
                                  const OpaqueSet& opaque);

// ─────────────────────────────────────────────────────────────────────────────
// propagateInExpr — substitute identifier uses according to the copy map
// ─────────────────────────────────────────────────────────────────────────────

static unsigned propagateInExpr(std::unique_ptr<Expression>& expr,
                                 const CopyMap& map,
                                 const OpaqueSet& opaque) {
    if (!expr) return 0;
    unsigned count = 0;

    switch (expr->type) {
    case ASTNodeType::IDENTIFIER_EXPR: {
        auto* id = static_cast<const IdentifierExpr*>(expr.get());
        // Never substitute references to volatile/atomic variables — their
        // values can change between reads and must be re-loaded every time.
        if (opaque.count(id->name)) break;
        auto it = map.find(id->name);
        if (it != map.end()) {
            // Replace this identifier with a fresh clone of the source.
            expr = makeIdentifier(it->second);
            ++count;
        }
        break;
    }
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<BinaryExpr*>(expr.get());
        count += propagateInExpr(bin->left,  map, opaque);
        count += propagateInExpr(bin->right, map, opaque);
        break;
    }
    case ASTNodeType::UNARY_EXPR:
        count += propagateInExpr(static_cast<UnaryExpr*>(expr.get())->operand, map, opaque);
        break;
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<TernaryExpr*>(expr.get());
        count += propagateInExpr(tern->condition, map, opaque);
        count += propagateInExpr(tern->thenExpr,  map, opaque);
        count += propagateInExpr(tern->elseExpr,  map, opaque);
        break;
    }
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<CallExpr*>(expr.get());
        for (auto& arg : call->arguments)
            count += propagateInExpr(arg, map, opaque);
        break;
    }
    case ASTNodeType::ASSIGN_EXPR: {
        // y = <rhs>: propagate into RHS, then kill y.
        // Note: we only propagate into the RHS here; the LHS kill is handled
        // by propagateInBlock when it sees this as an ExprStmt.
        auto* asgn = static_cast<AssignExpr*>(expr.get());
        count += propagateInExpr(asgn->value, map, opaque);
        break;
    }
    case ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<IndexExpr*>(expr.get());
        count += propagateInExpr(idx->array, map, opaque);
        count += propagateInExpr(idx->index, map, opaque);
        break;
    }
    case ASTNodeType::INDEX_ASSIGN_EXPR: {
        auto* ia = static_cast<IndexAssignExpr*>(expr.get());
        count += propagateInExpr(ia->array, map, opaque);
        count += propagateInExpr(ia->index, map, opaque);
        count += propagateInExpr(ia->value, map, opaque);
        break;
    }
    case ASTNodeType::FIELD_ACCESS_EXPR:
        count += propagateInExpr(static_cast<FieldAccessExpr*>(expr.get())->object, map, opaque);
        break;
    case ASTNodeType::FIELD_ASSIGN_EXPR: {
        auto* fa = static_cast<FieldAssignExpr*>(expr.get());
        count += propagateInExpr(fa->object, map, opaque);
        count += propagateInExpr(fa->value,  map, opaque);
        break;
    }
    case ASTNodeType::POSTFIX_EXPR:
        count += propagateInExpr(static_cast<PostfixExpr*>(expr.get())->operand, map, opaque);
        break;
    case ASTNodeType::PREFIX_EXPR:
        count += propagateInExpr(static_cast<PrefixExpr*>(expr.get())->operand, map, opaque);
        break;
    case ASTNodeType::SPREAD_EXPR:
        count += propagateInExpr(static_cast<SpreadExpr*>(expr.get())->operand, map, opaque);
        break;
    case ASTNodeType::PIPE_EXPR:
        count += propagateInExpr(static_cast<PipeExpr*>(expr.get())->left, map, opaque);
        break;
    case ASTNodeType::MOVE_EXPR:
        count += propagateInExpr(static_cast<MoveExpr*>(expr.get())->source, map, opaque);
        break;
    case ASTNodeType::BORROW_EXPR:
        count += propagateInExpr(static_cast<BorrowExpr*>(expr.get())->source, map, opaque);
        break;
    case ASTNodeType::REBORROW_EXPR: {
        auto* rb = static_cast<ReborrowExpr*>(expr.get());
        count += propagateInExpr(rb->source, map, opaque);
        if (rb->indexExpr) count += propagateInExpr(rb->indexExpr, map, opaque);
        break;
    }
    case ASTNodeType::RANGE_ANNOT_EXPR:
        count += propagateInExpr(static_cast<RangeAnnotExpr*>(expr.get())->inner, map, opaque);
        break;
    case ASTNodeType::STRUCT_LITERAL_EXPR: {
        auto* sl = static_cast<StructLiteralExpr*>(expr.get());
        for (auto& fv : sl->fieldValues)
            count += propagateInExpr(fv.second, map, opaque);
        break;
    }
    case ASTNodeType::DICT_EXPR: {
        auto* de = static_cast<DictExpr*>(expr.get());
        for (auto& p : de->pairs) {
            count += propagateInExpr(p.first,  map, opaque);
            count += propagateInExpr(p.second, map, opaque);
        }
        break;
    }
    case ASTNodeType::ARRAY_EXPR: {
        auto* ae = static_cast<ArrayExpr*>(expr.get());
        for (auto& el : ae->elements)
            count += propagateInExpr(el, map, opaque);
        break;
    }
    default:
        break;
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: find all names that a statement may write to
// ─────────────────────────────────────────────────────────────────────────────

static void collectWrittenNames(const Expression* expr,
                                 std::unordered_set<std::string>& out) {
    if (!expr) return;
    switch (expr->type) {
    case ASTNodeType::ASSIGN_EXPR:
        out.insert(static_cast<const AssignExpr*>(expr)->name);
        collectWrittenNames(static_cast<const AssignExpr*>(expr)->value.get(), out);
        break;
    case ASTNodeType::INDEX_ASSIGN_EXPR:
        // Array element write; the array variable itself might be considered written.
        collectWrittenNames(static_cast<const IndexAssignExpr*>(expr)->array.get(), out);
        break;
    case ASTNodeType::POSTFIX_EXPR:
    case ASTNodeType::PREFIX_EXPR: {
        // x++ / x-- / ++x / --x all write to x.
        const Expression* operand = (expr->type == ASTNodeType::POSTFIX_EXPR)
            ? static_cast<const PostfixExpr*>(expr)->operand.get()
            : static_cast<const PrefixExpr*>(expr)->operand.get();
        if (const auto* operandId = asIdentifier(operand))
            out.insert(operandId->name);
        break;
    }
    case ASTNodeType::BINARY_EXPR: {
        const auto* bin = static_cast<const BinaryExpr*>(expr);
        collectWrittenNames(bin->left.get(),  out);
        collectWrittenNames(bin->right.get(), out);
        break;
    }
    default:
        break;
    }
}

static void collectWrittenInStmt(const Statement* stmt,
                                  std::unordered_set<std::string>& out) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::VAR_DECL:
        out.insert(static_cast<const VarDecl*>(stmt)->name);
        break;
    case ASTNodeType::MOVE_DECL:
        out.insert(static_cast<const MoveDecl*>(stmt)->name);
        break;
    case ASTNodeType::PREFETCH_STMT: {
        const auto* ps = static_cast<const PrefetchStmt*>(stmt);
        if (ps->varDecl) out.insert(ps->varDecl->name);
        break;
    }
    case ASTNodeType::EXPR_STMT:
        collectWrittenNames(static_cast<const ExprStmt*>(stmt)->expression.get(), out);
        break;
    case ASTNodeType::FOR_STMT: {
        const auto* fs = static_cast<const ForStmt*>(stmt);
        out.insert(fs->iteratorVar); // loop variable
        break;
    }
    default:
        break;
    }
}

/// Recursively collect all names written anywhere inside @p stmt (including
/// nested blocks, if/while/for bodies).  Used to conservatively kill the
/// copy map before propagating into a loop condition.
static void collectWrittenDeep(const Statement* stmt,
                                std::unordered_set<std::string>& out) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::VAR_DECL:
        out.insert(static_cast<const VarDecl*>(stmt)->name);
        break;
    case ASTNodeType::MOVE_DECL:
        out.insert(static_cast<const MoveDecl*>(stmt)->name);
        break;
    case ASTNodeType::PREFETCH_STMT: {
        const auto* ps = static_cast<const PrefetchStmt*>(stmt);
        if (ps->varDecl) out.insert(ps->varDecl->name);
        break;
    }
    case ASTNodeType::EXPR_STMT:
        collectWrittenNames(static_cast<const ExprStmt*>(stmt)->expression.get(), out);
        break;
    case ASTNodeType::FOR_STMT: {
        const auto* fs = static_cast<const ForStmt*>(stmt);
        out.insert(fs->iteratorVar);
        collectWrittenDeep(fs->body.get(), out);
        break;
    }
    case ASTNodeType::FOR_EACH_STMT: {
        const auto* fe = static_cast<const ForEachStmt*>(stmt);
        out.insert(fe->iteratorVar);
        collectWrittenDeep(fe->body.get(), out);
        break;
    }
    case ASTNodeType::WHILE_STMT:
        collectWrittenDeep(static_cast<const WhileStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::DO_WHILE_STMT:
        collectWrittenDeep(static_cast<const DoWhileStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::IF_STMT: {
        const auto* ifS = static_cast<const IfStmt*>(stmt);
        collectWrittenDeep(ifS->thenBranch.get(), out);
        collectWrittenDeep(ifS->elseBranch.get(), out);
        break;
    }
    case ASTNodeType::BLOCK: {
        const auto* blk = static_cast<const BlockStmt*>(stmt);
        for (const auto& s : blk->statements)
            collectWrittenDeep(s.get(), out);
        break;
    }
    case ASTNodeType::SWITCH_STMT: {
        const auto* sw = static_cast<const SwitchStmt*>(stmt);
        for (const auto& sc : sw->cases)
            for (const auto& s : sc.body)
                collectWrittenDeep(s.get(), out);
        break;
    }
    case ASTNodeType::CATCH_STMT:
        collectWrittenDeep(static_cast<const CatchStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::DEFER_STMT:
        collectWrittenDeep(static_cast<const DeferStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::PIPELINE_STMT: {
        const auto* pl = static_cast<const PipelineStmt*>(stmt);
        for (const auto& stage : pl->stages)
            collectWrittenDeep(stage.body.get(), out);
        break;
    }
    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// propagateInBlock — forward dataflow over one statement list
// ─────────────────────────────────────────────────────────────────────────────

/// Kill all names that @p body may write, then recurse into it.
/// Used by while/do-while/for/foreach handlers so the identical
/// "collect-writes, kill, recurse" pattern is not duplicated across
/// every loop statement kind.
static unsigned killAndRecurseBody(Statement* body, CopyMap& map,
                                    const OpaqueSet& opaque) {
    if (!body) return 0;
    std::unordered_set<std::string> writes;
    collectWrittenDeep(body, writes);
    for (const auto& w : writes) killName(map, w);
    if (body->type == ASTNodeType::BLOCK)
        return propagateInBlock(static_cast<BlockStmt*>(body), map, opaque);
    return 0;
}

static unsigned propagateInBlock(BlockStmt* block, CopyMap map,
                                  const OpaqueSet& opaque) {
    if (!block) return 0;
    unsigned count = 0;

    for (auto& stmt : block->statements) {
        if (!stmt) continue;

        switch (stmt->type) {

        case ASTNodeType::VAR_DECL: {
            auto* vd = static_cast<VarDecl*>(stmt.get());
            // Propagate into the initializer FIRST (it sees the current map).
            count += propagateInExpr(vd->initializer, map, opaque);
            // Never record a copy for volatile or atomic variables — their
            // values can change externally and must not be forwarded.
            if (vd->isVolatile || vd->isAtomic) {
                killName(map, vd->name);
                break;
            }
            // Then decide whether this creates a new copy or kills an existing one.
            if (const auto* initId = asIdentifier(vd->initializer.get())) {
                // Don't forward a copy from a volatile/atomic source.
                if (opaque.count(initId->name)) {
                    killName(map, vd->name);
                    break;
                }
                // var y = x  → record y → resolve(x) for transitive propagation.
                const auto& src = resolve(map, initId->name);
                // Kill any prior entry for y, then record the new copy.
                killName(map, vd->name);
                map[vd->name] = src;
            } else {
                // var y = <expr> — kill any copy whose destination or source is y.
                killName(map, vd->name);
            }
            break;
        }

        case ASTNodeType::MOVE_DECL: {
            auto* md = static_cast<MoveDecl*>(stmt.get());
            // Propagate into the initializer FIRST.
            count += propagateInExpr(md->initializer, map, opaque);
            // A move consumes the source variable — kill any copy entry that
            // references the source name (if the initializer is a simple ident).
            if (const auto* initId = asIdentifier(md->initializer.get()))
                killName(map, initId->name);
            // Always kill the destination: move semantics means the new name
            // holds a unique copy and cannot alias a prior expression.
            killName(map, md->name);
            break;
        }

        case ASTNodeType::PREFETCH_STMT: {
            auto* ps = static_cast<PrefetchStmt*>(stmt.get());
            if (ps->varDecl) {
                // prefetch [hot] var x = expr  — treats the embedded VarDecl
                // exactly like a regular VAR_DECL for copy-propagation purposes.
                count += propagateInExpr(ps->varDecl->initializer, map, opaque);
                killName(map, ps->varDecl->name);
            } else if (ps->addrExpr) {
                count += propagateInExpr(ps->addrExpr, map, opaque);
            }
            break;
        }

        case ASTNodeType::EXPR_STMT: {
            auto* es = static_cast<ExprStmt*>(stmt.get());
            // Collect writes *before* propagating (conservative ordering):
            // the kill happens after expression evaluation.
            std::unordered_set<std::string> written;
            collectWrittenNames(es->expression.get(), written);
            count += propagateInExpr(es->expression, map, opaque);
            for (const auto& w : written) killName(map, w);
            break;
        }

        case ASTNodeType::RETURN_STMT:
            count += propagateInExpr(
                static_cast<ReturnStmt*>(stmt.get())->value, map, opaque);
            break;

        case ASTNodeType::IF_STMT: {
            auto* ifS = static_cast<IfStmt*>(stmt.get());
            count += propagateInExpr(ifS->condition, map, opaque);
            // Compute a conservative set of names written in either branch,
            // then invalidate those copies so the post-if map is safe.
            // Use collectWrittenDeep so writes inside nested blocks inside the
            // branch body are also killed (collectWrittenInStmt is shallow and
            // misses writes inside BlockStmt children).
            std::unordered_set<std::string> branchWrites;
            if (ifS->thenBranch)
                collectWrittenDeep(ifS->thenBranch.get(), branchWrites);
            if (ifS->elseBranch)
                collectWrittenDeep(ifS->elseBranch.get(), branchWrites);
            // Recurse into branches with a *copy* of the current map.
            if (ifS->thenBranch && ifS->thenBranch->type == ASTNodeType::BLOCK)
                count += propagateInBlock(
                    static_cast<BlockStmt*>(ifS->thenBranch.get()), map, opaque);
            if (ifS->elseBranch && ifS->elseBranch->type == ASTNodeType::BLOCK)
                count += propagateInBlock(
                    static_cast<BlockStmt*>(ifS->elseBranch.get()), map, opaque);
            // Kill everything that either branch might have written.
            for (const auto& w : branchWrites) killName(map, w);
            break;
        }

        case ASTNodeType::WHILE_STMT: {
            auto* ws = static_cast<WhileStmt*>(stmt.get());
            // Kill all names written anywhere in the body BEFORE propagating
            // into the condition — otherwise a copy `v → x` would replace `v`
            // in `while ((v & 1) == 0)` even though `v` is mutated inside the
            // body, causing LLVM to fold the condition as loop-invariant and
            // emit `noreturn`.
            {
                std::unordered_set<std::string> bodyWrites;
                collectWrittenDeep(ws->body.get(), bodyWrites);
                for (const auto& w : bodyWrites) killName(map, w);
            }
            count += propagateInExpr(ws->condition, map, opaque);
            count += killAndRecurseBody(ws->body.get(), map, opaque);
            break;
        }

        case ASTNodeType::DO_WHILE_STMT: {
            auto* dw = static_cast<DoWhileStmt*>(stmt.get());
            count += killAndRecurseBody(dw->body.get(), map, opaque);
            count += propagateInExpr(dw->condition, map, opaque);
            break;
        }

        case ASTNodeType::FOR_STMT: {
            auto* fs = static_cast<ForStmt*>(stmt.get());
            count += propagateInExpr(fs->start, map, opaque);
            count += propagateInExpr(fs->end,   map, opaque);
            count += propagateInExpr(fs->step,  map, opaque);
            // Kill the loop variable before recursing into the body.
            killName(map, fs->iteratorVar);
            count += killAndRecurseBody(fs->body.get(), map, opaque);
            break;
        }

        case ASTNodeType::FOR_EACH_STMT: {
            auto* fe = static_cast<ForEachStmt*>(stmt.get());
            // Propagate into the collection expression (e.g. `for x in arr` →
            // substitute arr if it is a copy alias).
            count += propagateInExpr(fe->collection, map, opaque);
            // The iterator variable is bound fresh each iteration; kill any
            // prior copy entry for it before recursing into the body.
            killName(map, fe->iteratorVar);
            count += killAndRecurseBody(fe->body.get(), map, opaque);
            break;
        }

        case ASTNodeType::BLOCK:
            // Nested bare block: recurse with the current map (same scope).
            count += propagateInBlock(
                static_cast<BlockStmt*>(stmt.get()), map, opaque);
            break;

        case ASTNodeType::INVALIDATE_STMT:
            // `invalidate v` kills any copy whose source or destination is v,
            // preventing propagation of a stale alias across the invalidation.
            killName(map, static_cast<InvalidateStmt*>(stmt.get())->varName);
            break;

        case ASTNodeType::ASSUME_STMT: {
            auto* as = static_cast<AssumeStmt*>(stmt.get());
            count += propagateInExpr(as->condition, map, opaque);
            if (as->deoptBody)
                count += killAndRecurseBody(as->deoptBody.get(), map, opaque);
            break;
        }

        case ASTNodeType::THROW_STMT:
            count += propagateInExpr(
                static_cast<ThrowStmt*>(stmt.get())->value, map, opaque);
            break;

        case ASTNodeType::SWITCH_STMT: {
            auto* sw = static_cast<SwitchStmt*>(stmt.get());
            count += propagateInExpr(sw->condition, map, opaque);
            // Collect every name written across all cases, kill them before
            // recursing so the post-switch map is conservatively safe.
            std::unordered_set<std::string> switchWrites;
            for (auto& sc : sw->cases)
                for (auto& s : sc.body)
                    collectWrittenDeep(s.get(), switchWrites);
            for (const auto& w : switchWrites) killName(map, w);
            // Propagate into each case body with a snapshot of the current
            // (already-killed) map for intra-case opportunities.
            for (auto& sc : sw->cases) {
                if (sc.value) count += propagateInExpr(sc.value, map, opaque);
                for (auto& val : sc.values) count += propagateInExpr(val, map, opaque);
                for (auto& s : sc.body) {
                    if (s && s->type == ASTNodeType::BLOCK)
                        count += propagateInBlock(
                            static_cast<BlockStmt*>(s.get()), map, opaque);
                }
            }
            break;
        }

        case ASTNodeType::CATCH_STMT: {
            auto* cs = static_cast<CatchStmt*>(stmt.get());
            // Catch body runs only on the exceptional path; kill writes
            // conservatively and propagate inside the body.
            count += killAndRecurseBody(cs->body.get(), map, opaque);
            break;
        }

        case ASTNodeType::DEFER_STMT: {
            auto* ds = static_cast<DeferStmt*>(stmt.get());
            // Defer body runs at scope exit; kill writes conservatively and
            // propagate inside the body.
            count += killAndRecurseBody(ds->body.get(), map, opaque);
            break;
        }

        case ASTNodeType::PIPELINE_STMT: {
            auto* pl = static_cast<PipelineStmt*>(stmt.get());
            // Propagate into the count expression first.
            if (pl->count) count += propagateInExpr(pl->count, map, opaque);
            // Kill all writes across all stages, then recurse into each.
            std::unordered_set<std::string> pipeWrites;
            for (const auto& stage : pl->stages)
                collectWrittenDeep(stage.body.get(), pipeWrites);
            for (const auto& w : pipeWrites) killName(map, w);
            for (auto& stage : pl->stages)
                count += killAndRecurseBody(stage.body.get(), map, opaque);
            break;
        }

        default:
            break;
        }
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// collectOpaqueVars — find all volatile/atomic variable names in a function
// ─────────────────────────────────────────────────────────────────────────────

static void collectOpaqueVarsInBlock(const BlockStmt* block, OpaqueSet& out);

static void collectOpaqueVarsInStmt(const Statement* stmt, OpaqueSet& out) {
    if (!stmt) return;
    switch (stmt->type) {
    case ASTNodeType::VAR_DECL: {
        const auto* vd = static_cast<const VarDecl*>(stmt);
        if (vd->isVolatile || vd->isAtomic)
            out.insert(vd->name);
        break;
    }
    case ASTNodeType::BLOCK:
        collectOpaqueVarsInBlock(static_cast<const BlockStmt*>(stmt), out);
        break;
    case ASTNodeType::IF_STMT: {
        const auto* ifS = static_cast<const IfStmt*>(stmt);
        collectOpaqueVarsInStmt(ifS->thenBranch.get(), out);
        collectOpaqueVarsInStmt(ifS->elseBranch.get(), out);
        break;
    }
    case ASTNodeType::WHILE_STMT:
        collectOpaqueVarsInStmt(static_cast<const WhileStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::DO_WHILE_STMT:
        collectOpaqueVarsInStmt(static_cast<const DoWhileStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::FOR_STMT:
        collectOpaqueVarsInStmt(static_cast<const ForStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::FOR_EACH_STMT:
        collectOpaqueVarsInStmt(static_cast<const ForEachStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::SWITCH_STMT: {
        const auto* sw = static_cast<const SwitchStmt*>(stmt);
        for (const auto& sc : sw->cases)
            for (const auto& s : sc.body)
                collectOpaqueVarsInStmt(s.get(), out);
        break;
    }
    case ASTNodeType::CATCH_STMT:
        collectOpaqueVarsInStmt(static_cast<const CatchStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::DEFER_STMT:
        collectOpaqueVarsInStmt(static_cast<const DeferStmt*>(stmt)->body.get(), out);
        break;
    case ASTNodeType::PIPELINE_STMT: {
        const auto* pl = static_cast<const PipelineStmt*>(stmt);
        for (const auto& stage : pl->stages)
            collectOpaqueVarsInStmt(stage.body.get(), out);
        break;
    }
    default:
        break;
    }
}

static void collectOpaqueVarsInBlock(const BlockStmt* block, OpaqueSet& out) {
    if (!block) return;
    for (const auto& s : block->statements)
        collectOpaqueVarsInStmt(s.get(), out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

CopyPropStats runCopyPropPass(Program* program, bool verbose) {
    CopyPropStats total;

    forEachFunction(program, [&](FunctionDecl* fn) {
        // Pre-scan the function for volatile/atomic variable names.
        // These are "opaque" — their loads cannot be forwarded.
        OpaqueSet opaque;
        collectOpaqueVarsInBlock(fn->body.get(), opaque);

        const unsigned before = total.copiesEliminated;
        total.copiesEliminated +=
            propagateInBlock(fn->body.get(), CopyMap{}, opaque);

        const unsigned applied = total.copiesEliminated - before;
        if (verbose && applied > 0) {
            std::cerr << "[CopyProp] " << fn->name
                      << ": " << applied << " copy(ies) propagated\n";
        }
    });

    return total;
}

} // namespace omscript
