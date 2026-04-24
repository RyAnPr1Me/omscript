#pragma once

#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "cfctre.h"
#include "diagnostic.h"
#include "opt_context.h"
#include "optimization_manager.h"
#include <functional>
#include <iostream>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declaration: avoids including the full TargetMachine header.
namespace llvm {
class TargetMachine;
} // namespace llvm

namespace omscript {

// Returns true if the given name is a stdlib built-in function.
// Stdlib functions are always compiled to native machine code.
[[nodiscard]] bool isStdlibFunction(const std::string& name);

enum class OptimizationLevel {
    O0, // No optimization
    O1, // Basic optimization
    O2, // Moderate optimization
    O3  // Aggressive optimization
};

/// Counters tracking which optimizations fired during compilation.
/// Printed when verbose mode is on or @optmax(report=true) is set.
struct OptStats {
    unsigned constFolded      = 0; ///< Expressions folded to compile-time constants
    unsigned callsInlined     = 0; ///< Call sites inlined
    unsigned escapeStackAllocs = 0; ///< Array/struct allocations moved to stack (escape analysis)
    unsigned roGlobalArrays   = 0; ///< Read-only array literals bound directly to a private global (no alloc, no copy)
    unsigned foreachRangeFused = 0; ///< `for x in range(a,b)` lowered to a direct counting loop (no array alloc)
    unsigned loopsFused       = 0; ///< Loop pairs fused by the @fuse pre-pass
    unsigned borrowsFrozen    = 0; ///< Variables frozen (freeze + alias propagation)
    unsigned independentLoops = 0; ///< Loops annotated with @independent
    unsigned allocatorFuncs   = 0; ///< User functions annotated as @allocator
    unsigned regionsCoalesced = 0; ///< Region pairs coalesced by the RLC pass

    // ── CF-CTRE / abstract-interpretation statistics ──────────────────────
    // Populated from CTEngine::stats() at print time when a CT engine is
    // available.  Surface the analysis evidence the optimizer collected, so
    // users can see which proofs/folds the engine made and which did not fire.
    unsigned pureFunctions       = 0; ///< Pure user functions detected by purity analysis
    unsigned uniformReturnFns    = 0; ///< Pure functions with always-the-same constant return
    unsigned deadFunctions       = 0; ///< Functions unreachable from any entry point
    unsigned loopsReasoned       = 0; ///< For-loops handled by closed-form symbolic reasoning
    unsigned branchMerges        = 0; ///< Path-sensitive branch merges (symbolic-IF folds)
    unsigned partialEvalFolds    = 0; ///< Partial-specialisation cache hits
    unsigned algebraicFolds      = 0; ///< Algebraic identity simplifications (x+0, x-x, …)
    unsigned constraintFolds     = 0; ///< Branch-constraint folds (range narrowing)
    unsigned deadBranches        = 0; ///< Branches proven always-dead by abstract interpretation
    unsigned safeArrayAccesses   = 0; ///< Array accesses proven in-bounds
    unsigned safeDivisions       = 0; ///< Divisions proven non-zero divisor
    unsigned cheaperRewrites     = 0; ///< Range-conditioned strength reductions

    // ── Missed-opportunity hints ─────────────────────────────────────────
    // Higher-level diagnostic counts the user can act on:
    unsigned almostPureFns       = 0; ///< @pure-annotated functions whose body looks impure
    unsigned untaggedPureFns     = 0; ///< Pure functions without an explicit @pure annotation

    void print() const {
        std::cout << "\n[opt-report] Optimization statistics:\n"
                  << "  const-folded expressions : " << constFolded << "\n"
                  << "  calls inlined            : " << callsInlined << "\n"
                  << "  stack allocs (escape)    : " << escapeStackAllocs << "\n"
                  << "  ro-global arrays         : " << roGlobalArrays << "\n"
                  << "  foreach-range fused      : " << foreachRangeFused << "\n"
                  << "  loops fused              : " << loopsFused << "\n"
                  << "  borrows frozen           : " << borrowsFrozen << "\n"
                  << "  independent loops        : " << independentLoops << "\n"
                  << "  allocator wrappers       : " << allocatorFuncs << "\n"
                  << "  regions coalesced (RLC)  : " << regionsCoalesced << "\n";

        // Only print the CF-CTRE block when at least one CT-engine counter is
        // non-zero (avoids visual clutter for pipelines that don't run CTRE).
        const bool anyCtre = pureFunctions | uniformReturnFns | deadFunctions
                           | loopsReasoned | branchMerges | partialEvalFolds
                           | algebraicFolds | constraintFolds | deadBranches
                           | safeArrayAccesses | safeDivisions | cheaperRewrites;
        if (anyCtre) {
            std::cout << "  --- compile-time reasoning ---\n"
                      << "  pure functions           : " << pureFunctions << "\n"
                      << "  uniform-return functions : " << uniformReturnFns << "\n"
                      << "  dead functions           : " << deadFunctions << "\n"
                      << "  loops reasoned (O(1))    : " << loopsReasoned << "\n"
                      << "  branch merges            : " << branchMerges << "\n"
                      << "  partial-eval folds       : " << partialEvalFolds << "\n"
                      << "  algebraic folds          : " << algebraicFolds << "\n"
                      << "  constraint folds         : " << constraintFolds << "\n"
                      << "  dead branches            : " << deadBranches << "\n"
                      << "  safe array accesses      : " << safeArrayAccesses << "\n"
                      << "  safe divisions           : " << safeDivisions << "\n"
                      << "  cheaper rewrites         : " << cheaperRewrites << "\n";
        }

        // Missed opportunities — only show when there is something actionable.
        if (almostPureFns || untaggedPureFns) {
            std::cout << "  --- optimization opportunities ---\n";
            if (untaggedPureFns)
                std::cout << "  unannotated pure fns     : " << untaggedPureFns
                          << "  (consider adding @pure)\n";
            if (almostPureFns)
                std::cout << "  @pure with impure body   : " << almostPureFns
                          << "  (annotation may be incorrect)\n";
        }
    }
};

/// Ownership lattice states for compile-time memory safety.
/// Owned→Borrowed/MutBorrowed/Frozen/Moved/Invalidated; see VarBorrowState for transitions.
enum class OwnershipState {
    Owned,        ///< Variable owns its value — full read/write access
    Borrowed,     ///< Has ≥1 immutable borrows — readable but not writable
    MutBorrowed,  ///< Has one mutable alias — source is completely locked
    Frozen,       ///< Permanently immutable — all loads are invariant
    Moved,        ///< Ownership transferred out — use is a compile error
    Invalidated   ///< Explicitly killed — use is a compile error
};

/// Per-variable borrow state (count-based ownership tracking).
struct VarBorrowState {
    int  immutBorrowCount = 0;  ///< Number of active immutable borrows
    bool mutBorrowed      = false; ///< True if there is one active mutable borrow
    bool moved            = false;
    bool invalidated      = false;
    bool frozen           = false;

