#pragma once
#ifndef WIDTH_LEGALIZATION_H
#define WIDTH_LEGALIZATION_H

/// @file width_legalization.h
/// @brief Semantic width tracking and hardware-friendly legalization for OmScript.
///
/// ## Design
///
/// The width system operates in two layers:
///
///  1. **Semantic width** — the exact minimum number of bits a value needs,
///     derived from compile-time range analysis, literal values, and explicit
///     type annotations.  Preserved through the optimization pipeline so that
///     redundant masks, unnecessary extends, and spurious control-flow can be
///     eliminated before legalization.
///
///  2. **Storage (legal) width** — the hardware-friendly size chosen just
///     before code generation.  Only these widths reach the LLVM backend:
///
///       • Scalar : 8, 16, 32, 64 bits
///       • Wide   : any multiple of 64 bits (128, 192, 256, 320, …)
///
/// ## Width normalization rule
///
///   bits ≤ 8   →  8
///   bits ≤ 16  → 16
///   bits ≤ 32  → 32
///   bits ≤ 64  → 64
///   bits > 64  → round_up_to_multiple_of_64(bits)
///
/// ## Operation width rules (prevent width explosion)
///
///   +, -        : max(lhs, rhs) + 1          (carry/borrow bit)
///   *           : lhs + rhs                  (full product width)
///   /, %        : max(lhs, rhs)              (result fits in dividend width)
///   &, |, ^, ~ : max(lhs, rhs)
///   <<          : lhs + rhs_max              (shift can double width)
///   >> (logic)  : lhs                        (shrinks)
///   >> (arith)  : lhs                        (shrinks)
///   compare     : 1                          (boolean result)
///   unary -     : lhs + 1                    (two's complement negation)
///
/// All results are then passed through legalizeWidth() before any LLVM type
/// is chosen.

#include <cstdint>
#include <optional>
#include <string>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// SemanticWidth — exact bit-width and signedness of a value
// ─────────────────────────────────────────────────────────────────────────────
///
/// Carries the minimum number of bits required to represent a value together
/// with its signedness.  A width of 0 means "unknown / untracked".

struct SemanticWidth {
    uint32_t bits     = 0;      ///< Exact minimum bits required (0 = unknown)
    bool     isSigned = true;   ///< True for i-prefixed (signed); false for u-prefixed

    // ── Construction helpers ──────────────────────────────────────────────

    static SemanticWidth unknown() noexcept { return {0, true}; }

    /// Signed width for an exact bit count.
    static SemanticWidth i(uint32_t bits) noexcept { return {bits, true}; }

    /// Unsigned width for an exact bit count.
    static SemanticWidth u(uint32_t bits) noexcept { return {bits, false}; }

    /// Derive from a signed constant value.
    static SemanticWidth fromSignedValue(int64_t v) noexcept;

    /// Derive from an unsigned constant value.
    static SemanticWidth fromUnsignedValue(uint64_t v) noexcept;

    /// Derive from a closed signed integer range [lo, hi].
    static SemanticWidth fromSignedRange(int64_t lo, int64_t hi) noexcept;

    /// Derive from a closed unsigned integer range [lo, hi].
    static SemanticWidth fromUnsignedRange(uint64_t lo, uint64_t hi) noexcept;

    /// Parse a type-annotation string ("i32", "u64", "int" → i64, "uint" → u64).
    /// Returns unknown() if the annotation is not a recognized width form.
    static SemanticWidth fromAnnotation(const std::string& ann) noexcept;

    // ── Predicates ────────────────────────────────────────────────────────

    bool isKnown()  const noexcept { return bits > 0; }
    bool isWide()   const noexcept { return bits > 64; }  ///< Needs multi-limb representation
    bool isScalar() const noexcept { return bits > 0 && bits <= 64; }

    // ── Formatting ────────────────────────────────────────────────────────

