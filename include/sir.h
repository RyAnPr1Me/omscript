// === COMPILER LAYER 2 (SEMANTICS): Semantic IR (SIR) ===
// SIR is a dense, queryable representation of every semantic fact known about
// the program BEFORE LLVM IR is emitted.  It consolidates the outputs of all
// pre-passes (type analysis, purity, ERSL, CFCTRE, range analysis, uniqueness,
// borrow checking) into a single data structure that Layer 4 (codegen) can
// query at any point without re-deriving information.
//
// Design goals:
//   • Maximum information density — every field that a codegen pass might want.
//   • No LLVM types — purely language-level and AST-level facts.
//   • Cheap to build (single forward pass over AST after other passes run).
//   • Cheap to query (hash-map by variable/function name; O(1) lookups).
//
// Place in the pipeline:
//   kSIR runs as the LAST pre-pass (after HGOE-EGraph), consuming the outputs
//   of every prior pass and crystallising them into SIRModule.

#pragma once
#ifndef SIR_H
#define SIR_H

#include "ast.h"          // LoopConfig, StructRepr, FunctionEffects, etc.
#include "ersl.h"         // EscapeClass, EffectSummary, EffectKind, Region, Stability
#include "opt_context.h"  // ValueRange, FunctionFacts

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// SIRType — rich type descriptor for any value in the program
// ─────────────────────────────────────────────────────────────────────────────
//
// A SIRType captures everything the compiler knows about the concrete type of
// a variable or expression: base kind, bit-width, signedness, qualifiers
// (const / volatile / atomic), and for aggregate types the struct name or
// element type.  This replaces the scattered string-based type inference that
// currently lives inside CodeGenerator.
struct SIRType {
    enum class BaseKind : uint8_t {
        Unknown = 0, ///< Not yet determined (default)
        Void,        ///< No value
        Bool,        ///< 1-bit boolean (i1 in LLVM)
        Int,         ///< Signed integer (i8/i16/i32/i64/i128)
        UInt,        ///< Unsigned integer (u8/u16/u32/u64/u128)
        Float,       ///< Floating-point (f32 or f64)
        String,      ///< OmScript string (heap-allocated, ref-counted)
        Array,       ///< Dynamic array (typed element)
        Struct,      ///< Named struct type
        Enum,        ///< Named enum type (encoded as integer)
        BigInt,      ///< Arbitrary-precision integer
        Fn,          ///< First-class function value
    };

    BaseKind kind    = BaseKind::Unknown;
    int      bitWidth = 64;    ///< Bit-width for Int/UInt (8,16,32,64,128) or Float (32,64)
    bool     isConst     = false; ///< Declared with `const` / in a const context
    bool     isVolatile  = false; ///< Declared with `volatile`
    bool     isAtomic    = false; ///< Declared with `atomic`
    bool     isSigned    = true;  ///< True for Int, irrelevant for UInt

    // For Struct / Enum
    std::string structName;     ///< Name of the struct/enum type
    StructRepr  repr = StructRepr::Auto; ///< Layout repr for structs

    // For Array / Fn (element or return type)
    std::shared_ptr<SIRType> elementType; ///< Inner type (null if not applicable)

    // For fixed-size arrays (static dimension, -1 = dynamic)
    int64_t staticArrayLen = -1;

    // Convenience constructors
    static SIRType makeInt(int bits = 64, bool isSigned = true) {
        SIRType t; t.kind = BaseKind::Int; t.bitWidth = bits; t.isSigned = isSigned; return t;
    }
    static SIRType makeUInt(int bits = 64) {
        SIRType t; t.kind = BaseKind::UInt; t.bitWidth = bits; t.isSigned = false; return t;
    }
    static SIRType makeFloat(int bits = 64) {
        SIRType t; t.kind = BaseKind::Float; t.bitWidth = bits; return t;
    }
    static SIRType makeString() {
        SIRType t; t.kind = BaseKind::String; return t;
    }
    static SIRType makeArray(SIRType elem, int64_t len = -1) {
        SIRType t; t.kind = BaseKind::Array;
        t.elementType = std::make_shared<SIRType>(std::move(elem));
        t.staticArrayLen = len;
        return t;
    }
    static SIRType makeStruct(std::string name, StructRepr r = StructRepr::Auto) {
        SIRType t; t.kind = BaseKind::Struct; t.structName = std::move(name); t.repr = r; return t;
    }
    static SIRType makeBool() {
        SIRType t; t.kind = BaseKind::Bool; t.bitWidth = 1; return t;
    }
    static SIRType makeVoid() {
        SIRType t; t.kind = BaseKind::Void; return t;
    }
    static SIRType makeUnknown() { return {}; }