    bool isDead()     const { return moved || invalidated; }
    /// Source can be read when not mutably borrowed and not dead.
    bool isReadable() const { return !isDead() && !mutBorrowed; }
    /// Source can be written only when no borrows exist, not frozen, not dead.
    bool isWritable() const {
        return !isDead() && !mutBorrowed && immutBorrowCount == 0 && !frozen;
    }
    /// Derive the canonical OwnershipState.
    OwnershipState state() const {
        if (invalidated)         return OwnershipState::Invalidated;
        if (moved)               return OwnershipState::Moved;
        if (frozen)              return OwnershipState::Frozen;
        if (mutBorrowed)         return OwnershipState::MutBorrowed;
        if (immutBorrowCount > 0) return OwnershipState::Borrowed;
        return OwnershipState::Owned;
    }
};

/// Per-scope record of the borrow aliases introduced in that scope.
/// When the scope ends every borrow is released.
struct BorrowInfo {
    std::string refVar;    ///< Name of the borrow variable (the alias)
    std::string srcVar;    ///< Name of the source variable being borrowed
    bool        isMut;     ///< true for mutable borrow, false for immutable
};

class CodeGenerator {
  public:
    CodeGenerator(OptimizationLevel optLevel = OptimizationLevel::O2);
    ~CodeGenerator();

    void generate(Program* program);
    void writeObjectFile(const std::string& filename);
    /// Write the module as LLVM bitcode for full link-time optimization (FLTO).
    void writeBitcodeFile(const std::string& filename);
    [[nodiscard]] llvm::Module* getModule() noexcept {
        return module.get();
    }
    void setOptimizationLevel(OptimizationLevel level) {
        optimizationLevel = level;
    }

    /// Set the target CPU architecture ("native" or "" for host auto-detection).
    void setMarch(const std::string& cpu) {
        marchCpu_ = cpu;
    }

    /// Set the CPU model for scheduling/micro-architecture tuning (defaults to -march).
    void setMtune(const std::string& cpu) {
        mtuneCpu_ = cpu;
    }

    /// Enable or disable position-independent code generation (default: true).
    void setPIC(bool enable) {
        usePIC_ = enable;
    }

    /// Enable or disable unsafe floating-point optimizations (default: false).
    void setFastMath(bool enable) {
        useFastMath_ = enable;
    }

    /// Enable or disable OPTMAX block optimization (default: true).
    void setOptMax(bool enable) {
        enableOptMax_ = enable;
    }

    /// Enable or disable explicit loop vectorization hints (default: true at O2+).
    void setVectorize(bool enable) {
        enableVectorize_ = enable;
    }

    /// Enable or disable loop unrolling hints (default: true at O2+).
    void setUnrollLoops(bool enable) {
        enableUnrollLoops_ = enable;
    }

    /// Enable or disable polyhedral-style loop optimizations (default: true at O3).
    void setLoopOptimize(bool enable) {
        enableLoopOptimize_ = enable;
    }

    /// Enable or disable automatic loop parallelization (default: true at O2+).
    void setParallelize(bool enable) {
        enableParallelize_ = enable;
    }

    /// Enable or disable e-graph equality saturation optimization (default: true at O2+).
    void setEGraphOptimize(bool enable) {
        enableEGraph_ = enable;
    }

    /// Enable or disable the superoptimizer pass (default: true at O2+).
    void setSuperoptimize(bool enable) {
        enableSuperopt_ = enable;
    }

    /// Set the superoptimizer aggressiveness level (0=off, 1=light, 2=normal, 3=aggressive).
    void setSuperoptLevel(unsigned level) {
        superoptLevel_ = level;
        enableSuperopt_ = (level > 0);
    }

    /// Enable/disable the Hardware Graph Optimization Engine (default: true).
    /// HGOE activates only when -march/-mtune are set; this flag allows disabling it explicitly.
    void setHardwareGraphOpt(bool enable) {
        enableHGOE_ = enable;
    }

    /// Enable/disable Speculative Devectorization & Revectorization (SDR, default: true at O2+).
    void setSDR(bool enable) {
        enableSDR_ = enable;
    }

    /// Enable/disable the Implicit Phase Ordering Fixer (IPOF, default: true at O2+).
    void setIPOF(bool enable) {
        enableIPOF_ = enable;
    }

    /// Set IPOF aggression level (0=off, 1=fast, 2=balanced, 3=aggressive).
    void setIPOFLevel(unsigned level) {
        ipofLevel_   = std::min(level, 3u);
        enableIPOF_  = (level > 0);
    }

    /// Enable/disable post-codegen LLVM IR optimization pipeline (default: true).
    void setRunIRPasses(bool enable) {
        runIRPasses_ = enable;
    }

    /// Enable PGO instrumentation generation mode (writes .profraw to profilePath).
    void setPGOGen(const std::string& profilePath) {
        pgoGenPath_ = profilePath;
    }

    /// Enable PGO use mode (reads .profdata from profilePath for guided optimization).
    void setPGOUse(const std::string& profilePath) {
        pgoUsePath_ = profilePath;
    }

    /// Enable LTO pre-link pipeline (defers heavy IPO to linker).
    void setLTO(bool enable) {
        lto_ = enable;
    }

    /// Enable/disable DWARF debug info generation.
    void setDebugMode(bool enable) {
        debugMode_ = enable;
    }

    /// Enable/disable verbose output during code generation.
    void setVerbose(bool enable) {
        verbose_ = enable;
    }

    [[nodiscard]] bool isVerbose() const noexcept {
        return verbose_;
    }

    /// Set the source filename for debug info metadata.
    void setSourceFilename(const std::string& filename) {
        sourceFilename_ = filename;
    }

    /// Return accumulated optimization statistics.
    [[nodiscard]] const OptStats& getOptStats() const noexcept {
        return optStats_;
    }

    /// Return the unified optimization context (non-null after generate()).
    [[nodiscard]] const OptimizationContext* optimizationContext() const noexcept {
        return optCtx_.get();
    }

    /// True when @p name is classified as const-foldable (auto-detected or @const_eval).
    [[nodiscard]] bool isConstEvalFunction(const std::string& name) const noexcept {
        return optCtx_ && optCtx_->isConstFoldable(name);
    }

    /// Return the always-constant integer return value for @p name, if known.
    [[nodiscard]] std::optional<int64_t> getConstIntReturn(const std::string& name) const noexcept {
        return optCtx_ ? optCtx_->constIntReturn(name) : std::nullopt;
    }

    /// Return the always-constant string return value for @p name, if known.
    [[nodiscard]] std::optional<std::string> getConstStringReturn(const std::string& name) const noexcept {
        return optCtx_ ? optCtx_->constStringReturn(name) : std::nullopt;
    }

    /// Return the effect summary for @p name (default if not analysed).
    [[nodiscard]] FunctionEffects getFunctionEffects(const std::string& name) const noexcept {
        return optCtx_ ? optCtx_->effects(name) : FunctionEffects{};
    }

    void preAnalyzeStringTypes(Program* program);
    void preAnalyzeArrayTypes(Program* program);
    void analyzeConstantReturnValues(Program* program);
    void autoDetectConstEvalFunctions(Program* program);
    void inferFunctionEffects(Program* program);

    /// E-graph pre-pass wrapper (checks enableEGraph_, called by Orchestrator).
    void runEGraphPass(Program* program, OptimizationContext& ctx);

    /// Public-facing wrapper for runSynthesisPass that the Orchestrator calls.
    /// Delegates to the free function declared in synthesize.h.
    void runSynthesisPass(Program* program, bool verbose);

    /// Public-facing wrapper for the CF-CTRE pre-pass (was private).
    void runCFCTRE(Program* program);

