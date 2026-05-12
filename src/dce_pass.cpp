/// @file dce_pass.cpp
/// @brief Dead Code Elimination (DCE) pass implementation.
///
/// Algorithm overview:
///   For each function, recursively walk its BlockStmt and apply:
///
///   Pass A — constant-condition if/while elimination (bottom-up recursive):
///     if (0)  { T } else { E }  → E          (or nothing if no else-branch)
///     if (!0) { T } else { E }  → T
///     while (0) { ... }          → (removed)
///     do { B } while (0)         → B          (executes exactly once,
///                                              ONLY when B has no top-level
///                                              break/continue — see below)
///
///   Pass B — unreachable-after-return pruning (per block):
///     Once a statement that unconditionally exits the block is encountered,
///     all following statements are dropped.  "Unconditionally exits" is
///     determined by stmtAlwaysExits() (see below).
///
/// ── do{B}while(0) safety constraint ────────────────────────────────────────
///   A break or continue directly inside B (not inside a nested loop or
///   switch) targets the do-while loop itself.  If we blindly replace
///   do{B}while(0) with just B, those jumps now escape to the *outer* loop,
///   changing the program's semantics and causing Pass B to over-prune the
///   statements that follow the do-while inside the outer loop body.
///   Fix: only apply the transformation when B has no top-level break/continue.

#include "dce_pass.h"
#include "pass_utils.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

static DCEStats transformBlock(BlockStmt* block);
static DCEStats transformStmt(std::unique_ptr<Statement>& stmt);

// ─────────────────────────────────────────────────────────────────────────────
// hasTopLevelBreakContinue — would a break/continue inside @p s escape to the
// enclosing do-while that we are about to eliminate?
// ─────────────────────────────────────────────────────────────────────────────
//
// Returns true if @p s (or a nested statement reachable without crossing
// another loop or switch boundary) is a break or continue statement.
//
// We stop recursing into nested FOR/WHILE/DO_WHILE/FOR_EACH and SWITCH because
// any break/continue inside those targets *them*, not the do-while being
// considered for elimination.  We do recurse into BLOCK and IF because
// those are transparent to break/continue flow.

