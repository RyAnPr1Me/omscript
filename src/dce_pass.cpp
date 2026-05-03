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
///     Once a return, break, continue, or throw is encountered in a block's
///     statement list, all following statements are dropped.

#include "dce_pass.h"
#include "pass_utils.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

namespace omscript {

/// Returns true when @p stmt terminates control flow unconditionally in its
/// own block (return, break, continue, throw).
static bool isUnconditionalExit(const Statement* stmt) {
    if (!stmt) return false;
    switch (stmt->type) {
        case ASTNodeType::RETURN_STMT:
        case ASTNodeType::BREAK_STMT:
        case ASTNodeType::CONTINUE_STMT:
        case ASTNodeType::THROW_STMT:
            return true;
        default:
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

static DCEStats transformBlock(BlockStmt* block);
static DCEStats transformStmt(std::unique_ptr<Statement>& stmt);

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
        if (s->type == ASTNodeType::RETURN_STMT ||
            s->type == ASTNodeType::BREAK_STMT  ||
            s->type == ASTNodeType::CONTINUE_STMT) {
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