    /// True when this type is a numeric integer/bool type that carries value-range info.
    bool isIntegral() const noexcept {
        return kind == BaseKind::Int || kind == BaseKind::UInt || kind == BaseKind::Bool;
    }

    /// String representation for diagnostics.
    std::string str() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// SIRVarFacts — all semantic facts known about a single local variable
// ─────────────────────────────────────────────────────────────────────────────
//
// Populated by the SIR builder from: type inference, range analysis, ERSL,
// uniqueness analysis, and borrow checker results.  Codegen reads this instead
// of re-running analyses.
struct SIRVarFacts {
    SIRType type;  ///< Full type of the variable

    // ── Value range ───────────────────────────────────────────────────────
    std::optional<ValueRange>  range;       ///< Closed integer range [lo, hi]
    bool                       isNonNeg = false; ///< Proven lo >= 0

    // ── Compile-time value ────────────────────────────────────────────────
    std::optional<int64_t>     constIntVal; ///< Constant int, if known
    std::optional<std::string> constStrVal; ///< Constant string, if known

    // ── Ownership / aliasing ──────────────────────────────────────────────
    EscapeClass escape      = EscapeClass::NoEscape; ///< Where the value can escape
    bool        isUnique    = false; ///< No live aliases (uniqueness analysis)
    bool        isMoved     = false; ///< Has been moved (borrow checker)
    bool        isImmutable = false; ///< const or proven-never-reassigned
    bool        isAtomic    = false; ///< atomic qualifier
    bool        isVolatile  = false; ///< volatile qualifier
    bool        mayAlias    = true;  ///< Conservative: may alias another var

    // ── Borrow state ──────────────────────────────────────────────────────
    bool hasActiveBorrow    = false; ///< Has at least one active borrow ref
    bool hasActiveMutBorrow = false; ///< Has at least one active mutable borrow ref
};

// ─────────────────────────────────────────────────────────────────────────────
// SIRCallSite — semantic facts about a single call expression in the body
// ─────────────────────────────────────────────────────────────────────────────
struct SIRCallSite {
    std::string callee;  ///< Name of the called function (empty = indirect/unknown)

    // Per-argument const values (nullopt = not known at compile time)
    std::vector<std::optional<int64_t>> constIntArgs;
    std::vector<std::optional<std::string>> constStrArgs;

    // Effect of this specific call (callee's effect propagated to this site)
    EffectKind  effectKind    = EffectKind::None;
    Region      effectRegion  = Region::None;
    Stability   stability     = Stability::Unknown;
    EscapeClass argEscape     = EscapeClass::NoEscape;

    bool isTailPosition = false;  ///< Appears in tail position in the function
    bool isInlinable    = false;  ///< Worth inlining at this site (based on callee size + opt level)
    bool isPureCall     = false;  ///< Callee is pure → result can be CSE'd
    bool isBuiltin      = false;  ///< Callee is a built-in
};

// ─────────────────────────────────────────────────────────────────────────────
// SIRLoopInfo — rich info about one loop in the function body
// ─────────────────────────────────────────────────────────────────────────────
struct SIRLoopInfo {
    std::string iterVar;  ///< Name of the induction variable (empty for while/forever)

    // Bounds — only set when statically known
    std::optional<int64_t> staticStart;  ///< Constant start value
    std::optional<int64_t> staticEnd;    ///< Constant exclusive-end value
    std::optional<int64_t> staticStep;   ///< Constant step (default 1)

    // Derived loop properties
    std::optional<int64_t> tripCount;    ///< Exact trip count if statically computable
    std::optional<int64_t> tripCountMax; ///< Upper bound on trip count (may be imprecise)
    bool countingUp   = true;  ///< True = ascending (start < end)
    bool hasConstBounds = false; ///< Both start and end are compile-time constants

    int nestingDepth = 0;  ///< Nesting level (0 = outermost)

    // Arrays accessed inside the loop body (for prefetch / cache analysis)
    std::vector<std::string> readArrays;    ///< Arrays only read inside the loop
    std::vector<std::string> writtenArrays; ///< Arrays written inside the loop