static bool hasTopLevelBreakContinue(const Statement* s) {
    if (!s)
        return false;
    switch (s->type) {
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
        return true;
    case ASTNodeType::BLOCK: {
        const auto* blk = static_cast<const BlockStmt*>(s);
        for (const auto& st : blk->statements)
            if (hasTopLevelBreakContinue(st.get()))
                return true;
        return false;
    }
    case ASTNodeType::IF_STMT: {
        const auto* ifs = static_cast<const IfStmt*>(s);
        return hasTopLevelBreakContinue(ifs->thenBranch.get()) || hasTopLevelBreakContinue(ifs->elseBranch.get());
    }
    // Nested loops and switch absorb break/continue — do not recurse.
    case ASTNodeType::FOR_STMT:
    case ASTNodeType::WHILE_STMT:
    case ASTNodeType::DO_WHILE_STMT:
    case ASTNodeType::FOR_EACH_STMT:
    case ASTNodeType::SWITCH_STMT:
        return false;
    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// stmtAlwaysExits — does this statement unconditionally exit the current block?
// ─────────────────────────────────────────────────────────────────────────────
//
// Returns true when control can NEVER fall through past @p s:
//   • return / break / continue — direct exits.
//   • BlockStmt — exits iff its last non-null statement always exits.
//     (Pass A may produce a bare BlockStmt by folding `if(1){return x;}`
//     into `{return x;}`; Pass B must recognise such blocks as exits so the
//     dead code that follows is pruned.)
//   • IF_STMT with an else branch — exits iff BOTH the then-branch AND the
//     else-branch always exit.  Without an else branch the condition might
//     be false and fall through to the next statement.
//
// throw is deliberately excluded: it is handled specially in Pass B because a
// throw may be followed by catch() handlers that must NOT be removed.

static bool stmtAlwaysExits(const Statement* s) {
    if (!s)
        return false;
    switch (s->type) {
    case ASTNodeType::RETURN_STMT:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
        return true;
    case ASTNodeType::BLOCK: {
        const auto* blk = static_cast<const BlockStmt*>(s);
        if (blk->statements.empty())
            return false;
        // Walk backwards past any null slots to find the last real statement.
        // By the time Pass B runs, Pass A has already pruned inner-block dead
        // code, so the last statement is the last *reachable* statement.
        for (auto it = blk->statements.rbegin(); it != blk->statements.rend(); ++it) {
            if (*it)
                return stmtAlwaysExits(it->get());
        }
        return false;
    }
    case ASTNodeType::IF_STMT: {
        const auto* ifs = static_cast<const IfStmt*>(s);
        // Only an if WITH an else can guarantee both paths exit.
        // An if without else may fall through when the condition is false.
        return ifs->elseBranch && stmtAlwaysExits(ifs->thenBranch.get()) && stmtAlwaysExits(ifs->elseBranch.get());
    }
    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// transformStmt — recursively eliminate dead code in a single statement
// ─────────────────────────────────────────────────────────────────────────────
//
// May replace *stmt with a different node (or a nullptr-equivalent placeholder).
// Returns the number of transformations applied.

static DCEStats transformStmt(std::unique_ptr<Statement>& stmt) {
    DCEStats stats;
    if (!stmt)
        return stats;

    switch (stmt->type) {

    // ── If statement ────────────────────────────────────────────────────────
    case ASTNodeType::IF_STMT: {
        auto* ifStmt = static_cast<IfStmt*>(stmt.get());

        // Recursively clean up both branches first (bottom-up).
        if (ifStmt->thenBranch) {
            auto sub = transformStmt(ifStmt->thenBranch);
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }
        if (ifStmt->elseBranch) {
            auto sub = transformStmt(ifStmt->elseBranch);
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }

        // Now check if the condition is a constant integer.
        long long condVal = 0;
        if (isIntLiteral(ifStmt->condition.get(), &condVal)) {
            ++stats.deadIfBranches;
            if (condVal != 0) {
                // Condition is always-true: keep the then-branch.
                stmt = std::move(ifStmt->thenBranch);
            } else {
                // Condition is always-false: keep the else-branch (may be null).
                if (ifStmt->elseBranch) {
                    stmt = std::move(ifStmt->elseBranch);
                } else {
                    // No else-branch → replace with empty block.
                    stmt = std::make_unique<BlockStmt>(std::vector<std::unique_ptr<Statement>>{});
                }
            }
        }
        break;
    }

    // ── While statement ─────────────────────────────────────────────────────
    case ASTNodeType::WHILE_STMT: {
        auto* whileStmt = static_cast<WhileStmt*>(stmt.get());

        // Recursively clean the body first.
        if (whileStmt->body) {
            auto sub = transformStmt(whileStmt->body);
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }

        long long condVal = 0;
        if (isIntLiteral(whileStmt->condition.get(), &condVal) && condVal == 0) {
            // while (0) — body is unreachable; remove the loop entirely.
            ++stats.deadLoops;
            stmt = std::make_unique<BlockStmt>(std::vector<std::unique_ptr<Statement>>{});
        }
        break;
    }

    // ── Do-while statement ──────────────────────────────────────────────────
    case ASTNodeType::DO_WHILE_STMT: {
        auto* doWhile = static_cast<DoWhileStmt*>(stmt.get());

        // Recursively clean the body first.
        if (doWhile->body) {
            auto sub = transformStmt(doWhile->body);
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }

        long long condVal = 0;
        if (isIntLiteral(doWhile->condition.get(), &condVal) && condVal == 0) {
            // do { B } while (0) — body executes exactly once.
            // SAFETY: only replace with the body when the body has no
            // top-level break or continue.  A break/continue inside B
            // targets the do-while; if we lift B to the enclosing scope those
            // jumps would escape to the *outer* loop, changing semantics and
            // causing Pass B to over-prune the statements that follow this
            // do-while (the "trimmed too much" bug).
            if (!hasTopLevelBreakContinue(doWhile->body.get())) {
                ++stats.deadLoops;
                stmt = std::move(doWhile->body);
            }
        }
        break;
    }

    // ── Block statement ─────────────────────────────────────────────────────
    case ASTNodeType::BLOCK: {
        auto* block = static_cast<BlockStmt*>(stmt.get());
        auto sub = transformBlock(block);
        stats.deadIfBranches += sub.deadIfBranches;
        stats.deadLoops += sub.deadLoops;
        stats.unreachableStmts += sub.unreachableStmts;
        break;
    }

    // ── For statement (range loop) ──────────────────────────────────────────
    case ASTNodeType::FOR_STMT: {
        auto* forStmt = static_cast<ForStmt*>(stmt.get());
        // If both bounds are compile-time integer literals and start >= end,
        // the loop body is unreachable — replace the entire for statement with
        // an empty block.
        long long startVal = 0, endVal = 0;
        if (isIntLiteral(forStmt->start.get(), &startVal) && isIntLiteral(forStmt->end.get(), &endVal) &&
            startVal >= endVal) {
            ++stats.deadLoops;
            stmt = std::make_unique<BlockStmt>(std::vector<std::unique_ptr<Statement>>{});
            break;
        }
        if (forStmt->body) {
            auto sub = transformStmt(forStmt->body);
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }
        break;
    }

    // ── Variable declaration ────────────────────────────────────────────────
    case ASTNodeType::VAR_DECL:
        // No dead-code structure inside a simple declaration.
        break;

    // ── ForEach statement ───────────────────────────────────────────────────
    case ASTNodeType::FOR_EACH_STMT: {
        auto* feStmt = static_cast<ForEachStmt*>(stmt.get());
        if (feStmt->body) {
            auto sub = transformStmt(feStmt->body);
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }
        break;
    }

    // ── Switch statement ────────────────────────────────────────────────────
    case ASTNodeType::SWITCH_STMT: {
        auto* swStmt = static_cast<SwitchStmt*>(stmt.get());
        for (auto& sc : swStmt->cases) {
            // Each case body is a vector of statements: wrap in a temporary
            // block so transformBlock can apply both Pass 1 and Pass 2 to it.
            // We then unwrap the (possibly pruned) block back into the case.
            auto tmpBlock = std::make_unique<BlockStmt>(std::move(sc.body));
            auto sub = transformBlock(tmpBlock.get());
            sc.body = std::move(tmpBlock->statements);
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }
        break;
    }

    // ── Catch handler ───────────────────────────────────────────────────────
    case ASTNodeType::CATCH_STMT: {
        auto* catchStmt = static_cast<CatchStmt*>(stmt.get());
        if (catchStmt->body) {
            auto sub = transformBlock(catchStmt->body.get());
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }
        break;
    }

    // ── Defer statement ─────────────────────────────────────────────────────
    case ASTNodeType::DEFER_STMT: {
        auto* deferStmt = static_cast<DeferStmt*>(stmt.get());
        if (deferStmt->body) {
            auto sub = transformStmt(deferStmt->body);
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }
        break;
    }

    // ── Assume statement ─────────────────────────────────────────────────────
    case ASTNodeType::ASSUME_STMT: {
        auto* asStmt = static_cast<AssumeStmt*>(stmt.get());
        if (asStmt->deoptBody) {
            auto sub = transformStmt(asStmt->deoptBody);
            stats.deadIfBranches += sub.deadIfBranches;
            stats.deadLoops += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }
        break;
    }

    // ── Prefetch statement ───────────────────────────────────────────────────
    // Nothing structurally to simplify; the embedded VarDecl is not a
    // compound statement so DCE cannot prune it independently.
    case ASTNodeType::PREFETCH_STMT:
        break;

    // ── Pipeline statement ───────────────────────────────────────────────────
    case ASTNodeType::PIPELINE_STMT: {
        auto* plStmt = static_cast<PipelineStmt*>(stmt.get());
        for (auto& stage : plStmt->stages) {
            if (stage.body) {
                auto sub = transformBlock(stage.body.get());
                stats.deadIfBranches += sub.deadIfBranches;
                stats.deadLoops += sub.deadLoops;
                stats.unreachableStmts += sub.unreachableStmts;
            }
        }
        break;
    }

    // ── Return / expression statements / other leaf nodes ───────────────────
    default:
        // Nothing to simplify at this level.
        break;
    }

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// transformBlock — process all statements in a block
// ─────────────────────────────────────────────────────────────────────────────

static DCEStats transformBlock(BlockStmt* block) {
    DCEStats stats;
    if (!block)
        return stats;

    // Pass 1 — recursively simplify each statement.
    for (auto& s : block->statements) {
        auto sub = transformStmt(s);
        stats.deadIfBranches += sub.deadIfBranches;
        stats.deadLoops += sub.deadLoops;
        stats.unreachableStmts += sub.unreachableStmts;
    }

    // Pass 2 — prune unreachable statements after a definite exit.
    // Find the first unconditional exit in the block.
    // NOTE: throw is NOT a pruning boundary here because catch(code) blocks
    // that follow a throw are intentional jump targets — removing them would
    // delete live handlers and cause buildCatchTable to find an empty table.
    size_t cutoff = block->statements.size();
    for (size_t i = 0; i < block->statements.size(); ++i) {
        const Statement* s = block->statements[i].get();
        if (!s)
            continue;
        if (s->type == ASTNodeType::THROW_STMT) {
            // A throw is only a pruning boundary if NO catch block follows it
            // anywhere in the same block — i.e. it is truly unhandled.
            bool hasCatchAfter = false;
            for (size_t j = i + 1; j < block->statements.size(); ++j) {
                if (block->statements[j] && block->statements[j]->type == ASTNodeType::CATCH_STMT) {
                    hasCatchAfter = true;
                    break;
                }
            }
            if (!hasCatchAfter) {
                cutoff = i + 1;
                break;
            }
            // Has catch handlers — do not prune; let codegen handle the jump table.
            continue;
        }
        // For all other statement types, use stmtAlwaysExits() which covers
        // return / break / continue directly, and also BlockStmts whose last
        // statement always exits (e.g. `{return x;}` produced by Pass A when
        // folding `if(1){return x;}`).  Without this, dead code that follows
        // such a block is not pruned and main appears to have empty/useless body.
        if (stmtAlwaysExits(s)) {
            cutoff = i + 1;
            break;
        }
    }
    const size_t pruned = block->statements.size() - cutoff;
    if (pruned > 0) {
        stats.unreachableStmts += static_cast<unsigned>(pruned);
        block->statements.resize(cutoff);
    }

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

DCEStats runDCEPass(Program* program, bool verbose) {
    DCEStats total;

    forEachFunction(program, [&](FunctionDecl* fn) {
        DCEStats fnStats = transformBlock(fn->body.get());

        total.deadIfBranches += fnStats.deadIfBranches;
        total.deadLoops += fnStats.deadLoops;
        total.unreachableStmts += fnStats.unreachableStmts;

        if (verbose && (fnStats.deadIfBranches || fnStats.deadLoops || fnStats.unreachableStmts)) {
            std::cerr << "[DCE] " << fn->name << ": " << fnStats.deadIfBranches << " dead if-branch(es), "
                      << fnStats.deadLoops << " dead loop(s), " << fnStats.unreachableStmts
                      << " unreachable stmt(s) removed\n";
        }
    });

    return total;
}

} // namespace omscript
