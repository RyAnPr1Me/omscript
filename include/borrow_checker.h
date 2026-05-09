#pragma once
#ifndef BORROW_CHECKER_H
#define BORROW_CHECKER_H

/// @file borrow_checker.h
/// @brief Standalone AST-level borrow checker for OmScript.
///
/// ## Purpose
///
/// This pass performs a flow-sensitive, intra-procedural borrow analysis on
/// each function's AST, detecting ownership violations BEFORE code generation.
/// It complements the codegen-embedded ownership checks by providing:
///
///   - Structured error codes (E015–E018) instead of ad-hoc codegenError()
///   - Precise source locations from AST nodes
///   - Early termination: errors are caught before expensive IR generation
///   - Clear, user-facing messages referencing the violating variable by name
///
/// ## What it checks
///
/// | Code | Violation                                         |
/// |------|---------------------------------------------------|
/// | E015 | Read or write of a variable after it was moved    |
/// | E016 | Write to a variable with active immutable borrow(s)|
/// | E017 | Creating a mutable borrow of an already-mutably-  |
/// |      | borrowed variable (double mutable borrow)         |
/// | E018 | Moving a variable that still has active borrow(s) |
///
/// ## Scope handling
///
/// Borrows are released at the end of the lexical scope in which the borrow
/// reference was declared.  This mirrors OmScript's `borrow ref = &x;` /
/// `borrow mut ref = &x;` syntax.
///
/// ## Control-flow handling
///
/// The checker handles:
///   - Sequential blocks: straightforward forward propagation
///   - `if/else`: both branches are checked independently; states are joined
///     conservatively (a variable moved in *either* branch is marked moved)
///   - `while`/`for`/`foreach`/`do-while`: the loop body is checked from the
///     pre-loop state; variables moved inside the body are marked as possibly
///     moved after the loop (conservative)
///
/// ## Integration
///
/// `runBorrowCheck(program)` is called as pass `kBorrowCheck` in the
/// optimization orchestrator, after parsing and before any AST transforms.

#include "ast.h"
#include "diagnostic.h"
#include <string>
#include <vector>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// BorrowState — per-variable ownership/borrow state
// ─────────────────────────────────────────────────────────────────────────────

/// Ownership and borrow state for a single variable.
struct BorrowState {
    int  immutBorrows   = 0;  ///< Active immutable borrows (from `borrow`)
    int  reborrows      = 0;  ///< Active reborrow aliases (from `reborrow`)
    bool mutBorrowed    = false;  ///< True when a mutable borrow is active
    bool moved          = false;  ///< True after the variable's value was moved out
    bool invalidated    = false;  ///< True after an explicit `invalidate` statement
    bool frozen         = false;  ///< True after a `freeze` statement
    bool shared         = false;  ///< True after a `shared x;` statement (Ω spec §3.1)
                                  ///< Read-only aliasable ownership: multiple immutable
                                  ///< borrows allowed, but mutation and mutable borrows
                                  ///< are compile-time errors.  Still owns the memory.

    /// True if the variable cannot be used at all (moved or invalidated).
    bool isDead()      const noexcept { return moved || invalidated; }

    /// True if the variable can be read.
    bool isReadable()  const noexcept { return !isDead() && !mutBorrowed; }

    /// True if the variable can be written (assigned to).
    bool isWritable()  const noexcept {
        return !isDead() && !mutBorrowed && immutBorrows == 0 && reborrows == 0
               && !frozen && !shared;
    }

    /// True if ownership can be moved out.
    /// A frozen variable may be moved even when it has active *reborrow* aliases
    /// (`reborrow` is explicitly a short-lived, non-owning alias; the programmer
    /// promises not to use the alias after the move).  Full `borrow` aliases still
    /// block the move.
    bool isMovable()   const noexcept {
        if (isDead() || mutBorrowed || immutBorrows > 0) return false;
        // Frozen/shared + only reborrows: caller has promised the reborrows are dead.
        if ((frozen || shared) && reborrows > 0) return true;
        return reborrows == 0;
    }

    /// True if an additional immutable borrow can be created.
    bool canImmutBorrow() const noexcept { return !isDead() && !mutBorrowed; }

    /// True if a mutable borrow can be created.
    /// Shared variables do not permit mutable borrows (Ω spec §3.1).
    bool canMutBorrow()   const noexcept {
        return !isDead() && !mutBorrowed && immutBorrows == 0 && reborrows == 0
               && !frozen && !shared;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// BorrowCheckResult — aggregated result for one program
// ─────────────────────────────────────────────────────────────────────────────

/// A single memory-sanitizer diagnostic entry produced by `--mem-sanitize`.
struct MemSanitizerDiag {
    std::string kind;       ///< "use-after-invalidate", "null-deref", etc.
    std::string varName;    ///< Variable involved
    std::string file;       ///< Source file name
    int         causeLine;  ///< Line where the invalidation/null-assign happened
    int         useLine;    ///< Line of the invalid use
    std::string causeDesc;  ///< Human-readable cause (e.g. "invalidate p")
    std::string useDesc;    ///< Human-readable use (e.g. "*p (invalid use)")
};

/// Result from running the borrow checker over a program.
struct BorrowCheckResult {
    /// True when at least one E015–E018 error was found.
    bool hadError = false;
    /// Memory-sanitizer diagnostics (only populated with --mem-sanitize).
    std::vector<MemSanitizerDiag> memSanitizerDiags;
};

// ─────────────────────────────────────────────────────────────────────────────
// runBorrowCheck — public entry point
// ─────────────────────────────────────────────────────────────────────────────

/// Run the standalone borrow checker over every function in @p program.
///
/// Throws `DiagnosticError` (with code E015–E018) on the first borrow
/// violation found (unless @p noOwnershipChecks is true).  Returns normally
/// if no violations are detected.
///
/// @param program           The parsed program to check.
/// @param verbose           When true, log per-function check activity to stderr.
/// @param noOwnershipChecks When true, skip all borrow/invalidation checks
///                          (Ω spec §6.2: --no-ownership-checks flag).
/// @param memSanitize       When true, perform additional path-sensitive
///                          diagnostics and populate result.memSanitizerDiags
///                          (Ω spec §7: --mem-sanitize flag).
BorrowCheckResult runBorrowCheck(const Program& program,
                                 bool verbose          = false,
                                 bool noOwnershipChecks = false,
                                 bool memSanitize      = false);

} // namespace omscript

#endif // BORROW_CHECKER_H