    /// Run the Region Lifetime Coalescing pass (coalesces disjoint regions).
    void runRLCPass(Program* program, bool verbose);

  private:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::Module> module;

    /// Optimization statistics accumulated during code generation.
    OptStats optStats_;

    llvm::StringMap<llvm::Value*> namedValues;
    std::vector<std::unordered_map<std::string, llvm::Value*>> scopeStack;

    // Defer stack: each scope level has its own list of deferred statements (LIFO)
    std::vector<std::vector<Statement*>> deferStack;

    struct LoopContext {
        llvm::BasicBlock* breakTarget;
        llvm::BasicBlock* continueTarget;
    };
    std::vector<LoopContext> loopStack;

    /// Iterator variables proven safe for specific arrays (cleared on loop exit).
    llvm::StringSet<> safeIndexVars_;

    /// Maps iterator variable name → its LLVM upper bound value.
    /// Used to emit llvm.assume hints for the optimizer.
    llvm::StringMap<llvm::Value*> loopIterEndBound_;

    /// Maps iterator variable → start bound (used for arr[i-K] elision when start ≥ K).
    llvm::StringMap<llvm::Value*> loopIterStartBound_;

    /// Maps iterator → array name used as loop end bound (enables bounds check elision).
    llvm::StringMap<std::string> loopIterEndArray_;

    /// LLVM global variable registry: name → GlobalVariable*.
    llvm::StringMap<llvm::GlobalVariable*> globalVars_;

    /// Maps array name → size alloca from array_fill(n,v) for bounds check elision.
    llvm::StringMap<llvm::AllocaInst*> knownArraySizeAllocas_;

    // Per-function catch table: error-code (i64) → BasicBlock; reset per function.
    std::unordered_map<int64_t, llvm::BasicBlock*> catchTable_;
    // Maps string error codes to their assigned integer IDs for this module.
    std::unordered_map<std::string, int64_t> catchStringIds_;
    int64_t nextCatchStringId_ = 1; // start at 1; 0 reserved for "no error"
    // Default (unmatched-throw) block used by the jump table's default arm.
    llvm::BasicBlock* catchDefaultBB_ = nullptr;
    bool inOptMaxFunction;
    bool hasOptMaxFunctions;
    llvm::StringSet<> optMaxFunctions;
    OptMaxConfig currentOptMaxConfig_;
    /// Maps LLVM function name → its @optmax config, for use by the
    /// post-generation pass in optimizeOptMaxFunctions().
    std::unordered_map<std::string, OptMaxConfig> optMaxFunctionConfigs_;

    struct ConstBinding {
        bool wasPreviouslyDefined;
        bool previousIsConst;
        // Previous constIntFolds_ value for this variable (if any).
        bool hadPreviousIntFold = false;
        int64_t previousIntFold = 0;
        // Previous constFloatFolds_ value for this variable (if any).
        bool hadPreviousFloatFold = false;
        double previousFloatFold = 0.0;
        // Previous constStringFolds_ value for this variable (if any).
        bool hadPreviousStringFold = false;
        std::string previousStringFold;
    };
    llvm::StringMap<bool> constValues;
    std::vector<std::unordered_map<std::string, ConstBinding>> constScopeStack;
    llvm::StringMap<llvm::Function*> functions;

    /// Maps variable name → declared type annotation (empty = untyped/i64).
    llvm::StringMap<std::string> varTypeAnnotations_;

    // Store AST function declarations for default parameter lookup at call sites.
    llvm::StringMap<const FunctionDecl*> functionDecls_;

    // Enum constant values (name → integer value), populated from enum declarations.
    llvm::StringMap<long long> enumConstants_;

    // Enum scope registry: maps enum name → set of member names for scope resolution validation.
    std::unordered_map<std::string, std::vector<std::string>> enumMembers_;

    // Struct type definitions: struct name → ordered list of field names.
    std::unordered_map<std::string, std::vector<std::string>> structDefs_;
    // Rich struct field metadata: struct name → ordered list of StructField with attributes.
    std::unordered_map<std::string, std::vector<StructField>> structFieldDecls_;
    // Variables known to hold struct values, maps var name → struct type name.
    std::unordered_map<std::string, std::string> structVars_;
    // Per-struct LLVM StructType (built lazily; enables SROA/mem2reg for small structs).
    std::unordered_map<std::string, llvm::StructType*> structLLVMTypes_;

    // Operator overload registry: maps "StructName::op" → generated LLVM function name.
    // e.g. "Vec2::+" → "__op_Vec2_add"
    std::unordered_map<std::string, std::string> operatorOverloads_;

    OptimizationLevel optimizationLevel;

    // String type tracking: stringVars_ (i64-stored string vars), stringReturningFunctions_,
    // funcParamStringTypes_ (param indices), stringArrayVars_ (arrays of string pointers).
    llvm::StringSet<> stringVars_;
    llvm::StringSet<> stringReturningFunctions_;
    std::unordered_map<std::string, std::unordered_set<size_t>> funcParamStringTypes_;
    llvm::StringSet<> stringArrayVars_;
    // arrayVars_: non-string pointer-typed array vars (disambiguates from string pointers in isStringExpr).
    llvm::StringSet<> arrayVars_;
    // Whole-program array type info (mirrors string type system, see preAnalyzeArrayTypes).
    llvm::StringSet<> arrayReturningFunctions_;
    std::unordered_map<std::string, std::unordered_set<size_t>> funcParamArrayTypes_;
    // stringLenCache_: cached strlen alloca per string var (avoids O(n) strlen in append loops).
    llvm::StringMap<llvm::AllocaInst*> stringLenCache_;
    // stringCapCache_: cached capacity alloca per string var (amortized O(1) appends).
    llvm::StringMap<llvm::AllocaInst*> stringCapCache_;

    /// Maps string literal content → LLVM global constant pointer (deduplication/pointer-eq).
    llvm::StringMap<llvm::GlobalVariable*> internedStrings_;

    /// Return the unique global pointer for a string literal (creating it if needed).
    llvm::GlobalVariable* internString(const std::string& content);

    /// Max string length for SSO (stack alloca instead of strdup; 23 = common threshold).
    static constexpr size_t kSSOMaxLen = 23;

    /// Per-variable borrow state; absent variables are implicitly Owned.
    /// Transitions: Owned→Borrowed/MutBorrowed/Frozen/Moved/Invalidated; released on scope exit.
    std::unordered_map<std::string, VarBorrowState> varBorrowStates_;

    /// Variables explicitly moved/invalidated (use-after detection; only set by move/invalidate).
    llvm::StringSet<> deadVars_;
    /// Tracks the reason a variable became dead: "moved" or "invalidated".
    std::unordered_map<std::string, std::string> deadVarReason_;

    /// Maps borrow-alias → BorrowInfo (released on scope pop).
    std::unordered_map<std::string, BorrowInfo> borrowMap_;

    /// Per-scope borrow records; released on scope pop.
    std::vector<std::vector<BorrowInfo>> borrowScopeStack_;

    /// Variables frozen via `freeze`; loads get !invariant.load, writes are errors.
    llvm::StringSet<> frozenVars_;

    /// @cold-annotated functions (preserved during cold-stripping pass).
    llvm::StringSet<> userAnnotatedColdFunctions_;

    /// Functions explicitly annotated with @hot by the user.
    llvm::StringSet<> userAnnotatedHotFunctions_;

