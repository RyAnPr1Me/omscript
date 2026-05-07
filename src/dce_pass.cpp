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
///     do { B } while (0)         → B          (executes exactly once)
///
///   Pass B — unreachable-after-return pruning (per block):
///     Once a statement that unconditionally exits the block (return, break,
///     continue, throw, or a BlockStmt whose last statement always exits) is
///     encountered in the statement list, all following statements are dropped.
///
///     NOTE on BlockStmt exits: Pass A may replace `if(1){return x;}` with
///     a bare BlockStmt `{return x;}`.  Pass B must recognise such blocks as
///     unconditional exits so that subsequent statements — which are now dead
///     — are correctly pruned rather than left in the AST.

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
// stmtAlwaysExits — does this statement unconditionally exit the block?
// ─────────────────────────────────────────────────────────────────────────────
//
// Returns true when control can never fall through past @p s:
//   • return / break / continue — direct exits.
//   • BlockStmt — exits iff its last non-null statement always exits
//     (handles the case where Pass A replaced `if(1){return x;}` with the
//     bare BlockStmt `{return x;}`; without this, Pass B sees a BLOCK at
//     position i and leaves all subsequent statements alive, producing dead
//     code that makes main appear empty of meaningful work).
//
// throw is deliberately excluded — it is handled separately in Pass B because
// it may be followed by catch() handlers that must be preserved.

static bool stmtAlwaysExits(const Statement* s) {
    if (!s) return false;
    switch (s->type) {
    case ASTNodeType::RETURN_STMT:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
        return true;
    case ASTNodeType::BLOCK: {
        const auto* blk = static_cast<const BlockStmt*>(s);
        if (blk->statements.empty()) return false;
        // Walk backwards past any null slots to the last real statement.
        for (auto it = blk->statements.rbegin(); it != blk->statements.rend(); ++it) {
            if (*it) return stmtAlwaysExits(it->get());
        }
        return false;
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
    if (!stmt) return stats;

    switch (stmt->type) {

    // ── If statement ────────────────────────────────────────────────────────
    case ASTNodeType::IF_STMT: {
        auto* ifStmt = static_cast<IfStmt*>(stmt.get());

        // Recursively clean up both branches first (bottom-up).
        if (ifStmt->thenBranch) {
            auto sub = transformStmt(ifStmt->thenBranch);
            stats.deadIfBranches   += sub.deadIfBranches;
            stats.deadLoops        += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }
        if (ifStmt->elseBranch) {
            auto sub = transformStmt(ifStmt->elseBranch);
            stats.deadIfBranches   += sub.deadIfBranches;
            stats.deadLoops        += sub.deadLoops;
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
                    stmt = std::make_unique<BlockStmt>(
                        std::vector<std::unique_ptr<Statement>>{});
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
            stats.deadIfBranches   += sub.deadIfBranches;
            stats.deadLoops        += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }

        long long condVal = 0;
        if (isIntLiteral(whileStmt->condition.get(), &condVal) && condVal == 0) {
            // while (0) — body is unreachable; remove the loop entirely.
            ++stats.deadLoops;
            stmt = std::make_unique<BlockStmt>(
                std::vector<std::unique_ptr<Statement>>{});
        }
        break;
    }

    // ── Do-while statement ──────────────────────────────────────────────────
    case ASTNodeType::DO_WHILE_STMT: {
        auto* doWhile = static_cast<DoWhileStmt*>(stmt.get());

        // Recursively clean the body first.
        if (doWhile->body) {
            auto sub = transformStmt(doWhile->body);
            stats.deadIfBranches   += sub.deadIfBranches;
            stats.deadLoops        += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }

        long long condVal = 0;
        if (isIntLiteral(doWhile->condition.get(), &condVal) && condVal == 0) {
            // do { B } while (0) — body executes exactly once; replace with body.
            ++stats.deadLoops;
            stmt = std::move(doWhile->body);
        }
        break;
    }

    // ── Block statement ─────────────────────────────────────────────────────
    case ASTNodeType::BLOCK: {
        auto* block = static_cast<BlockStmt*>(stmt.get());
        auto sub = transformBlock(block);
        stats.deadIfBranches   += sub.deadIfBranches;
        stats.deadLoops        += sub.deadLoops;
        stats.unreachableStmts += sub.unreachableStmts;
        break;
    }

    // ── For statement (range loop) ──────────────────────────────────────────
    case ASTNodeType::FOR_STMT: {
        auto* forStmt = static_cast<ForStmt*>(stmt.get());
        if (forStmt->body) {
            auto sub = transformStmt(forStmt->body);
            stats.deadIfBranches   += sub.deadIfBranches;
            stats.deadLoops        += sub.deadLoops;
            stats.unreachableStmts += sub.unreachableStmts;
        }
        break;
    }

    // ── Variable declaration ────────────────────────────────────────────────
    case ASTNodeType::VAR_DECL:
        // No dead-code structure inside a simple declaration.
        break;

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
    if (!block) return stats;

    // Pass 1 — recursively simplify each statement.
    for (auto& s : block->statements) {
        auto sub = transformStmt(s);
        stats.deadIfBranches   += sub.deadIfBranches;
        stats.deadLoops        += sub.deadLoops;
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
        if (!s) continue;
        if (s->type == ASTNodeType::THROW_STMT) {
            // A throw is only a pruning boundary if NO catch block follows it
            // anywhere in the same block — i.e. it is truly unhandled.
            bool hasCatchAfter = false;
            for (size_t j = i + 1; j < block->statements.size(); ++j) {
                if (block->statements[j] &&
                    block->statements[j]->type == ASTNodeType::CATCH_STMT) {
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

        total.deadIfBranches   += fnStats.deadIfBranches;
        total.deadLoops        += fnStats.deadLoops;
        total.unreachableStmts += fnStats.unreachableStmts;

        if (verbose && (fnStats.deadIfBranches || fnStats.deadLoops || fnStats.unreachableStmts)) {
            std::cerr << "[DCE] " << fn->name
                      << ": " << fnStats.deadIfBranches   << " dead if-branch(es), "
                      << fnStats.deadLoops        << " dead loop(s), "
                      << fnStats.unreachableStmts << " unreachable stmt(s) removed\n";
        }
    });

    return total;
}

} // namespace omscript