    // User-provided optimization hints from @loop() annotations
    LoopConfig hints;  ///< Contains vectorize, unroll, tile, parallel, fuse, etc.

    // Derived safety flags
    bool isIterVarNonNeg = false; ///< Induction variable is always >= 0
    bool hasSideEffects  = true;  ///< Body may have I/O or writes to globals
    bool isCountable     = false; ///< Known to terminate in finite iterations
};

// ─────────────────────────────────────────────────────────────────────────────
// SIRParam — parameter descriptor for a function signature
// ─────────────────────────────────────────────────────────────────────────────
struct SIRParam {
    std::string name;
    SIRType     type;
    bool isBorrow    = false; ///< `borrow ref = &x`
    bool isMutBorrow = false; ///< `borrow mut ref = &x`
    bool isMove      = false; ///< Passed by move (takes ownership)
    bool isConst     = false; ///< Declared const (read-only at call site)
    std::optional<ValueRange> range; ///< Known range for integral parameters
};

// ─────────────────────────────────────────────────────────────────────────────
// SIRFunction — all semantic facts known about one function
// ─────────────────────────────────────────────────────────────────────────────
struct SIRFunction {
    std::string name;

    // ── Signature ─────────────────────────────────────────────────────────
    std::vector<SIRParam> params;
    SIRType               returnType;

    // ── Consolidated analysis results ─────────────────────────────────────
    // These are the distilled outputs of ALL pre-passes for this function.
    FunctionFacts facts;   ///< Purity, effects, ERSL, const-return, range

    // ── Per-variable semantic facts ───────────────────────────────────────
    // Variable name → rich semantic descriptor.  Includes all locals and
    // all loop induction variables.
    std::unordered_map<std::string, SIRVarFacts> varFacts;

    // ── Call sites ────────────────────────────────────────────────────────
    // One entry per call expression in the function body.  Used by the
    // inliner and devirtualizer.
    std::vector<SIRCallSite> callSites;

    // ── Loop structure ────────────────────────────────────────────────────
    // One entry per loop (for/while/foreach/etc.) in the function body.
    // Nested loops appear in pre-order (outer before inner), with
    // nestingDepth reflecting the nesting level.
    std::vector<SIRLoopInfo> loops;

    // ── Code shape ────────────────────────────────────────────────────────
    bool isLeaf       = false; ///< Contains no calls (or only leaf builtins)
    bool isRecursive  = false; ///< Directly self-recursive
    bool isEntry      = false; ///< Is a program entry point
    bool alwaysReturns = true; ///< Every path hits a return (or noreturn call)
    bool neverReturns  = false; ///< Annotated / proven noreturn

    // ── Inline hints ──────────────────────────────────────────────────────
    bool forceInline  = false; ///< @inline annotation or small body
    bool neverInline  = false; ///< @noinline annotation
    bool isHot        = false; ///< @hot annotation
    bool isCold       = false; ///< @cold annotation

    // ── Code-size estimate ────────────────────────────────────────────────
    // Rough instruction count estimate (not an LLVM instruction count).
    // Used to decide inlining profitability.
    unsigned estimatedBodySize = 0;

    // ── Callee set ────────────────────────────────────────────────────────
    // Names of all directly-called functions (for call-graph construction).
    std::unordered_set<std::string> directCallees;
};

// ─────────────────────────────────────────────────────────────────────────────
// SIRStructType — struct type descriptor with field-level facts
// ─────────────────────────────────────────────────────────────────────────────
struct SIRStructField {
    std::string name;
    SIRType     type;
    FieldAttrs  attrs;  ///< From the AST (align, immut, noalias, range, cold)
};

struct SIRStructType {
    std::string name;
    std::vector<SIRStructField> fields;
    StructRepr repr         = StructRepr::Auto;
    int        reprAlignN   = 0;
    bool       hasSoAGroup  = false; ///< @repr(soa) or @repr(aos_to_soa)
    bool       isExternC    = false; ///< @repr(C) — stable C ABI layout
    bool       isPacked     = false; ///< @repr(packed) — no padding
};

// ─────────────────────────────────────────────────────────────────────────────
// SIRModule — top-level semantic IR for an entire compilation unit
// ─────────────────────────────────────────────────────────────────────────────
//
// The canonical query interface for the compiler's semantic knowledge.
// Built by buildSIR() after all pre-passes have run.  Read-only after
// construction (codegen only reads, never writes to SIRModule).
struct SIRModule {
    // ── Functions ─────────────────────────────────────────────────────────
    std::unordered_map<std::string, SIRFunction> functions;