    /// @prefetch-annotated parameters (cache invalidation emitted on return if not transferred).
    std::unordered_set<std::string> prefetchedParams_;

    /// Variables declared with `prefetch` (must be invalidated before return, else compile error).
    std::unordered_set<std::string> prefetchedVars_;

    /// Values known to be non-negative (enables urem/udiv instead of srem/sdiv).
    llvm::DenseSet<llvm::Value*> nonNegValues_;

    /// Loop-scope array length cache (shared SSA length value per array; cleared on loop entry/exit).
    llvm::DenseMap<llvm::Value*, llvm::Value*> loopArrayLenCache_;
    /// Nesting depth of loopArrayLenCache_ — pushed/popped on loop entry/exit.
    unsigned loopLenCacheDepth_ = 0;

    /// File-level @noalias: all pointer parameters are marked noalias.
    bool fileNoAlias_ = false;

    /// TBAA metadata: length (slot 0) and elements (slots 1+) don't alias,
    /// enabling LLVM to hoist length loads out of element-mutating loops.
    llvm::MDNode* tbaaRoot_ = nullptr;       ///< Root of TBAA type hierarchy
    llvm::MDNode* tbaaArrayLen_ = nullptr;   ///< TBAA access tag for array length (slot 0)
    llvm::MDNode* tbaaArrayElem_ = nullptr;  ///< TBAA access tag for array elements (slots 1+)
    llvm::MDNode* tbaaStructField_ = nullptr; ///< TBAA access tag for struct field loads/stores (generic)
    llvm::MDNode* tbaaStructTypeNode_ = nullptr; ///< TBAA type node for "struct field" (parent of per-field types)
    llvm::MDNode* tbaaStringData_ = nullptr;  ///< TBAA access tag for string character data
    llvm::MDNode* tbaaMapKey_ = nullptr;      ///< TBAA access tag for map key slots
    llvm::MDNode* tbaaMapVal_ = nullptr;      ///< TBAA access tag for map value slots
    llvm::MDNode* tbaaMapHash_ = nullptr;     ///< TBAA access tag for map hash slots
    llvm::MDNode* tbaaMapMeta_ = nullptr;     ///< TBAA access tag for map header (capacity/size)
    /// Per-field TBAA cache: unique tag per (structType, fieldIdx) to prevent field aliasing.
    std::map<std::pair<std::string, size_t>, llvm::MDNode*> tbaaStructFieldCache_;
    /// Returns (creating if needed) a per-field TBAA access tag for the given struct type and field index.
    llvm::MDNode* getOrCreateFieldTBAA(const std::string& structType, size_t fieldIdx);

    /// !range metadata for array length loads: [0, INT64_MAX).
    /// Array lengths are always non-negative (they're sizes).
    llvm::MDNode* arrayLenRangeMD_ = nullptr;

    /// !range [0,2) metadata for boolean-valued results (is_alpha, str_eq, etc).
    llvm::MDNode* boolRangeMD_ = nullptr;

    /// !range [0,256) metadata for char_at results.
    llvm::MDNode* charRangeMD_ = nullptr;

    /// !range [0,65) metadata for bit-count results (popcount, clz, ctz).
    llvm::MDNode* bitcountRangeMD_ = nullptr;

    /// Compile-time known array sizes (elides runtime length header reads).
    llvm::StringMap<llvm::Value*> knownArraySizes_;

    /// Variables declared with `prefetch immut` — their loads get invariant
    /// metadata so LLVM can hoist/CSE them aggressively.
    llvm::StringSet<> prefetchedImmutVars_;

    /// Variables declared with `register` keyword — forces register allocation
    /// by running mem2reg on the function after codegen.
    llvm::StringSet<> registerVars_;

    /// Compile-time integer values for `const` variables (enables urem/udiv fast path).
    llvm::StringMap<int64_t> constIntFolds_;

    /// Compile-time float values for `const` variables (enables constant folding).
    llvm::StringMap<double> constFloatFolds_;

    /// Compile-time string values for `const` variables (enables builtin folding).
    llvm::StringMap<std::string> constStringFolds_;

    /// Evaluate a @const_eval function at compile time (nullopt = falls back to runtime).
    std::optional<int64_t> tryConstEval(const FunctionDecl* func,
                                        const std::vector<int64_t>& argVals);

    /// Compile-time constant value (int, string, or array) for unified constant propagation.
    struct ConstValue {
        enum class Kind { Integer, String, Array } kind = Kind::Integer;
        int64_t intVal = 0;
        std::string strVal;
        std::vector<ConstValue> arrVal;  // for Kind::Array
        static ConstValue fromInt(int64_t v)     { return {Kind::Integer, v, {}, {}}; }
        static ConstValue fromStr(std::string s) { return {Kind::String, 0, std::move(s), {}}; }
        static ConstValue fromArr(std::vector<ConstValue> a)
                                                 { return {Kind::Array, 0, {}, std::move(a)}; }
    };

    /// Compile-time array values for `const` arrays (enables array index folding).
    llvm::StringMap<std::vector<ConstValue>> constArrayFolds_;

    /// Try to reduce any expression to a compile-time constant (nullopt if runtime needed).
    std::optional<ConstValue> tryFoldExprToConst(Expression* expr,
                                                 int depth = 0) const;

    /// Evaluate a function body at compile time given a fully-known argument environment.
    std::optional<ConstValue> tryConstEvalFull(
        const FunctionDecl* func,
        const std::unordered_map<std::string, ConstValue>& argEnv,
        int depth = 0) const;

    /// Overload that evaluates a BlockStmt directly (used by comptime blocks).
    std::optional<ConstValue> tryConstEvalFull(
        const BlockStmt* body,
        const std::unordered_map<std::string, ConstValue>& argEnv,
        int depth = 0) const;

    /// Fold an expression to a compile-time integer or string (more powerful than getConstantInt).
    std::optional<int64_t>     tryFoldInt(Expression* e) const;
    std::optional<std::string> tryFoldStr(Expression* e) const;

    /// Evaluate a pure built-in at compile time (~80 supported; nullopt if unknown/impure/wrong args).
    static std::optional<ConstValue> evalConstBuiltin(
        const std::string& name, const std::vector<ConstValue>& args);

    /// Emit a compile-time constant array as a private global (OmScript array layout).
    llvm::Value* emitComptimeArray(const std::vector<ConstValue>& elems);

    /// Variables with SIMD vector types for operator dispatch.
    llvm::StringSet<> simdVars_;

    /// Dict/map variable names (routes dict["key"] through map_get IR).
    llvm::StringSet<> dictVarNames_;

    /// Variables with type `ptr`/`ptr<T>` (excluded from isStringExpr).
    llvm::StringSet<> ptrVarNames_;

    /// Element type string for typed pointer variables (`ptr<T>`).
    /// Maps variable name → inner type annotation (e.g., "i64", "i32[]").
    /// Empty for untyped `ptr` variables.
    llvm::StringMap<std::string> ptrElemTypes_;

    /// Element type for borrow ref vars (`borrow var r:&T = &x`); reads auto-deref,
    /// writes write-through. Value-form borrows (`borrow var r = x`) NOT in this map.
    llvm::StringMap<std::string> refVarElemTypes_;

    /// Subset of ptrVarNames_ whose stored value is heap-allocated (malloc /
    /// alloc<T> with large/dynamic count).  `invalidate` on these emits free().
    llvm::StringSet<> heapPtrVarNames_;

