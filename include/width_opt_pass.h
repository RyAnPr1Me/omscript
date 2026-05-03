#pragma once
#ifndef WIDTH_OPT_PASS_H
#define WIDTH_OPT_PASS_H

/// @file width_opt_pass.h
/// @brief Width-aware optimizations: masking elimination, shift narrowing, branch pruning.
///
/// ## Motivation
///
/// Once semantic widths are known for every expression, three cheap but high-
/// value transformations become possible at the AST level:
///
///  1. **Masking elimination** — `x & M` where M covers all bits of x.
///     If `x` has semantic width W bits and M is a mask of W or more bits
///     (i.e., all bits of x are already inside the mask), the AND is a no-op
///     and can be replaced by `x` directly.
///     Example:  `u8 x`, `x & 0xFF`  →  `x`
///               `u8 x`, `x & 0x0F`  →  keep (mask narrows the value)
///
///  2. **Shift narrowing** — `x >> N` (logical) where x has W semantic bits.
///     If N >= W, the result is provably zero.
///     If N > 0, the result has at most W-N bits; the semantic width is updated.
///     The pass tags zero-result shifts for DCE and annotates surviving shifts
///     with tighter semantic widths so later analysis sees the improvement.
///
///  3. **Impossible-branch pruning (width-based VRP)** — A comparison whose
///     result range is impossible given the operand widths folds to a literal
///     false (for ==) or literal true (for !=), enabling DCE to remove the
///     dead branch.
///     Examples:
///       `(x & 0xFF) == 300`  — LHS ∈ [0,255], RHS = 300: always false → fold
///       `(x & 0xFF) != 300`  — always true → fold
///       `u8_var > 300`       — u8 ∈ [0,255], 300 > 255: always false → fold
///
/// ## Integration
///
/// This pass is registered as `kWidthOpt` in the pipeline and scheduled
/// immediately after `kWidthLegalization`.  It is a purely AST-level
/// CostTransform; it never changes semantics and produces no new allocations.
/// It invalidates range_analysis and width_legalization (which will be
/// re-run by subsequent demand-driven queries if needed).

#include "ast.h"
#include "width_legalization.h"
#include <cstdint>

namespace omscript {

class OptimizationContext;

// ─────────────────────────────────────────────────────────────────────────────
// WidthOptStats — pass statistics
// ─────────────────────────────────────────────────────────────────────────────

struct WidthOptStats {
    uint32_t masksEliminated  = 0; ///< `x & M` → `x` eliminations
    uint32_t shiftsNarrowed   = 0; ///< Shifts with tightened result width
    uint32_t shiftsZeroed     = 0; ///< `x >> N` → 0 (N >= width(x))
    uint32_t branchesPruned   = 0; ///< Impossible comparisons folded to literal
};

// ─────────────────────────────────────────────────────────────────────────────
// WidthOptPass — main pass class
// ─────────────────────────────────────────────────────────────────────────────

class WidthOptPass {
public:
    WidthOptPass(OptimizationContext& ctx, bool verbose) noexcept
        : ctx_(ctx), verbose_(verbose) {}

    /// Run all three sub-passes over the program.
    /// Returns the number of transformations applied (0 = nothing changed).
    uint32_t run(Program* program);

    const WidthOptStats& stats() const noexcept { return stats_; }

    /// Value range: closed interval [lo, hi] with a `known` flag.
    /// When `known` is false, the range covers the full i64 domain.
    struct Range { int64_t lo; int64_t hi; bool known; };

private:
    OptimizationContext& ctx_;
    bool                 verbose_;
    WidthOptStats        stats_;
    WidthAnalyzer*       analyzer_ = nullptr; // borrowed from WidthLegalizationPass

    // ── Expression transformers ───────────────────────────────────────────

    /// Try to simplify a single expression.  Returns a replacement expression
    /// or nullptr if nothing changed.  The returned node is heap-allocated and
    /// ownership passes to the caller.
    std::unique_ptr<Expression> trySimplifyExpr(Expression* expr);

    /// Recursively walk and transform an expression in-place.
    /// Ownership of `expr` is NOT transferred; the pointer inside the unique_ptr
    /// is replaced if simplification fires.
    void transformExprInPlace(std::unique_ptr<Expression>& expr);

    /// Recursively walk and transform a statement in-place.
    void transformStmtInPlace(std::unique_ptr<Statement>& stmt);

    // ── Sub-pass helpers ──────────────────────────────────────────────────

    /// Masking elimination: try to replace `bin` (an `&` node) with its LHS.
    std::unique_ptr<Expression> tryEliminateMask(BinaryExpr* bin);

    /// Shift narrowing / zeroing: try to fold `bin` (a `>>` or `>>>` node).
    std::unique_ptr<Expression> tryNarrowShift(BinaryExpr* bin);

    /// Branch pruning: try to fold `bin` (a comparison) to a literal bool.
    std::unique_ptr<Expression> tryPruneBranch(BinaryExpr* bin);

    // ── Utility ───────────────────────────────────────────────────────────

    /// Return the semantic width of an expression (forwarded to analyzer_).
    SemanticWidth widthOf(const Expression* expr) const noexcept;

    /// Return the value range (lo..hi) for an expression, derived from the
    /// semantic width.  Unsigned: [0, 2^bits-1].  Signed: [-(2^(bits-1)), 2^(bits-1)-1].
    Range rangeOf(const Expression* expr) const noexcept;

    /// True when `val` is a literal integer with value `out`.
    static bool isLiteralInt(const Expression* e, int64_t& out) noexcept;

    /// True when `val` is a literal integer mask covering at least `bits` bits
    /// (i.e., all of 0..(2^bits - 1) pass through the mask unchanged).
    static bool isMaskCovering(int64_t mask, uint32_t bits) noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// Free function — called by the orchestrator
// ─────────────────────────────────────────────────────────────────────────────

/// Run the width-aware optimization pass over the program.
/// Returns the number of transformations applied.
uint32_t runWidthOptPass(Program* program, OptimizationContext& ctx, bool verbose = false);

} // namespace omscript

#endif // WIDTH_OPT_PASS_H