    /// Return the annotation string ("i32", "u8", "i128", …).
    std::string toString() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// legalizeWidth — map any bit count to a hardware-friendly storage size
// ─────────────────────────────────────────────────────────────────────────────
///
/// Scalar values (≤ 64 bits) snap to the next power-of-two byte boundary:
///   1..8  → 8    9..16 → 16    17..32 → 32    33..64 → 64
///
/// Wide values (> 64 bits) round up to the next multiple of 64:
///   65..128 → 128    129..192 → 192    …
///
/// A bits value of 0 (unknown) maps to 64 (the default register width).

uint32_t legalizeWidth(uint32_t bits) noexcept;

/// Apply legalizeWidth to a SemanticWidth, preserving signedness.
SemanticWidth legalize(SemanticWidth sw) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Operation width rules
// ─────────────────────────────────────────────────────────────────────────────
///
/// All helpers return the *semantic* (pre-legalization) width of the result.
/// Callers should pass the result through legalizeWidth() or legalize() before
/// choosing an LLVM type.

namespace OpWidthRules {

/// Width of `lhs + rhs` or `lhs - rhs` (needs one extra carry/borrow bit).
SemanticWidth addSub(SemanticWidth lhs, SemanticWidth rhs) noexcept;

/// Width of `lhs * rhs` (full product needs lhs.bits + rhs.bits).
SemanticWidth mul(SemanticWidth lhs, SemanticWidth rhs) noexcept;

/// Width of `lhs / rhs` or `lhs % rhs` (result fits within dividend width).
SemanticWidth divRem(SemanticWidth lhs, SemanticWidth rhs) noexcept;

/// Width of `lhs & rhs`, `lhs | rhs`, or `lhs ^ rhs`.
SemanticWidth bitwise(SemanticWidth lhs, SemanticWidth rhs) noexcept;

/// Width of `lhs << shift` where shift is at most shiftMax bits wide.
/// The shift is assumed to be a literal or narrow unsigned value so we add
/// the value of the shift operand as a bit-count extension.
SemanticWidth shl(SemanticWidth lhs, uint32_t shiftMax) noexcept;

/// Width of `lhs >> shift` (logical or arithmetic — result never wider than lhs).
SemanticWidth shr(SemanticWidth lhs) noexcept;

/// Width of a unary negation (`-lhs`): needs one extra bit.
SemanticWidth neg(SemanticWidth operand) noexcept;

/// Width of a comparison result (always 1 bit; callers should legalize to i8).
SemanticWidth compare() noexcept;

/// Join (widen to the larger) of two widths — used when a value flows through
/// a branch join (phi / select).
SemanticWidth join(SemanticWidth a, SemanticWidth b) noexcept;

} // namespace OpWidthRules

// ─────────────────────────────────────────────────────────────────────────────
// WidthInfo — analysis annotation attached to an AST expression node
// ─────────────────────────────────────────────────────────────────────────────
///
/// The width analysis pass populates one WidthInfo per expression node and
/// stores the map by expression pointer.  The legalization pass reads the
/// WidthInfo to choose the LLVM type for each sub-expression.

struct WidthInfo {
    SemanticWidth semantic;  ///< Exact minimum width computed by analysis
    SemanticWidth legal;     ///< Hardware-legalized width (= legalize(semantic))

    /// True when the semantic width fits in the legal width without truncation.
    bool fitsExact() const noexcept {
        if (!semantic.isKnown()) return true; // unknown — assume no truncation needed
        return semantic.bits <= legal.bits;
    }

    /// True when a zero-extend is needed on the way from semantic to legal
    /// storage (unsigned and smaller than legal width).
    bool needsZExt() const noexcept {
        return semantic.isKnown() && !semantic.isSigned && semantic.bits < legal.bits;
    }

    /// True when a sign-extend is needed (signed and smaller than legal width).
    bool needsSExt() const noexcept {
        return semantic.isKnown() && semantic.isSigned && semantic.bits < legal.bits;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// WidthAnalyzer — compute SemanticWidth for every expression in a program
// ─────────────────────────────────────────────────────────────────────────────
///
/// Usage:
///   WidthAnalyzer wa(ctx);
///   wa.analyze(program);
///   SemanticWidth sw = wa.widthOf(exprPtr);

class OptimizationContext;  // forward
class Program;              // forward (AST)
struct Expression;          // forward
struct Statement;           // forward
struct FunctionDecl;        // forward

#include <unordered_map>

class WidthAnalyzer {
public:
    explicit WidthAnalyzer(const OptimizationContext& ctx) noexcept : ctx_(ctx) {}

    /// Analyze the entire program; populates the internal width map.
    void analyze(const Program* program);

    /// Return the semantic width for an expression (unknown if not analyzed).
    SemanticWidth widthOf(const Expression* expr) const noexcept;

    /// Return the fully-populated WidthInfo (semantic + legal) for an expression.
    WidthInfo infoOf(const Expression* expr) const noexcept;

    /// Total number of expression nodes analyzed.
    std::size_t nodeCount() const noexcept { return widths_.size(); }

private:
    const OptimizationContext& ctx_;
    std::unordered_map<const Expression*, SemanticWidth> widths_;

    // Internal traversal helpers.
    SemanticWidth analyzeExpr(const Expression* expr);
    void          analyzeStmt(const Statement* stmt);
    void          analyzeFunction(const FunctionDecl* fn);

    // Cache the result and return it.
    SemanticWidth cache(const Expression* expr, SemanticWidth sw) {
        widths_[expr] = sw;
        return sw;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// WidthLegalizationPass — wrap analysis + legalized-type map for pipeline use
// ─────────────────────────────────────────────────────────────────────────────
///
/// This is the public entry point called by the orchestrator.  It runs
/// WidthAnalyzer and stores the result map in the OptimizationContext so
/// subsequent phases (LLVM IR emission) can look up legal types cheaply.

class WidthLegalizationPass {
public:
    explicit WidthLegalizationPass(OptimizationContext& ctx) noexcept : ctx_(ctx) {}

    /// Run the pass over the program and populate width annotations.
    void run(const Program* program);

    /// After run(), return the WidthInfo for an expression.
    WidthInfo infoOf(const Expression* expr) const noexcept;

    /// Number of wide (>64-bit) expressions detected.
    uint32_t wideCount() const noexcept { return wideCount_; }

    /// Number of expressions where semantic width differs from 64 bits
    /// (i.e., the pass found something actionable).
    uint32_t narrowedCount() const noexcept { return narrowedCount_; }

private:
    OptimizationContext& ctx_;
    WidthAnalyzer        analyzer_{ctx_};
    uint32_t             wideCount_    = 0;
    uint32_t             narrowedCount_ = 0;
};

} // namespace omscript

#endif // WIDTH_LEGALIZATION_H