    /// Maps ptr var → backing alloca for stack alloc<T>(N) (for lifetime.end on invalidate).
    llvm::StringMap<llvm::AllocaInst*> stackPtrBackingAlloca_;

    /// Scratch: backing alloca passed from generateCall to VarDecl for alloc<T> registration.
    llvm::AllocaInst* lastStackAllocBacking_ = nullptr;

    /// Per-function loop unrolling hints from @unroll / @nounroll annotations.
    bool currentFuncHintUnroll_ = false;
    bool currentFuncHintNoUnroll_ = false;
    bool currentFuncHintVectorize_ = false;
    bool currentFuncHintNoVectorize_ = false;
    bool currentFuncHintParallelize_ = false;
    bool currentFuncHintNoParallelize_ = false;
    bool currentFuncHintHot_ = false;  ///< Current function has @hot annotation
    const FunctionDecl* currentFuncDecl_ = nullptr; ///< Currently-generating function declaration
    unsigned loopNestDepth_ = 0; ///< Current for-loop nesting depth (0 = not in a loop)
    bool bodyHasInnerLoop_ = false; ///< Set when a while/for loop is found inside a for-loop body
    bool bodyHasNonPow2Modulo_ = false; ///< Set when a for-loop body has non-power-of-2 modulo
    bool bodyHasNonPow2ModuloValue_ = false; ///< Set when non-pow2 modulo result is used as a VALUE (not just in a comparison). Combined with bodyHasNonPow2Modulo_, suppresses vectorize.enable=false when true — the profitable abs/min/max vectorization outweighs the cost of vector urem.
    bool bodyHasNonPow2ModuloArrayStore_ = false; ///< Set when a non-pow2 modulo result is stored to an array element (arr[i] = expr%K). Disables forced vectorization because urem <N x i64> scalarizes on x86-64, and the extra extract/insert round-trip is slower than scalar ILP from unrolled code.
    bool inIndexAssignValueContext_ = false; ///< True while generating the VALUE expression of an IndexAssignExpr (arr[i] = VALUE). Used to detect modulo operations that produce array-element values, which enables bodyHasNonPow2ModuloArrayStore_ tracking.
    bool bodyHasBackwardArrayRef_ = false; ///< Set when a for-loop body has a backward array reference (arr[i-K] where K>0) AND the same array is written to in the same loop body. Only true loop-carried write-read dependencies should suppress parallel_accesses and LICM versioning.
    llvm::StringSet<> loopWrittenArrays_; ///< Arrays written to (via IndexAssignExpr) in the current for-loop body. Used to refine bodyHasBackwardArrayRef_ detection.
    llvm::StringSet<> loopBackwardReadArrays_; ///< Arrays read with backward references (arr[i-K]) in the current for-loop body. Combined with loopWrittenArrays_ to detect true loop-carried dependencies.
    std::unordered_set<std::string> loopIterVars_; ///< Names of all active for-loop iterators (populated unconditionally, used to detect backward array refs at any optimization level).
    bool inComparisonContext_ = false; ///< True while generating operands of == != < > <= >= (used to classify urem as "for branch" vs "for value")
    /// Per-alloca upper bound from `x % C`; emits llvm.assume(value ult C) on loads
    /// to propagate [0,C) range through PHI nodes for conditional-subtract optimization.
    llvm::DenseMap<llvm::Value*, int64_t> allocaUpperBound_;
    llvm::MDNode* currentLoopAccessGroup_ = nullptr; ///< Access group for parallel loop metadata

    // Pointer-mode loop optimization: pre-computed data pointers for arr[i] loops (no bounds check).
    llvm::StringMap<llvm::Value*> loopPtrModeDataPtrs_;
    /// Pre-loaded lengths for pointer-mode arrays (keyed by array name).
    /// Cached here so `canElideBoundsCheck` can use the same SSA value.
    llvm::StringMap<llvm::Value*> loopPtrModeLens_;
    /// Active iterator variable name for the current pointer-mode loop.
    /// Empty when not in a pointer-mode loop.  Used by generateIndex to check
    /// whether the current index expression is exactly the active iterator.
    std::string loopPtrModeIterVar_;

    [[gnu::hot]] llvm::Function* generateFunction(FunctionDecl* func);
    [[gnu::hot]] void generateStatement(Statement* stmt);
    [[gnu::hot]] llvm::Value* generateExpression(Expression* expr);

    [[gnu::hot]] llvm::Value* generateLiteral(LiteralExpr* expr);
    [[gnu::hot]] llvm::Value* generateIdentifier(IdentifierExpr* expr);
    [[gnu::hot]] llvm::Value* generateBinary(BinaryExpr* expr);
    /// Fold a chain of string literal concatenations to a compile-time constant.
    bool tryFoldStringConcat(Expression* expr, std::string& out) const;
    llvm::Value* generateUnary(UnaryExpr* expr);
    [[gnu::hot]] llvm::Value* generateCall(CallExpr* expr);
    llvm::Value* generateAssign(AssignExpr* expr);
    llvm::Value* generatePostfix(PostfixExpr* expr);
    llvm::Value* generatePrefix(PrefixExpr* expr);
    llvm::Value* generateTernary(TernaryExpr* expr);
    llvm::Value* generateArray(ArrayExpr* expr);
    llvm::Value* generateDict(DictExpr* expr);
    llvm::Value* generateIndex(IndexExpr* expr);
    llvm::Value* generateScopeResolution(ScopeResolutionExpr* expr);

    /// Returns true if the array literal for varName can be stack-allocated (no escape).
    bool canStackAllocateArray(const std::string& varName) const;

    /// Returns true if varName may escape the function (conservative: false only if proven safe).
    bool doesVarEscapeCurrentScope(const std::string& varName) const;

    /// Returns true if varName is the target of any IndexAssign in the current function body.
    bool doesVarHaveIndexAssign(const std::string& varName) const;

    /// Returns true if every use of varName is provably read-only (for RO-global optimization).
    bool doesVarHaveOnlyReadOnlyUses(const std::string& varName) const;

    /// Max array elements for stack allocation (64 × 8B = 512B, prevents stack overflow).
    static constexpr size_t kMaxStackArrayElements = 64;

    /// Track which variables hold stack-allocated arrays so that free()
    /// is not called on them and bounds-check code uses the correct base.
    llvm::StringSet<> stackAllocatedArrays_;

    /// Variables pointing to RO-global constant arrays (skip free(), writes are UB).
    llvm::StringSet<> readOnlyGlobalArrays_;

    /// Hint flag set by generateVarDecl to tell generateArray to use alloca
    /// instead of malloc for the next array allocation.
    bool pendingArrayStackAlloc_ = false;

    /// Hint: bind next array to a RO-global constant (no malloc/alloca/memcpy).
    bool pendingArrayReadOnlyGlobal_ = false;

    /// Returns true if @p expr statically resolves to a dict/map value.
    /// Used to route dict["key"] through map_get IR rather than array element IR.
    bool isDictExpr(Expression* expr) const;

    /// Emit inline map_get (equivalent to map_get(mapVal, keyVal, 0)).
    llvm::Value* emitMapGet(llvm::Value* mapVal, llvm::Value* keyVal);
    llvm::Value* generateIndexAssign(IndexAssignExpr* expr);
    llvm::Value* generateStructLiteral(StructLiteralExpr* expr);
    llvm::Value* generateFieldAccess(FieldAccessExpr* expr);
    llvm::Value* generateFieldAssign(FieldAssignExpr* expr);