    // ── Type table ────────────────────────────────────────────────────────
    std::unordered_map<std::string, SIRStructType> structTypes;

    // ── Call graph ────────────────────────────────────────────────────────
    // caller → set of callees (direct calls only)
    std::unordered_map<std::string, std::unordered_set<std::string>> callGraph;
    // callee → set of callers (reverse call graph)
    std::unordered_map<std::string, std::unordered_set<std::string>> callerGraph;

    // ── Entry points ──────────────────────────────────────────────────────
    std::unordered_set<std::string> entryPoints;

    // ── Module-level metrics ──────────────────────────────────────────────
    unsigned totalFunctions     = 0;
    unsigned estimatedTotalSize = 0; ///< Sum of estimatedBodySize across functions

    // ── Query helpers ─────────────────────────────────────────────────────

    /// Return pointer to the SIRFunction for @p name, or null if not found.
    const SIRFunction* getFunction(const std::string& name) const noexcept {
        const auto it = functions.find(name);
        return it != functions.end() ? &it->second : nullptr;
    }

    /// Return pointer to the SIRVarFacts for variable @p varName in function
    /// @p funcName, or null if not found.
    const SIRVarFacts* getVarFacts(const std::string& funcName,
                                    const std::string& varName) const noexcept {
        const SIRFunction* fn = getFunction(funcName);
        if (!fn) return nullptr;
        const auto it = fn->varFacts.find(varName);
        return it != fn->varFacts.end() ? &it->second : nullptr;
    }

    /// Return pointer to the SIRStructType for struct @p name, or null.
    const SIRStructType* getStruct(const std::string& name) const noexcept {
        const auto it = structTypes.find(name);
        return it != structTypes.end() ? &it->second : nullptr;
    }

    /// Return true iff @p funcName is known to be pure.
    bool isFunctionPure(const std::string& funcName) const noexcept {
        const SIRFunction* fn = getFunction(funcName);
        return fn && fn->facts.isPure;
    }

    /// Return true iff @p funcName is a dead (unreachable) function.
    bool isFunctionDead(const std::string& funcName) const noexcept {
        const SIRFunction* fn = getFunction(funcName);
        return fn && fn->facts.isDead;
    }

    /// Return the maximum safe optimization level for @p funcName (1–5).
    int maxOptLevel(const std::string& funcName) const noexcept {
        const SIRFunction* fn = getFunction(funcName);
        return fn ? fn->facts.ersl.maxSafeOptLevel : 1;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// buildSIR — construct a SIRModule from an AST + OptimizationContext
// ─────────────────────────────────────────────────────────────────────────────
///
/// Must be called after ALL pre-passes have run (kHGOEEGraph is the last
/// dependency in the pipeline).  Reads FunctionFacts, range maps, uniqueness
/// sets, and struct type tables from the codegen context.
///
/// @param program  The (possibly-transformed) AST.
/// @param ctx      The OptimizationContext carrying all analysis results.
/// @param rangeMap Per-variable range maps keyed by function name, then var
///                 name. May be null/empty — missing entries fall back to the
///                 FunctionFacts.returnRange or wide [MIN, MAX].
/// @param uniqueSets Per-function sets of unique (no-alias) variable names.
///                 May be empty — missing entries conservatively set isUnique=false.
/// @param structFieldDecls  Struct field declarations carrying FieldAttrs.
///                 Forwarded from the CodeGenerator.  May be empty.
/// @param structReprs  Per-struct StructRepr enum forwarded from CodeGenerator.
/// @param structReprAlignN  Per-struct alignment forwarded from CodeGenerator.
///
/// @returns  A fully-populated SIRModule ready for codegen to consume.
SIRModule buildSIR(
    const Program&  program,
    const OptimizationContext& ctx,
    const std::unordered_map<std::string, std::unordered_map<std::string, ValueRange>>& rangeMap,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& uniqueSets,
    const std::unordered_map<std::string, std::vector<StructField>>& structFieldDecls,
    const std::unordered_map<std::string, StructRepr>&  structReprs,
    const std::unordered_map<std::string, int>&         structReprAlignN);

} // namespace omscript

#endif // SIR_H