    std::string resolveStructType(Expression* objExpr) const;
    size_t resolveFieldIndex(const std::string& structType, const std::string& fieldName,
                             const ASTNode* errorNode);
    /// Resolve fieldName to index, owner struct, and field LLVM type.
    struct ResolvedField {
        size_t index;
        std::string structName;
        llvm::StructType* structType;
        llvm::Type* fieldType;
        std::string fieldTypeAnnot; ///< Original annotation text (for signedness)
    };
    ResolvedField resolveField(const std::string& structHint, const std::string& fieldName,
                               const ASTNode* errorNode);
    /// Build (and cache) the LLVM StructType for a declared struct (nullptr if unknown).
    llvm::StructType* getOrCreateStructLLVMType(const std::string& name);
    /// Widen a narrow field value (sign/zero-extend integers, float→double).
    llvm::Value* liftFieldLoad(llvm::Value* v, const std::string& annot);

    void generateVarDecl(VarDecl* stmt);
    void generateGlobals(Program* program);
    void generateReturn(ReturnStmt* stmt);
    void generateIf(IfStmt* stmt);
    void generateWhile(WhileStmt* stmt);
    void generateDoWhile(DoWhileStmt* stmt);
    void generateFor(ForStmt* stmt);
    void generateForEach(ForEachStmt* stmt);
    void generateBlock(BlockStmt* stmt);
    void generateExprStmt(ExprStmt* stmt);
    void generateSwitch(SwitchStmt* stmt);
    void generateCatch(CatchStmt* stmt);
    void generateThrow(ThrowStmt* stmt);
    /// Assign (or look up) a unique compile-time integer ID for a string error code.
    int64_t getCatchStringId(const std::string& s);
    /// Pre-pass: register catch(code) BasicBlocks before generating the function body.
    void buildCatchTable(const std::vector<std::unique_ptr<Statement>>& stmts,
                         llvm::Function* fn);
    void generateInvalidate(InvalidateStmt* stmt);
    void generateMoveDecl(MoveDecl* stmt);
    void generateFreeze(FreezeStmt* stmt);
    void generatePrefetch(PrefetchStmt* stmt);
    void generateAssume(AssumeStmt* stmt);
    void generatePipeline(PipelineStmt* stmt);
    llvm::Value* generateMoveExpr(MoveExpr* expr);
    llvm::Value* generateBorrowExpr(BorrowExpr* expr);
    llvm::Value* generateReborrowExpr(ReborrowExpr* expr);

    /// Lower `@range[lo, hi] expr`: emits llvm.assume + !range metadata (pure optimization hint).
    llvm::Value* generateRangeAnnot(RangeAnnotExpr* expr);

    /// Loop fusion pre-pass: merge adjacent ForStmt pairs with matching bounds and fuse=true.
    void fuseLoops(BlockStmt* block);

    /// Mark a variable as moved: emit lifetime.end + store undef on its alloca,
    /// and record it in deadVars_ for use-after-move detection.
    void markVariableMoved(const std::string& varName);

    /// Increment immutable borrow count for srcVar, recording the alias in
    /// the current borrow scope so it is released on scope exit.
    void markVariableBorrowed(const std::string& refVar, const std::string& srcVar);

    /// Lock srcVar as mutably borrowed, recording the alias in the current
    /// borrow scope so it is released on scope exit.
    void markVariableMutBorrowed(const std::string& refVar, const std::string& srcVar);

    /// Mark a variable as frozen: emits llvm.invariant.start; all subsequent loads get !invariant.load.
    void markVariableFrozen(const std::string& varName);

    /// Release the borrow held by alias variable refVar against its source.
    /// Called by endScope when a borrow alias goes out of scope.
    void releaseBorrow(const std::string& refVar);

    /// Get the mutable borrow state for a variable, creating a default entry if absent.
    VarBorrowState& getBorrowState(const std::string& varName);
    const VarBorrowState* getBorrowStateOpt(const std::string& varName) const;

    /// Check if a variable is currently borrowed (immutably, count ≥ 1).
    bool isVariableBorrowed(const std::string& varName) const;

    /// Check if a variable is frozen (permanently immutable).
    bool isVariableFrozen(const std::string& varName) const;

    /// Validate that varName can be read at this point.
    /// Errors on: moved, invalidated, or mutably-borrowed-by-someone-else.
    void checkVariableReadable(const std::string& varName, ASTNode* site);

    /// Get the ownership state of a variable.  Returns Owned if not tracked.
    OwnershipState getOwnershipState(const std::string& varName) const;

    [[nodiscard]] llvm::Type* getDefaultType();
    [[nodiscard]] llvm::Type* getFloatType();
    /// Map type annotation ("int", "float", etc.) to LLVM type (falls back to i64).
    [[nodiscard]] llvm::Type* resolveAnnotatedType(const std::string& annotation);
    llvm::Value* toBool(llvm::Value* v);
    llvm::Value* toDefaultType(llvm::Value* v);
    /// Convert v to targetTy inserting appropriate casts (unchanged if not needed).
    llvm::Value* convertTo(llvm::Value* v, llvm::Type* targetTy);
    llvm::Value* ensureFloat(llvm::Value* v);
    /// Convert scalar v to elemTy, inserting casts as needed (returns v unchanged if no cast needed).
    llvm::Value* convertToVectorElement(llvm::Value* v, llvm::Type* elemTy);
    /// Broadcast scalar to all lanes of vecTy; inserts type conversion if element type differs.
    llvm::Value* splatScalarToVector(llvm::Value* scalar, llvm::Type* vecTy);
    void setupPrintfDeclaration();
    void initTBAAMetadata();
    llvm::Function* getPrintfFunction();
    void beginScope();
    void endScope();
    void bindVariable(const std::string& name, llvm::Value* value, bool isConst = false);
    /// Bind a variable with its type annotation (for signed/unsigned tracking).
    void bindVariableAnnotated(const std::string& name, llvm::Value* value,
                               const std::string& typeAnnot, bool isConst = false);
    /// Returns true for unsigned type annotation strings: "uint", "uN" (N=1..256).
    [[nodiscard]] static bool isUnsignedAnnot(const std::string& annot);
    /// Returns true if \p v is an unsigned integer value based on its declared type annotation.
    [[nodiscard]] bool isUnsignedValue(llvm::Value* v) const;
    void checkConstModification(const std::string& name, const std::string& action);
    void validateScopeStacksMatch(const char* location);
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* function, const std::string& name,
                                             llvm::Type* type = nullptr);
    [[noreturn]] [[gnu::cold]] void codegenError(const std::string& message, const ASTNode* node);
    void validateArgCount(const CallExpr* expr, const std::string& funcName, size_t expected);

    /// Declare a C library function with common attributes (NoUnwind, WillReturn, NoFree, NoSync).
    llvm::Function* declareExternalFn(llvm::StringRef name, llvm::FunctionType* ty);

    /// Attach mustprogress loop metadata to a back-edge branch (gated at O1+).
    void attachLoopMetadata(llvm::BranchInst* backEdgeBr);

    /// Like attachLoopMetadata but also adds vectorize.enable=1 and interleave.count at O2+.
    void attachLoopMetadataVec(llvm::BranchInst* backEdgeBr,
                               unsigned interleaveCount = 4);

    // IR emit helpers: each performs one logical operation with all required metadata attached.

    /// Load the array length header word (attaches tbaaArrayLen_ + arrayLenRangeMD_; result marked non-neg).
    llvm::Value* emitLoadArrayLen(llvm::Value* arrPtr, const llvm::Twine& name = "arrlen");

    /// Load one array element (attaches tbaaArrayElem_).
    llvm::LoadInst* emitLoadArrayElem(llvm::Value* elemPtr, const llvm::Twine& name = "arrelem");

    /// Store the array length header word (attaches tbaaArrayLen_).
    void emitStoreArrayLen(llvm::Value* len, llvm::Value* arrPtr);

    /// Store one array element aligned+tbaaArrayElem_; returns StoreInst for additional metadata.
    llvm::StoreInst* emitStoreArrayElem(llvm::Value* val, llvm::Value* elemPtr);

    /// Allocate a new array: malloc((len+1)*8), store length in slot 0, return raw buffer pointer.
    llvm::Value* emitAllocArray(llvm::Value* len, const llvm::Twine& name = "arr");

    /// Convert an i64 array handle to a typed pointer (toDefaultType + CreateIntToPtr).
    llvm::Value* emitToArrayPtr(llvm::Value* val, const llvm::Twine& name = "arrptr");

    /// Information returned by emitCountingLoop.
    struct CountingLoopInfo {
        llvm::PHINode*    idx;    ///< The induction-variable PHI node inside loopBB.
        llvm::BasicBlock* doneBB; ///< The exit block (insert point after the call returns).
    };

    /// Emit a standard counting loop: for (idx=start; idx<limit; ++idx) calling bodyFn(idx, loopBB).
    CountingLoopInfo emitCountingLoop(
        llvm::StringRef prefix,
        llvm::Value* limit,
        llvm::Value* start,
        unsigned interleaveCount,
        const std::function<void(llvm::PHINode* /*idx*/, llvm::BasicBlock* /*loopBB*/)>& bodyFn);

    /// RAII guard: calls beginScope() on construction and endScope() on destruction.
    class ScopeGuard {
      public:
        explicit ScopeGuard(CodeGenerator& cg) : cg_(cg) {
            cg_.beginScope();
        }
        ~ScopeGuard() noexcept {
            try {
                cg_.endScope();
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // Swallow exceptions to satisfy noexcept destructor contract.
            }
        }
        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;

      private:
        CodeGenerator& cg_;
    };

    // String type inference helpers.
    bool isStringExpr(Expression* expr) const;
    bool isStringArrayExpr(Expression* expr) const;
    bool isPreAnalysisStringExpr(Expression* expr, const std::unordered_set<size_t>& paramStringIndices,
                                 const FunctionDecl* func) const;
    bool scanStmtForStringReturns(Statement* stmt, const std::unordered_set<size_t>& paramStringIndices,
                                  const FunctionDecl* func) const;
    void scanStmtForStringCalls(Statement* stmt);

    // Target CPU configuration for LLVM code generation.
    std::string marchCpu_;            // -march: CPU arch for instruction selection ("" = native)
    std::string mtuneCpu_;            // -mtune: CPU for scheduling tuning ("" = same as march)
    bool usePIC_ = true;              // -fpic / -fno-pic
    bool useFastMath_ = false;        // -ffast-math / -fno-fast-math
    bool enableOptMax_ = true;        // -foptmax / -fno-optmax
    bool enableVectorize_ = true;     // -fvectorize / -fno-vectorize
    bool enableUnrollLoops_ = true;   // -funroll-loops / -fno-unroll-loops
    bool enableLoopOptimize_ = true;  // -floop-optimize / -fno-loop-optimize
    bool enableParallelize_ = true;   // -fparallelize / -fno-parallelize
    bool enableEGraph_ = true;        // -fegraph / -fno-egraph (e-graph equality saturation)
    bool enableSuperopt_ = true;      // -fsuperopt / -fno-superopt (superoptimizer)
    unsigned superoptLevel_ = 2;      // -fsuperopt-level=0/1/2/3 (default: 2)
    bool enableHGOE_ = true;          // -fhgoe / -fno-hgoe (hardware graph optimization)
    bool enableSDR_  = true;          // -fsdr / -fno-sdr (speculative devectorization & revectorization)
    bool enableIPOF_ = true;          // -fipof / -fno-ipof (implicit phase ordering fixer)
    unsigned ipofLevel_ = 0;          // 0 = auto (set from optimization level at call time)
    bool runIRPasses_ = true;         // Run runOptimizationPasses() after codegen (set false in IR unit tests).
    unsigned preferredVectorWidth_ = 4; // SIMD vector width for loop hints (target-aware)
    std::string pgoGenPath_;          // --pgo-gen=<path>: emit raw profile to this file
    std::string pgoUsePath_;          // --pgo-use=<path>: read profile data from this file
    bool lto_ = false;                // LTO mode: use pre-link pipeline
    bool verbose_ = false;            // -V: print optimization pass messages

    // DWARF debug info infrastructure
    bool debugMode_ = false;                       // -g: emit debug metadata
    std::string sourceFilename_;                   // Source file for debug CU
    std::unique_ptr<llvm::DIBuilder> debugBuilder_; // Debug info builder (null if !debugMode_)
    llvm::DICompileUnit* debugCU_ = nullptr;       // DWARF compile unit
    llvm::DIFile* debugFile_ = nullptr;            // DWARF file descriptor
    llvm::DIScope* debugScope_ = nullptr;          // Current debug scope (CU or subprogram)

    /// Compile-time resource budget (not atomic — CodeGenerator is not thread-shared).
    static constexpr size_t kMaxFunctions = 10000;
    static constexpr size_t kMaxIRInstructions = 1000000;
    size_t irInstructionCount_ = 0;

    /// Increment the IR instruction counter and abort if the budget is exceeded.
    void checkIRBudget() {
        if (++irInstructionCount_ > kMaxIRInstructions) {
            throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error,
                                             {"", 0, 0},
                                             "Compilation aborted: IR instruction limit exceeded (" +
                                                 std::to_string(kMaxIRInstructions) +
                                                 "). Input program is too large or complex."});
        }
    }

    /// Resolve effective CPU name and feature string for LLVM target machine construction.
    void resolveTargetCPU(std::string& cpu, std::string& features) const;

    /// Create a configured TargetMachine; shared by runOptimizationPasses() and writeObjectFile().
    std::unique_ptr<llvm::TargetMachine> createTargetMachine() const;

    // Lazy-declaration helpers: return existing or create new external C library function declaration.
    llvm::Function* getOrDeclareStrlen();
    llvm::Function* getOrDeclareMalloc();
    llvm::Function* getOrDeclareAlignedAlloc();
    llvm::Function* getOrDeclareCalloc();
    llvm::Function* getOrDeclareStrcpy();
    llvm::Function* getOrDeclareStrcat();
    llvm::Function* getOrDeclareStrcmp();
    llvm::Function* getOrDeclareStrncmp();
    llvm::Function* getOrDeclareMemcmp();
    llvm::Function* getOrDeclarePutchar();
    llvm::Function* getOrDeclarePuts();
    llvm::Function* getOrDeclareFputs();
    llvm::Value* getOrDeclareStdout();
    llvm::Function* getOrDeclareScanf();
    llvm::Function* getOrDeclareExit();
    llvm::Function* getOrDeclareAbort();
    llvm::Function* getOrDeclareSnprintf();
    llvm::Function* getOrDeclareMemchr();
    llvm::Function* getOrDeclareFree();
    llvm::Function* getOrDeclareStrstr();
    llvm::Function* getOrDeclareMemcpy();
    llvm::Function* getOrDeclareMemmove();
    llvm::Function* getOrDeclareToupper();
    llvm::Function* getOrDeclareTolower();
    llvm::Function* getOrDeclareIsspace();
    llvm::Function* getOrDeclareStrtoll();
    llvm::Function* getOrDeclareStrtod();
    llvm::Function* getOrDeclareStrdup();
    llvm::Function* getOrDeclareFloor();
    llvm::Function* getOrDeclareCeil();
    llvm::Function* getOrDeclareRound();
    llvm::Function* getOrDeclareQsort();
    llvm::Function* getOrDeclareRand();
    llvm::Function* getOrDeclareSrand();
    llvm::Function* getOrDeclareTimeFunc();
    llvm::Function* getOrDeclareUsleep();
    llvm::Function* getOrDeclareStrchr();
    llvm::Function* getOrDeclareStrndup();
    llvm::Function* getOrDeclareRealloc();
    llvm::Function* getOrDeclareAtoi();
    llvm::Function* getOrDeclareAtof();
    llvm::Function* getOrDeclareFwrite();
    llvm::Function* getOrDeclareFflush();
    llvm::Function* getOrDeclareFgets();
    llvm::Function* getOrDeclareFopen();
    llvm::Function* getOrDeclareFclose();
    llvm::Function* getOrDeclareFread();
    llvm::Function* getOrDeclareFseek();
    llvm::Function* getOrDeclareFtell();
    llvm::Function* getOrDeclareAccess();
    llvm::Function* getOrDeclarePthreadCreate();
    llvm::Function* getOrDeclarePthreadJoin();
    llvm::Function* getOrDeclarePthreadMutexInit();
    llvm::Function* getOrDeclarePthreadMutexLock();
    llvm::Function* getOrDeclarePthreadMutexUnlock();
    llvm::Function* getOrDeclarePthreadMutexDestroy();
    llvm::Function* getOrDeclareGetenv();
    llvm::Function* getOrDeclareSetenv();

    // BigInt runtime helpers: declare C functions from bigint_runtime.h in the LLVM module.
    llvm::Function* getOrDeclareBigintNewI64();
    llvm::Function* getOrDeclareBigintNewStr();
    llvm::Function* getOrDeclareBigintFree();
    llvm::Function* getOrDeclareBigintAdd();
    llvm::Function* getOrDeclareBigintSub();
    llvm::Function* getOrDeclareBigintMul();
    llvm::Function* getOrDeclareBigintDiv();
    llvm::Function* getOrDeclareBigintMod();
    llvm::Function* getOrDeclareBigintNeg();
    llvm::Function* getOrDeclareBigintAbs();
    llvm::Function* getOrDeclareBigintPow();
    llvm::Function* getOrDeclareBigintGcd();
    llvm::Function* getOrDeclareBigintEq();
    llvm::Function* getOrDeclareBigintLt();
    llvm::Function* getOrDeclareBigintLe();
    llvm::Function* getOrDeclareBigintGt();
    llvm::Function* getOrDeclareBigintGe();
    llvm::Function* getOrDeclareBigintCmp();
    llvm::Function* getOrDeclareBigintTostring();
    llvm::Function* getOrDeclareBigintToI64();
    llvm::Function* getOrDeclareBigintBitLength();
    llvm::Function* getOrDeclareBigintIsZero();
    llvm::Function* getOrDeclareBigintIsNegative();
    llvm::Function* getOrDeclareBigintShl();
    llvm::Function* getOrDeclareBigintShr();

    // Hash-table map runtime helpers: open-addressing, linear probing, power-of-2 capacity, FNV-1a.
    // Layout (all i64): [capacity, size, hash0, key0, val0, ...]; empty=0, tombstone=1, occupied≥2.
    llvm::Function* getOrEmitHashMapNew();
    llvm::Function* getOrEmitHashMapSet();
    llvm::Function* getOrEmitHashMapGet();
    llvm::Function* getOrEmitHashMapHas();
    llvm::Function* getOrEmitHashMapRemove();
    llvm::Function* getOrEmitHashMapKeys();
    llvm::Function* getOrEmitHashMapValues();
    llvm::Function* getOrEmitHashMapSize();

    /// Emit RA hash for a 64-bit key; low 2 bits guaranteed ≥ 2 (0=empty, 1=tombstone reserved).
    llvm::Value* emitKeyHash(llvm::Value* key);

    /// Shared inc/dec impl: returns old value (postfix) or new value (prefix).
    llvm::Value* generateIncDec(Expression* operandExpr, const std::string& op, bool isPostfix,
                                const ASTNode* errorNode);

    /// Returns true if arr[index] can provably skip the runtime bounds check (patterns A–F).
    bool canElideBoundsCheck(Expression* arrayExpr, Expression* indexExpr,
                             llvm::Value* basePtr, bool isStr,
                             const char* prefix);

    /// Emit a runtime bounds check (compare+branch+abort); insertion point is success block on return.
    void emitBoundsCheck(llvm::Value* idxVal, llvm::Value* basePtr,
                         bool isStr, bool isBorrowed, const char* prefix, int line = 0);

    /// Pre-scan loop body: collect arrays accessed ONLY as arr[iterVar] with no length-modifying calls.
    /// Map value: true = written in body, false = read-only (both eligible for pointer-mode opt).
    std::unordered_map<std::string, bool> preScanLoopArrayAccesses(
        const Statement* body, const std::string& iterVar);

    // Optimization methods
    void runOptimizationPasses();
    void optimizeOptMaxFunctions();

    /// Created at generate() start; populated by Orchestrator pre-passes; used for O(1) IR-emit queries.
    std::unique_ptr<OptimizationContext> optCtx_;

    /// Shared between OptimizationOrchestrator and runOptimizationPasses() (same cost model/legality).
    std::unique_ptr<OptimizationManager> optMgr_;

    /// CF-CTRE engine: initialised once in generateProgram(); provides cross-function CT eval/memoisation.
    std::unique_ptr<CTEngine> ctEngine_;

    /// Last CT result from COMPTIME_EXPR; consumed by VarDecl handler to register in folds maps.
    std::optional<CTValue> lastComptimeCtResult_;

    /// Per-function CT-known integers (NOT used by generateIdentifier to avoid interfering with mutable vars).
    /// Only consulted by buildComptimeEnv(); cleared at function start and on assignment.
    llvm::StringMap<int64_t> scopeComptimeInts_;

    ConstValue ctValueToConstValue(const CTValue& v) const;
    CTValue constValueToCTValue(const ConstValue& v) const;

    /// Build CT env from current scope's known locals for ctEngine_->evalComptimeBlock().
    std::unordered_map<std::string, CTValue> buildComptimeEnv() const;

  public:
    // Per-function optimization for targeted optimization of individual functions
    void optimizeFunction(llvm::Function* func);
};

} // namespace omscript

#endif // CODEGEN_H
