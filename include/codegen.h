#pragma once

#ifndef CODEGEN_H
#define CODEGEN_H

/// @file codegen.h
/// @brief LLVM IR code generation for OmScript.
///
/// This module defines the CodeGenerator class which walks the OmScript AST
/// and emits LLVM IR.  It handles type mapping, control flow, built-in
/// functions, debug information, and optimization attributes.

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

// Forward declaration avoids including the full TargetMachine header,
// reducing compilation dependencies for translation units that only
// need the CodeGenerator interface (e.g. compiler.cpp, main.cpp).
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
///
/// Every variable tracked by the ownership system transitions through
/// these states:
///
///  Owned → Borrowed (×N) → (back to Owned when all borrows end)
///  Owned → MutBorrowed → (back to Owned when mut borrow ends)
///  Owned → Frozen → (permanent; stays Frozen for rest of lifetime)
///  Owned → Moved → (variable is dead, use-after-move error)
///  Owned/Borrowed/Moved → Invalidated → (explicitly killed)
///
/// Rules:
///  - Owned: full control — may read, write, move, borrow, or invalidate.
///  - Borrowed (immutBorrowCount > 0): multiple concurrent read-only aliases
///    are allowed; source may NOT be written or mutably borrowed.
///  - MutBorrowed: exactly one mutable alias — source is completely locked;
///    no reads, writes, or new borrows until the mutable borrow ends.
///  - Frozen: permanently immutable — reads are !invariant.load, writes
///    are compile-time errors.  Cannot be moved or mut-borrowed.
///  - Moved: ownership transferred — any use is a compile-time error.
///  - Invalidated: explicitly killed — any use is a compile-time error.
enum class OwnershipState {
    Owned,        ///< Variable owns its value — full read/write access
    Borrowed,     ///< Has ≥1 immutable borrows — readable but not writable
    MutBorrowed,  ///< Has one mutable alias — source is completely locked
    Frozen,       ///< Permanently immutable — all loads are invariant
    Moved,        ///< Ownership transferred out — use is a compile error
    Invalidated   ///< Explicitly killed — use is a compile error
};

/// Per-variable borrow state tracked by the ownership system.
/// Replaces the separate StringSet approach with precise count-based tracking.
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
    /// Derive the canonical OwnershipState (for compatibility with callers
    /// that still consume OwnershipState).
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
    /// The linker (gcc/clang with -flto) reads bitcode and performs whole-program
    /// optimization across translation units at link time.
    void writeBitcodeFile(const std::string& filename);
    [[nodiscard]] llvm::Module* getModule() noexcept {
        return module.get();
    }
    void setOptimizationLevel(OptimizationLevel level) {
        optimizationLevel = level;
    }

    /// Set the target CPU architecture for instruction selection.
    /// Use "native" or "" for host auto-detection (default).
    void setMarch(const std::string& cpu) {
        marchCpu_ = cpu;
    }

    /// Set the CPU model for scheduling/micro-architecture tuning.
    /// Defaults to the same value as -march when empty.
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

    /// Set the superoptimizer aggressiveness level (0-3).
    ///   0 = disabled (same as -fno-superopt)
    ///   1 = light: idiom recognition + algebraic only (fast compilation)
    ///   2 = normal: all features, default synthesis (default)
    ///   3 = aggressive: all features, expanded synthesis (slower compilation)
    void setSuperoptLevel(unsigned level) {
        superoptLevel_ = level;
        enableSuperopt_ = (level > 0);
    }

    /// Enable or disable the Hardware Graph Optimization Engine (default: true).
    /// HGOE activates only when -march or -mtune flags are provided; this flag
    /// allows explicitly disabling it even when those flags are present.
    void setHardwareGraphOpt(bool enable) {
        enableHGOE_ = enable;
    }

    /// Enable or disable the post-codegen LLVM IR optimization pipeline
    /// (runOptimizationPasses()).  Default: true.  White-box unit tests that
    /// inspect the raw IR emitted by the CodeGenerator (metadata, nuw/nsw
    /// flags, load-bound annotations, loop-back-edge md) set this to false
    /// so LLVM's own O1+ passes do not eliminate the very patterns the test
    /// is verifying.
    void setRunIRPasses(bool enable) {
        runIRPasses_ = enable;
    }

    /// Enable PGO instrumentation generation mode.
    /// When set, the AOT-compiled binary will write a raw profile (.profraw)
    /// to @p profilePath at program exit, capturing branch and call counts.
    void setPGOGen(const std::string& profilePath) {
        pgoGenPath_ = profilePath;
    }

    /// Enable PGO profile-guided optimization use mode.
    /// When set, the optimizer reads the .profdata file at @p profilePath
    /// and uses its branch/call counts to improve inlining, branch layout,
    /// and hot-path specialization decisions.
    void setPGOUse(const std::string& profilePath) {
        pgoUsePath_ = profilePath;
    }

    /// Enable LTO pre-link optimization pipeline.
    /// When true, runOptimizationPasses() uses buildLTOPreLinkDefaultPipeline()
    /// instead of buildPerModuleDefaultPipeline(), deferring heavy IPO to the
    /// linker so that the bitcode is not double-optimized.
    void setLTO(bool enable) {
        lto_ = enable;
    }

    /// Enable or disable DWARF debug info generation.
    /// When true, the CodeGenerator emits debug metadata (compile unit,
    /// subprograms) so that compiled binaries can be debugged with GDB/LLDB.
    void setDebugMode(bool enable) {
        debugMode_ = enable;
    }

    /// Enable or disable verbose output during code generation.
    /// When true, the CodeGenerator prints messages about which optimization
    /// passes are running and their results.
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

    // ── Unified optimization context ──────────────────────────────────────
    //
    // Provides access to all analysis facts accumulated during the pre-pass
    // sequence.  Populated by the OptimizationOrchestrator before IR emission
    // begins; queries are valid from the start of IR codegen onward.

    /// Return the unified optimization context (non-null after generate()).
    [[nodiscard]] const OptimizationContext* optimizationContext() const noexcept {
        return optCtx_.get();
    }

    // ── Analysis result accessors (used by OptimizationOrchestrator) ──────
    // These delegate to OptimizationContext (the single authoritative store).

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

    // ── Pre-pass entry points (also called by OptimizationOrchestrator) ───
    //
    // These are declared public so the orchestrator can sequence them
    // externally without requiring friend access.  They were previously
    // private implementation details; elevating them to the public surface
    // is intentional: they are stable, well-defined analysis passes that
    // external tooling may wish to invoke selectively (e.g. for incremental
    // recompilation, testing, or IDE integration).

    void preAnalyzeStringTypes(Program* program);
    void preAnalyzeArrayTypes(Program* program);
    void analyzeConstantReturnValues(Program* program);
    void autoDetectConstEvalFunctions(Program* program);
    void inferFunctionEffects(Program* program);

    /// Conditional wrapper for the e-graph pre-pass.  Checks the optimization
    /// level and enableEGraph_ flag, then configures ctx.egraph() based on
    /// the current level and calls ctx.egraph().optimizeProgram().
    /// Called by the Orchestrator which owns the OptimizationContext.
    void runEGraphPass(Program* program, OptimizationContext& ctx);

    /// Public-facing wrapper for runSynthesisPass that the Orchestrator calls.
    /// Delegates to the free function declared in synthesize.h.
    void runSynthesisPass(Program* program, bool verbose);

    /// Public-facing wrapper for the CF-CTRE pre-pass (was private).
    void runCFCTRE(Program* program);

    /// Run the Region Lifetime Coalescing pass (AST-level transformation).
    /// Detects invalidate-based region lifetime patterns and coalesces
    /// temporally disjoint regions to eliminate redundant allocations.
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

    /// Variables whose index is provably within bounds of a specific array
    /// at compile time.  Populated during for-loop codegen for patterns like
    ///   for (i in 0...len(arr)) { arr[i] ... }
    /// or similar provably-safe patterns.  Cleared when exiting the loop.
    /// Maps: iterator-variable-name → set of array-variable-names that are safe.
    llvm::StringSet<> safeIndexVars_;

    /// Maps iterator variable name → its LLVM upper bound value.
    /// Used to emit llvm.assume hints for the optimizer.
    llvm::StringMap<llvm::Value*> loopIterEndBound_;

    /// Maps iterator variable name → its LLVM start bound value.
    /// Used for negative-offset bounds check elision: in patterns like
    /// for (i in C...n) { arr[i - K] }, knowing start >= K proves i-K >= 0.
    llvm::StringMap<llvm::Value*> loopIterStartBound_;

    /// Maps iterator variable name → the array variable name whose len()
    /// was used as the for-loop end bound (for(i in 0...len(arr))).
    /// Enables zero-cost bounds check elision for arr[i] inside such loops:
    /// the loop condition i < len(arr) already proves the index is valid.
    llvm::StringMap<std::string> loopIterEndArray_;

    /// LLVM global variable registry: name → GlobalVariable*.
    /// Populated by generateGlobals() and used by generateIdentifier() /
    /// generateScopeResolution() to resolve global variable references.
    llvm::StringMap<llvm::GlobalVariable*> globalVars_;

    /// Maps array variable name → the AllocaInst of the variable passed as
    /// size to array_fill(sizeVar, val).  Enables bounds check elision when
    /// the same variable is used as both the array size and the loop end bound
    /// (e.g. var arr = array_fill(n, 0); for (i in 0...n) { arr[i] }).
    /// Works for any runtime size, including values from input().
    llvm::StringMap<llvm::AllocaInst*> knownArraySizeAllocas_;

    // Per-function catch table: maps error-code (i64) → the BasicBlock for that
    // handler.  String error codes are assigned a unique compile-time integer
    // (via catchStringIds_).  Populated by a pre-pass before each function is
    // compiled, and reset at the start of every function.
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

    /// Maps variable name → its declared type annotation (e.g. "u8", "i32", "i128").
    /// Empty string means the type is untyped (default: i64 semantics).
    /// Populated by bindVariableAnnotated() when a type annotation is known.
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
    // Per-struct LLVM StructType — built lazily from structFieldDecls_/structDefs_,
    // with each field's LLVM type derived from its annotated typeName via
    // resolveAnnotatedType (empty annotation → i64).  Using a real StructType
    // (instead of an [N x i64] array) lets LLVM compute DataLayout-correct
    // field offsets, alignment, and total size, and enables SROA/mem2reg to
    // promote small structs to SSA registers.
    std::unordered_map<std::string, llvm::StructType*> structLLVMTypes_;

    // Operator overload registry: maps "StructName::op" → generated LLVM function name.
    // e.g. "Vec2::+" → "__op_Vec2_add"
    std::unordered_map<std::string, std::string> operatorOverloads_;

    OptimizationLevel optimizationLevel;

    // String type tracking across function boundaries.
    // stringVars_: names of variables/parameters that hold string values in the
    //   current function scope (pointer-typed values stored as i64).
    // stringReturningFunctions_: functions known to return a string value.
    // funcParamStringTypes_: maps function name to the set of parameter indices
    //   that are expected to receive string arguments.
    // stringArrayVars_: names of variables that hold arrays whose elements are
    //   string pointers (e.g. declared with ["a","b"] or assigned from str_split).
    //   Used by isStringExpr(IndexExpr) and generateForEach to propagate string
    //   type information through array element accesses.
    llvm::StringSet<> stringVars_;
    llvm::StringSet<> stringReturningFunctions_;
    std::unordered_map<std::string, std::unordered_set<size_t>> funcParamStringTypes_;
    llvm::StringSet<> stringArrayVars_;
    // arrayVars_: names of variables that hold array values (non-string pointers).
    // Used to disambiguate pointer-typed allocas: since arrays, structs, and
    // dicts now use pointer-typed allocas (instead of i64), we need to
    // distinguish them from string pointers in isStringExpr().
    llvm::StringSet<> arrayVars_;
    // Whole-program array type information (mirrors string type system):
    // arrayReturningFunctions_: functions whose return type is an array type.
    // funcParamArrayTypes_: maps function name → set of parameter indices that
    //   receive array arguments (annotated with [] suffix or known from call sites).
    // These are populated by preAnalyzeArrayTypes() and used to:
    //   (a) keep arrayVars_ accurate for variables assigned from function calls,
    //   (b) ensure isStringExpr() returns false for array-typed call results.
    llvm::StringSet<> arrayReturningFunctions_;
    std::unordered_map<std::string, std::unordered_set<size_t>> funcParamArrayTypes_;
    // stringLenCache_: maps string variable names to an alloca that caches the
    // current strlen of the variable's value.  Used by str_concat to avoid
    // O(n) strlen calls on growing strings in append loops.
    llvm::StringMap<llvm::AllocaInst*> stringLenCache_;
    // stringCapCache_: maps string variable names to an alloca that caches the
    // allocated buffer capacity.  Used by str_concat to skip realloc calls
    // when the existing buffer has enough space (amortized O(1) appends).
    llvm::StringMap<llvm::AllocaInst*> stringCapCache_;

    // ── String Interning Pool ──────────────────────────────────────
    /// Maps string literal content → LLVM global constant pointer.
    /// Enables pointer-equality comparison for interned strings and
    /// deduplicates identical string literals across the entire module.
    llvm::StringMap<llvm::GlobalVariable*> internedStrings_;

    /// Intern a string literal: return the unique global pointer for the
    /// given string content.  Creates a new global if this content has
    /// not been seen before; otherwise returns the existing one.
    llvm::GlobalVariable* internString(const std::string& content);

    /// Maximum string length for Small String Optimization (SSO).
    /// Strings ≤ this length are stack-allocated via alloca+memcpy
    /// instead of heap-allocated via strdup.  23 bytes matches the
    /// common SSO threshold (24-byte struct with 1 byte for NUL/flags).
    static constexpr size_t kSSOMaxLen = 23;

    /// Ownership lattice: tracks the ownership state of each variable.
    ///
    /// Only populated for variables that participate in ownership annotations
    /// (move, invalidate, borrow).  Variables not in this map are implicitly
    /// Owned with full read/write access.
    ///
    /// Per-variable borrow state.  Only populated for variables that participate
    /// in the ownership system; variables absent from this map are implicitly
    /// fully Owned.
    ///
    /// Transitions:
    ///   Owned → immutBorrowCount++ (via `borrow ref = x`)
    ///   Owned → mutBorrowed = true (via `borrow mut ref = x`)
    ///   Any   → moved / invalidated (via move / invalidate)
    ///   Any   → frozen = true (via `freeze x;`)
    ///   borrow release: immutBorrowCount-- or mutBorrowed = false (on scope exit)
    std::unordered_map<std::string, VarBorrowState> varBorrowStates_;

    /// Variables that have been explicitly moved or invalidated.
    /// Used to detect use-after-move and use-after-invalidate at compile time.
    /// Only populated when the user writes `move` or `invalidate` — normal
    /// code without ownership annotations is never affected.
    llvm::StringSet<> deadVars_;
    /// Tracks the reason a variable became dead: "moved" or "invalidated".
    std::unordered_map<std::string, std::string> deadVarReason_;

    /// Maps borrow-alias variable names to information about the borrow:
    ///   borrowMap_["b"] = {"a", false}   // `borrow b = a;`
    ///   borrowMap_["b"] = {"a", true}    // `borrow mut b = a;`
    /// Used when a scope pops to release borrows held by aliases going OOS.
    std::unordered_map<std::string, BorrowInfo> borrowMap_;

    /// Scope-indexed borrow tracking.  Each scope level stores the list of
    /// BorrowInfo records introduced in that scope.  On scope pop every
    /// borrow in the scope is released (decrement or clear the source's
    /// VarBorrowState).
    std::vector<std::vector<BorrowInfo>> borrowScopeStack_;

    /// Variables frozen via `freeze x;` — immutable for the rest of their
    /// lifetime.  Loads become !invariant.load; writes are compile errors.
    /// Kept as a separate set for fast O(1) lookup in generateIdentifier.
    llvm::StringSet<> frozenVars_;

    /// Functions explicitly annotated with @cold by the user.
    /// These are preserved when the post-pipeline cold-stripping pass runs.
    llvm::StringSet<> userAnnotatedColdFunctions_;

    /// Functions explicitly annotated with @hot by the user.
    llvm::StringSet<> userAnnotatedHotFunctions_;

    /// Parameters annotated with @prefetch in the current function.
    /// Tracks parameter names that were prefetched at function entry so that
    /// the return statement codegen can emit cache invalidation for parameters
    /// whose memory was not transferred out (returned).
    std::unordered_set<std::string> prefetchedParams_;

    /// Variables declared with `prefetch` statement in the current function.
    /// Tracks variable names that must be explicitly invalidated before the
    /// function returns.  A compile-time error is emitted if any prefetched
    /// variable is not found in deadVars_ at return time.
    std::unordered_set<std::string> prefetchedVars_;

    /// Values known to be non-negative at codegen time.  Populated when
    /// ascending for-loop counters are loaded and when binary operations
    /// on non-negative operands produce non-negative results.  Used to
    /// emit urem/udiv instead of srem/sdiv for modulo/division by positive
    /// constants, which the vectorizer then preserves as vector urem/udiv.
    llvm::DenseSet<llvm::Value*> nonNegValues_;

    /// Loop-scope array length cache: maps array base pointer → loaded length
    /// value within the current loop body.  When multiple array accesses in the
    /// same loop body use the same array, the length load is shared instead of
    /// re-loaded from memory on every bounds check.  TBAA already tells LLVM
    /// that length and element slots don't alias, but LLVM's GVN/LICM may not
    /// always succeed when the loads are in different control-flow paths (each
    /// behind a bounds-check branch).  This cache short-circuits that by
    /// re-using the SSA value directly.
    /// Cleared on loop entry/exit to avoid stale values.
    llvm::DenseMap<llvm::Value*, llvm::Value*> loopArrayLenCache_;
    /// Nesting depth of loopArrayLenCache_ — pushed/popped on loop entry/exit.
    unsigned loopLenCacheDepth_ = 0;

    /// File-level @noalias: all pointer parameters are marked noalias.
    bool fileNoAlias_ = false;

    /// TBAA (Type-Based Alias Analysis) metadata hierarchy.
    /// OmScript arrays store length in slot 0 and elements in slots 1+.
    /// TBAA tells LLVM that length loads can never alias element loads/stores,
    /// enabling hoisting of length loads out of element-mutating loops.
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
    /// Per-field TBAA access tag cache: maps (structTypeName, fieldIndex) → access tag.
    /// Each struct field gets a unique TBAA type node that is a child of tbaaStructTypeNode_,
    /// so accesses to different fields of the same (or different) struct types do not alias.
    std::map<std::pair<std::string, size_t>, llvm::MDNode*> tbaaStructFieldCache_;
    /// Returns (creating if needed) a per-field TBAA access tag for the given struct type and field index.
    llvm::MDNode* getOrCreateFieldTBAA(const std::string& structType, size_t fieldIdx);

    /// !range metadata for array length loads: [0, INT64_MAX).
    /// Array lengths are always non-negative (they're sizes).
    llvm::MDNode* arrayLenRangeMD_ = nullptr;

    /// !range metadata for boolean-valued i64 results (0 or 1):
    /// is_alpha, is_digit, str_eq, str_contains, str_starts_with,
    /// str_ends_with, array_contains.  Allows CVP/LVI/InstCombine to
    /// fold comparisons like (is_alpha(c) == 1) → (is_alpha(c) != 0).
    llvm::MDNode* boolRangeMD_ = nullptr;

    /// !range metadata for char-valued i64 results (0..255):
    /// char_at.  Allows CVP/LVI to prove non-negativity and drop any
    /// >255 branches.
    llvm::MDNode* charRangeMD_ = nullptr;

    /// !range metadata for bit-count results (0..64):
    /// popcount, clz, ctz.  These always return a value in [0, 64].
    /// More precise than just nonNeg tracking — tells CVP/LVI the exact
    /// upper bound so it can fold comparisons like (clz(x) > 64) → false.
    llvm::MDNode* bitcountRangeMD_ = nullptr;

    /// Compile-time known array sizes: maps variable name → LLVM Value*
    /// representing the known element count.  Populated when an array is
    /// created via array_fill(N, val) where N is a compile-time constant
    /// or a tracked variable.  Used to elide bounds checks without reading
    /// the length header at runtime.
    llvm::StringMap<llvm::Value*> knownArraySizes_;

    /// Variables declared with `prefetch immut` — their loads get invariant
    /// metadata so LLVM can hoist/CSE them aggressively.
    llvm::StringSet<> prefetchedImmutVars_;

    /// Variables declared with `register` keyword — forces register allocation
    /// by running mem2reg on the function after codegen.
    llvm::StringSet<> registerVars_;

    /// Constant integer values for `const` integer variables initialized with
    /// a compile-time constant.  Used to substitute constants directly in
    /// division/modulo expressions (e.g. `x % sz` where `sz` is a `const`
    /// variable with value 10000), enabling the urem/udiv fast path and
    /// avoiding the slow dynamic-divisor branch with zero-check overhead.
    llvm::StringMap<int64_t> constIntFolds_;

    /// Constant float values for `const` float variables initialized with
    /// a compile-time constant.  Enables compile-time evaluation of float
    /// arithmetic chains: `const PI = 3.14159; var x = PI * 2.0;` folds
    /// to 6.28318 at compile time, eliminating runtime fmul.
    llvm::StringMap<double> constFloatFolds_;

    /// Constant string values for `const` string variables initialized with
    /// a compile-time string literal.  Enables compile-time evaluation of
    /// string builtins: `const s = "hello"; var n = len(s);` folds to 5.
    llvm::StringMap<std::string> constStringFolds_;

    /// Evaluate a @const_eval function at compile time.
    /// Returns std::nullopt if the function body is too complex for the
    /// compile-time interpreter (falls back to runtime codegen).
    std::optional<int64_t> tryConstEval(const FunctionDecl* func,
                                        const std::vector<int64_t>& argVals);

    /// A compile-time constant value — either a 64-bit integer or a string.
    /// Used by tryFoldExprToConst and tryConstEvalFull for unified int+string
    /// constant propagation.
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

    /// Constant array values for `const` or comptime array variables whose
    /// elements are all compile-time constants.  Enables folding array indexing:
    /// `const arr = [10, 20, 30]; var x = arr[1];` folds to 20.
    llvm::StringMap<std::vector<ConstValue>> constArrayFolds_;

    /// tryFoldExprToConst: attempt to reduce any expression to a compile-time
    /// constant using all currently available compile-time information:
    ///   - integer / string / array literals
    ///   - identifiers tracked in constIntFolds_ or constStringFolds_
    ///   - enum constants and scope-resolution expressions
    ///   - zero-arg calls to zero-parameter constant-returning functions
    ///     (queried from OptimizationContext)
    ///   - recursive evaluation of multi-arg user functions via tryConstEvalFull
    ///   - arithmetic / concat / array indexing on any of the above
    ///   - pipe expressions (x |> f → f(x))
    ///   - comptime {} blocks
    ///   - all builtins supported by evalConstBuiltin (~80 pure functions)
    /// Returns nullopt for any expression that requires runtime information.
    std::optional<ConstValue> tryFoldExprToConst(Expression* expr,
                                                 int depth = 0) const;

    /// tryConstEvalFull: evaluate a function body at compile time given a
    /// fully-known argument environment (maps param names → ConstValues).
    /// Handles all common statement/expression forms including:
    ///   - const and non-const VarDecls (both tracked in the local env)
    ///   - assignments, compound assignments (desugared by parser)
    ///   - index assignment (arr[i] = val) with array const tracking
    ///   - if/else, for-range, while, do-while, foreach, switch, break, continue
    ///   - blocks with proper scope save/restore for shadowed variables
    ///   - 30+ builtins (arithmetic, string, bitwise, etc.)
    ///   - recursive calls to any user function with all-const args
    ///   - fuel limit (10 000 loop iterations) prevents runaway evaluation
    /// Returns nullopt if any step requires runtime information (I/O,
    /// loops with dynamic bounds, calls to non-foldable functions, etc.).
    std::optional<ConstValue> tryConstEvalFull(
        const FunctionDecl* func,
        const std::unordered_map<std::string, ConstValue>& argEnv,
        int depth = 0) const;

    /// Overload that evaluates a BlockStmt directly (used by comptime blocks).
    std::optional<ConstValue> tryConstEvalFull(
        const BlockStmt* body,
        const std::unordered_map<std::string, ConstValue>& argEnv,
        int depth = 0) const;

    /// Convenience wrappers used by generateBuiltin: fold an expression to a
    /// compile-time integer or string using all currently available information
    /// (const variables, enum constants, binary ops on constants, etc.).
    /// More powerful than the file-local getConstantInt() helper which only
    /// recognises plain integer literals.
    std::optional<int64_t>     tryFoldInt(Expression* e) const;
    std::optional<std::string> tryFoldStr(Expression* e) const;

    /// Apply a named pure built-in function to a list of already-evaluated
    /// compile-time ConstValues.  Returns nullopt if the builtin is unknown,
    /// impure, or the argument types/count are wrong.  Used by both
    /// tryFoldExprToConst and tryConstEvalFull so the fold logic is defined
    /// in exactly one place.
    ///
    /// Supported builtins (~80):
    ///   Math: abs, min, max, sign, clamp, pow, sqrt, gcd, lcm, log2, exp2
    ///   Bitwise: popcount, clz, ctz, bitreverse, bswap, rotate_left/right
    ///   Saturating: saturating_add, saturating_sub
    ///   Char: is_alpha, is_digit, is_upper, is_lower, is_space, is_alnum,
    ///         is_even, is_odd, to_char, char_code
    ///   String: len, str_len, str_eq, str_concat, str_find, str_substr,
    ///           str_upper, str_lower, str_contains, str_index_of, str_replace,
    ///           str_trim, str_starts_with, str_ends_with, str_repeat,
    ///           str_reverse, str_count, str_pad_left, str_pad_right,
    ///           str_to_int, to_string, to_int, number_to_string, string_to_number
    ///   Array: array_fill, array_concat, array_slice, array_contains, index_of,
    ///          array_min, array_max, array_last, array_product, sum, reverse,
    ///          sort, array_remove, array_insert, array_any, array_every,
    ///          array_count
    ///   Float math: sin, cos, tan, asin, acos, atan, atan2, cbrt, hypot,
    ///               fma, copysign, min_float, max_float
    ///   Fast/precise: fast_add/sub/mul/div, precise_add/sub/mul/div
    ///   Casts: u64, i64, int, uint, u32, i32, u16, i16, u8, i8, bool
    static std::optional<ConstValue> evalConstBuiltin(
        const std::string& name, const std::vector<ConstValue>& args);

    /// Emit a compile-time constant integer array as a private global with
    /// OmScript's `[length, elem0, …, elemN-1]` layout and return the base
    /// pointer as an i64 (OmScript's uniform array representation).
    /// Used by the comptime block emitter and the call-site constant folder.
    llvm::Value* emitComptimeArray(const std::vector<ConstValue>& elems);

    /// Variables with SIMD vector types for operator dispatch.
    llvm::StringSet<> simdVars_;

    /// Variables that hold dict/map values (created via dict literal, map_new,
    /// map_set, map_remove, or declared with type "dict").  Used to route
    /// dict["key"] index expressions through map_get IR instead of array IR.
    llvm::StringSet<> dictVarNames_;

    /// Variables declared with type `ptr` or `ptr<T>`.  Used to exclude them
    /// from isStringExpr() — pointer-typed allocas would otherwise be
    /// misidentified as string variables and routed through strlen/strcat paths.
    llvm::StringSet<> ptrVarNames_;

    /// Element type string for typed pointer variables (`ptr<T>`).
    /// Maps variable name → inner type annotation (e.g., "i64", "i32[]").
    /// Empty for untyped `ptr` variables.
    llvm::StringMap<std::string> ptrElemTypes_;

    /// Subset of ptrVarNames_ whose stored value is heap-allocated (malloc /
    /// alloc<T> with large/dynamic count).  `invalidate` on these emits free().
    llvm::StringSet<> heapPtrVarNames_;

    /// Maps ptr variable name → the backing AllocaInst for stack-allocated
    /// alloc<T>(N) pointers, so that `invalidate` can emit lifetime.end on the
    /// actual storage (not just the pointer variable's own alloca slot).
    llvm::StringMap<llvm::AllocaInst*> stackPtrBackingAlloca_;

    /// Scratch field: set by generateCall for alloc<T> to pass the backing
    /// AllocaInst to the enclosing VarDecl codegen so it can be registered in
    /// stackPtrBackingAlloca_.  Reset to nullptr after each use.
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
    /// Per-alloca exclusive upper bounds from modular arithmetic.
    /// When a variable is assigned `x % C` (a urem with constant C), we record
    /// C here so that subsequent loads emit `llvm.assume(value ult C)`.  This
    /// propagates the tight range [0, C) through loop PHI nodes via LLVM's
    /// LazyValueInfo, enabling the conditional-subtract optimisation
    /// (select(s<C, s, s-C)) to fire for ALL unrolled iterations — not just
    /// the ones where the divisor is immediately visible.
    llvm::DenseMap<llvm::Value*, int64_t> allocaUpperBound_;
    llvm::MDNode* currentLoopAccessGroup_ = nullptr; ///< Access group for parallel loop metadata

    // ── Range-to-pointer-arithmetic state ─────────────────────────────────────
    // When a `for i in start..end` loop is entered and the loop body pre-scan
    // (`preScanLoopArrayAccesses`) finds arrays accessed purely as `arr[i]`,
    // we pre-compute the data pointer for each such array BEFORE the loop.
    // Inside the loop body, `generateIndex`/`generateIndexAssign` use these
    // cached data pointers directly (inbounds GEP, no bounds check).
    //
    // Scoping: like `loopArrayLenCache_`, these maps are saved/restored on
    // loop entry/exit (see generateForStatement).  They are cleared when the
    // loop body has been fully generated so inner loops don't see stale data.
    //
    // Safety: arrays are only entered if ALL accesses in the body are of the
    // form `arr[iterVar]` (simple direct index — no push/pop, no arr[expr]).
    // For arrays that ARE written (arr[i] = ...), the data pointer is still
    // valid because writing elements does not change the array's base pointer
    // or length header.
    //
    // Key: array variable name.  Value: pre-computed data pointer (i64*, the
    // pointer to arr[0], i.e., `base + 1`).
    llvm::StringMap<llvm::Value*> loopPtrModeDataPtrs_;
    /// Pre-loaded lengths for pointer-mode arrays (keyed by array name).
    /// Cached here so `canElideBoundsCheck` can use the same SSA value.
    llvm::StringMap<llvm::Value*> loopPtrModeLens_;
    /// Active iterator variable name for the current pointer-mode loop.
    /// Empty when not in a pointer-mode loop.  Used by generateIndex to check
    /// whether the current index expression is exactly the active iterator.
    std::string loopPtrModeIterVar_;

    // Code generation methods
    [[gnu::hot]] llvm::Function* generateFunction(FunctionDecl* func);
    [[gnu::hot]] void generateStatement(Statement* stmt);
    [[gnu::hot]] llvm::Value* generateExpression(Expression* expr);

    // Expression generators
    [[gnu::hot]] llvm::Value* generateLiteral(LiteralExpr* expr);
    [[gnu::hot]] llvm::Value* generateIdentifier(IdentifierExpr* expr);
    [[gnu::hot]] llvm::Value* generateBinary(BinaryExpr* expr);
    /// Recursively check if an expression tree is a chain of string literal
    /// concatenations (e.g. "a" + "b" + "c").  If so, append the folded result
    /// to @p out and return true.  Otherwise return false and leave @p out
    /// unchanged.  This enables compile-time folding of arbitrarily deep
    /// chained string concatenations.
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

    // ── Array Escape Analysis ──────────────────────────────────────
    /// Check whether an array literal assigned to @p varName can be
    /// stack-allocated (does not escape the current function scope).
    /// Returns true if the array is safe for alloca (no escape).
    bool canStackAllocateArray(const std::string& varName) const;

    /// Returns true if the variable @p varName may escape the current function
    /// body (used in return, call args, or global store).  Conservative: only
    /// returns false when we can PROVE there is no escape.
    bool doesVarEscapeCurrentScope(const std::string& varName) const;

    /// Returns true if the variable @p varName is the target of any
    /// `varName[i] = ...` IndexAssign anywhere in the current function body
    /// (recursing into nested blocks / if / while / for).  Conservative: if
    /// the function body is unavailable, returns true.
    bool doesVarHaveIndexAssign(const std::string& varName) const;

    /// Returns true if every use of @p varName in the current function body
    /// is provably read-only.  Allowed uses are:
    ///   * `varName[i]` — IndexExpr read
    ///   * `len(varName)` and other non-mutating built-in calls
    ///   * Argument to a user function known-pure to the CTEngine
    /// Disallowed uses (return false):
    ///   * IndexAssign target on varName
    ///   * Return varName / use in returned expression
    ///   * Assignment of varName to another variable (creates alias)
    ///   * Argument to a callee that may mutate, may unwind into mutating
    ///     code, or whose effect we cannot prove is read-only
    /// Used by the read-only-global array literal optimization.
    bool doesVarHaveOnlyReadOnlyUses(const std::string& varName) const;

    /// Maximum number of array elements for stack allocation (prevents
    /// stack overflow from large arrays — 64 elements × 8 bytes = 512 B).
    static constexpr size_t kMaxStackArrayElements = 64;

    /// Track which variables hold stack-allocated arrays so that free()
    /// is not called on them and bounds-check code uses the correct base.
    llvm::StringSet<> stackAllocatedArrays_;

    /// Track which variables hold a pointer into a read-only global constant
    /// array (no allocation, no copy: PtrToInt of @arr.ro.const).  These must
    /// also be skipped from free() and any potential write would be UB; the
    /// pre-pass that sets this guarantees no IndexAssign and no escape.
    llvm::StringSet<> readOnlyGlobalArrays_;

    /// Hint flag set by generateVarDecl to tell generateArray to use alloca
    /// instead of malloc for the next array allocation.
    bool pendingArrayStackAlloc_ = false;

    /// Hint flag set by generateVarDecl to tell generateArray to bind the
    /// declared variable directly to a private read-only global constant
    /// (no malloc, no alloca, no memcpy).  Mutually exclusive with
    /// pendingArrayStackAlloc_; the pre-pass picks at most one.
    bool pendingArrayReadOnlyGlobal_ = false;

    /// Returns true if @p expr statically resolves to a dict/map value.
    /// Used to route dict["key"] through map_get IR rather than array element IR.
    bool isDictExpr(Expression* expr) const;

    /// Emit an inline map_get loop that looks up @p keyVal in the map whose
    /// i64 pointer is @p mapVal.  Returns the associated value or 0 if absent.
    /// Equivalent to map_get(mapVal, keyVal, 0) but emitted inline.
    llvm::Value* emitMapGet(llvm::Value* mapVal, llvm::Value* keyVal);
    llvm::Value* generateIndexAssign(IndexAssignExpr* expr);
    llvm::Value* generateStructLiteral(StructLiteralExpr* expr);
    llvm::Value* generateFieldAccess(FieldAccessExpr* expr);
    llvm::Value* generateFieldAssign(FieldAssignExpr* expr);

    // Struct type resolution helpers.
    std::string resolveStructType(Expression* objExpr) const;
    size_t resolveFieldIndex(const std::string& structType, const std::string& fieldName,
                             const ASTNode* errorNode);
    /// Resolve `fieldName` to its index, the owning struct's name, and the
    /// LLVM element type of that field.  When `structHint` is empty and the
    /// field is uniquely owned by one struct, the owner is auto-discovered
    /// (matching the behaviour of resolveFieldIndex).
    struct ResolvedField {
        size_t index;
        std::string structName;
        llvm::StructType* structType;
        llvm::Type* fieldType;
        std::string fieldTypeAnnot; ///< Original annotation text (for signedness)
    };
    ResolvedField resolveField(const std::string& structHint, const std::string& fieldName,
                               const ASTNode* errorNode);
    /// Build (and cache) the LLVM StructType for a declared OmScript struct.
    /// Field LLVM types come from each StructField::typeName via
    /// resolveAnnotatedType (empty annotation → i64, preserving the legacy
    /// uniform-i64 layout for untyped fields).  Uses a non-packed struct so
    /// LLVM applies natural alignment per the target DataLayout.  Returns
    /// nullptr if the struct name is unknown.
    llvm::StructType* getOrCreateStructLLVMType(const std::string& name);
    /// Lift a freshly-loaded struct field value (or any narrow scalar) to a
    /// type that the rest of the expression engine expects to consume:
    /// integers narrower than the default width are sign- (or zero-) extended,
    /// `float` is widened to `double`.  All other types pass through unchanged.
    llvm::Value* liftFieldLoad(llvm::Value* v, const std::string& annot);

    // Statement generators
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
    /// Pre-pass: scan a function body, register all catch(code) blocks and
    /// create their BasicBlocks.  Must be called before generating the body.
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

    /// Lower `@range[lo, hi] expr` (RangeAnnotExpr).  Compile-time fails
    /// when `expr` folds to an integer outside [lo, hi].  Otherwise emits
    /// `llvm.assume(val >= lo && val <= hi)`, attaches `!range !{lo, hi+1}`
    /// metadata to load/call results, and (when `lo >= 0`) records the
    /// value in `nonNegValues_` so later passes can skip non-negativity
    /// guards.  Pure hint: never affects correctness, only optimization.
    llvm::Value* generateRangeAnnot(RangeAnnotExpr* expr);

    /// Loop fusion pre-pass: walk a BlockStmt's statement list and merge
    /// adjacent ForStmt pairs where both/either has loopHints.fuse=true,
    /// and they share the same start/end bounds.
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

    /// Mark a variable as frozen: immutable for the rest of its lifetime.
    /// Emits llvm.invariant.start and marks constValues[name]=true so all
    /// subsequent loads get !invariant.load and writes are rejected.
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

    // Helper methods
    [[nodiscard]] llvm::Type* getDefaultType();
    [[nodiscard]] llvm::Type* getFloatType();
    /// Map a type annotation string ("int", "float", "string", etc.) to the
    /// corresponding LLVM type.  Unknown or empty annotations fall back to
    /// getDefaultType() (i64).
    [[nodiscard]] llvm::Type* resolveAnnotatedType(const std::string& annotation);
    llvm::Value* toBool(llvm::Value* v);
    llvm::Value* toDefaultType(llvm::Value* v);
    /// Convert \p v to \p targetTy, inserting appropriate casts (FPToSI,
    /// SIToFP, PtrToInt, IntToPtr, etc.) as needed.  Returns \p v unchanged
    /// when no conversion is required.
    llvm::Value* convertTo(llvm::Value* v, llvm::Type* targetTy);
    llvm::Value* ensureFloat(llvm::Value* v);
    /// Convert a scalar \p v to the element type of \p elemTy, inserting
    /// appropriate casts (FPTrunc, SIToFP, FPToSI, IntCast) as needed.
    /// Returns \p v unchanged when no conversion is required.
    llvm::Value* convertToVectorElement(llvm::Value* v, llvm::Type* elemTy);
    /// Broadcast a scalar \p scalar to all lanes of vector type \p vecTy.
    /// Inserts type conversion if the scalar type differs from the vector
    /// element type, then uses insertelement + shufflevector for the splat.
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

    /// Declare a C library function with common attributes.
    /// Most runtime functions share {NoUnwind, WillReturn, NoFree, NoSync} —
    /// this helper avoids repeating those four addFnAttr calls.
    /// Returns the newly created Function so callers can add extra attributes.
    llvm::Function* declareExternalFn(llvm::StringRef name, llvm::FunctionType* ty);

    /// Attach mustprogress loop metadata to a back-edge branch instruction.
    /// Gated behind optimizationLevel >= O1.  Consolidates the 6-line
    /// metadata construction pattern used by 50+ builtin and statement loops.
    void attachLoopMetadata(llvm::BranchInst* backEdgeBr);

    /// Like attachLoopMetadata but also adds vectorize.enable=1 and
    /// interleave.count=interleaveCount when optimizationLevel >= O2.
    /// interleaveCount=0 means omit the interleave hint (only vectorize.enable).
    /// This collapses the ~25 duplicated 12-line loop-metadata blocks in
    /// codegen_builtins.cpp and codegen_expr.cpp into single-line calls.
    void attachLoopMetadataVec(llvm::BranchInst* backEdgeBr,
                               unsigned interleaveCount = 4);

    // ── IR emit helpers — eliminate the 3-4 line TBAA/malloc patterns ────────
    // Each helper performs a single logical operation (load-len, store-elem,
    // allocate array, etc.) and attaches all required metadata in one call.
    // This removes ~200 duplicated metadata-attachment sequences scattered
    // across codegen_builtins.cpp, codegen_expr.cpp and codegen_stmt.cpp.

    /// Load the length word from an OmScript array header.
    /// Emits a MaybeAlign(8) load, attaches tbaaArrayLen_ + arrayLenRangeMD_,
    /// and marks the result non-negative in nonNegValues_.
    llvm::Value* emitLoadArrayLen(llvm::Value* arrPtr, const llvm::Twine& name = "arrlen");

    /// Load one element from an OmScript array body (slot index already adjusted).
    /// Emits a MaybeAlign(8) load and attaches tbaaArrayElem_.
    llvm::LoadInst* emitLoadArrayElem(llvm::Value* elemPtr, const llvm::Twine& name = "arrelem");

    /// Store the length word into an OmScript array header.
    /// Attaches tbaaArrayLen_ to the resulting StoreInst.
    void emitStoreArrayLen(llvm::Value* len, llvm::Value* arrPtr);

    /// Store one element into an OmScript array body (slot index already adjusted).
    /// Emits a MaybeAlign(8) aligned store and attaches tbaaArrayElem_.
    /// Returns the StoreInst so callers can attach additional metadata (e.g. access_group).
    llvm::StoreInst* emitStoreArrayElem(llvm::Value* val, llvm::Value* elemPtr);

    /// Allocate a new OmScript array with `len` elements.
    /// Computes bytes = (len+1)*8, calls malloc, attaches dereferenceable(8),
    /// stores the length in slot 0 with tbaaArrayLen_, and returns the raw
    /// buffer pointer (i8*/ptr type, NOT yet converted to i64).
    llvm::Value* emitAllocArray(llvm::Value* len, const llvm::Twine& name = "arr");

    /// Convert an i64 OmScript array handle to a typed pointer.
    /// Calls toDefaultType(val) then CreateIntToPtr.  The common
    /// "val = generateExpression(); val = toDefaultType(val); ptr = intToPtr(val)"
    /// 3-liner is reduced to a single call.
    llvm::Value* emitToArrayPtr(llvm::Value* val, const llvm::Twine& name = "arrptr");

    /// Information returned by emitCountingLoop.
    struct CountingLoopInfo {
        llvm::PHINode*    idx;    ///< The induction-variable PHI node inside loopBB.
        llvm::BasicBlock* doneBB; ///< The exit block (insert point after the call returns).
    };

    /// Emit a standard index-counting loop:
    ///   for (idx = start; idx < limit; ++idx) { bodyFn(idx, bodyBB); }
    ///
    /// Creates three basic blocks: prefix.loop, prefix.body, prefix.done.
    /// Jumps from the current insert point into prefix.loop, sets up a PHI
    /// for the induction variable, checks idx < limit (unsigned), and calls
    /// bodyFn(idxPHI, bodyBB) to generate the loop body.  The caller is
    /// responsible for incrementing the index and branching back — or, more
    /// commonly, calling emitCountingLoopTail() to do so.
    ///
    /// Usage:
    ///   auto [idx, doneBB] = emitCountingLoop("upper", strLen,
    ///       zero, 4, [&](llvm::Value* i, llvm::BasicBlock* bodyBB) {
    ///           // generate body using i
    ///       });
    ///   builder->SetInsertPoint(doneBB);
    ///
    /// Parameters:
    ///   prefix        — used as the BasicBlock name prefix and PHI name prefix.
    ///   limit         — the exclusive upper bound (loop runs while idx < limit).
    ///   start         — initial value of idx (usually zero).
    ///   interleaveCount — passed to attachLoopMetadataVec; 0 = plain mustprogress.
    ///   bodyFn        — called with (idxPHI, bodyBB); should generate the body
    ///                   AND close the iteration (increment + branch back to loopBB).
    ///                   The function is called with the insert point in bodyBB.
    CountingLoopInfo emitCountingLoop(
        llvm::StringRef prefix,
        llvm::Value* limit,
        llvm::Value* start,
        unsigned interleaveCount,
        const std::function<void(llvm::PHINode* /*idx*/, llvm::BasicBlock* /*loopBB*/)>& bodyFn);

    /// RAII guard that calls beginScope() on construction and endScope()
    /// on destruction, ensuring scope stacks are always balanced even
    /// when exceptions interrupt code generation.
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
    // isStringExpr: returns true if the given AST expression is known to
    //   produce a string value at the current codegen point (uses namedValues
    //   and stringVars_ for identifier lookups).
    bool isStringExpr(Expression* expr) const;
    // isStringArrayExpr: returns true if the expression is known to be an array
    //   whose elements are string pointers (uses stringArrayVars_ lookup).
    bool isStringArrayExpr(Expression* expr) const;
    // isPreAnalysisStringExpr: lightweight AST-only string check used by the
    //   pre-analysis (no access to namedValues; uses stringReturningFunctions_
    //   and paramStringIndices to track string parameters).
    bool isPreAnalysisStringExpr(Expression* expr, const std::unordered_set<size_t>& paramStringIndices,
                                 const FunctionDecl* func) const;
    // scanStmtForStringReturns: returns true if any return statement in the
    //   given statement subtree returns a string expression.
    bool scanStmtForStringReturns(Statement* stmt, const std::unordered_set<size_t>& paramStringIndices,
                                  const FunctionDecl* func) const;
    // scanStmtForStringCalls: walks a statement subtree and records which
    //   function parameters receive string arguments at call sites.
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
    bool runIRPasses_ = true;         // Run runOptimizationPasses() after codegen.
                                       // Unit tests that inspect the raw IR
                                       // emitted by the CodeGenerator (metadata,
                                       // nuw/nsw flags, loop-back-edge md, etc.)
                                       // set this to false so that LLVM's own
                                       // passes do not eliminate the patterns
                                       // the tests are verifying.
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

    /// Compile-time resource budget — limits to prevent DoS via oversized inputs.
    /// Checked during code generation to abort compilation if the program
    /// exceeds reasonable complexity bounds.
    /// Note: not atomic — CodeGenerator instances are not shared across threads.
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

    /// Resolve the effective CPU name and feature string for LLVM target machine
    /// construction based on the current -march / -mtune settings.
    void resolveTargetCPU(std::string& cpu, std::string& features) const;

    /// Create a configured TargetMachine for the current target triple and
    /// CPU settings.  Shared by runOptimizationPasses() and writeObjectFile()
    /// to eliminate duplicated setup code.
    std::unique_ptr<llvm::TargetMachine> createTargetMachine() const;

    // Lazy-declaration helpers for C library functions.  Each method returns
    // the existing declaration if one has already been added to the module,
    // or creates a new external declaration on first use.  This removes
    // duplicated getFunction()/Create() blocks that were scattered across
    // multiple built-in handlers.
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

    // ── BigInt runtime helpers ────────────────────────────────────────────────
    // These declare the C functions from bigint_runtime.h in the LLVM module.
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

    // ── Hash-table map runtime helpers (emitted into the LLVM module) ────
    // These implement an open-addressing hash table with linear probing,
    // power-of-2 capacity, and FNV-1a hashing.  Each helper is emitted once
    // per module as an internal function (InternalLinkage) with appropriate
    // attributes for inlining at O2+.
    //
    // Hash table layout (all i64):
    //   [capacity, size, hash0, key0, val0, hash1, key1, val1, ...]
    //   Total allocation: (2 + 3 * capacity) * 8 bytes
    //   Empty slot: hash == 0
    //   Tombstone:  hash == 1
    //   Occupied:   hash >= 2 (actual hash OR'd with 2)
    llvm::Function* getOrEmitHashMapNew();
    llvm::Function* getOrEmitHashMapSet();
    llvm::Function* getOrEmitHashMapGet();
    llvm::Function* getOrEmitHashMapHas();
    llvm::Function* getOrEmitHashMapRemove();
    llvm::Function* getOrEmitHashMapKeys();
    llvm::Function* getOrEmitHashMapValues();
    llvm::Function* getOrEmitHashMapSize();

    /// Emit the Rotate-Accumulate (RA) hash for a 64-bit integer key.
    /// Returns a hash value with the low two bits guaranteed >= 2
    /// (0=empty, 1=tombstone are reserved).  Only 4 IR instructions:
    /// mul, fshr (ror), add, or.
    llvm::Value* emitKeyHash(llvm::Value* key);

    /// Shared implementation for prefix and postfix increment/decrement.
    /// Returns the *old* value for postfix (isPostfix=true) and the *new*
    /// value for prefix (isPostfix=false).
    llvm::Value* generateIncDec(Expression* operandExpr, const std::string& op, bool isPostfix,
                                const ASTNode* errorNode);

    /// Shared bounds check elision analysis.
    ///
    /// Determines whether an array index operation arr[index] can provably
    /// skip the runtime bounds check.  Consolidates all elision patterns:
    ///   A) for(i in 0...len(arr)) { arr[i] }
    ///   B) array_fill(n,...) + for(i in 0...n) { arr[i] }
    ///   C) Known compile-time array sizes with constant loop bounds
    ///   D) SSA value equality (endBound == lenVal)
    ///   E) Compile-time constant comparison (endConst <= lenConst)
    ///   F) Arithmetic patterns: arr[i + K] and arr[i - K]
    ///
    /// @param arrayExpr  The array sub-expression (for identifier checks)
    /// @param indexExpr  The index sub-expression (for iterator/arithmetic checks)
    /// @param basePtr    The LLVM pointer to the array base (for length loads)
    /// @param isStr      True if the value is a string (skips array-specific checks)
    /// @param prefix     Name prefix for emitted IR instructions (e.g. "idx", "idxa", "incdec")
    /// @return true if the bounds check can be safely elided
    bool canElideBoundsCheck(Expression* arrayExpr, Expression* indexExpr,
                             llvm::Value* basePtr, bool isStr,
                             const char* prefix);

    /// Emit a runtime bounds check for an array/string index operation.
    /// Generates the compare + branch + abort pattern, placing the insertion
    /// point at the success block on return.
    ///
    /// @param idxVal   The index value to check
    /// @param basePtr  The base pointer to load length from
    /// @param isStr    True for string access (uses strlen instead of header load)
    /// @param isBorrowed True if the array is borrowed (length marked invariant)
    /// @param prefix   Name prefix for emitted IR instructions
    /// @param line     Source line number for the runtime error message (0 = unknown)
    void emitBoundsCheck(llvm::Value* idxVal, llvm::Value* basePtr,
                         bool isStr, bool isBorrowed, const char* prefix, int line = 0);

    // ── Range-to-pointer-arithmetic helpers ──────────────────────────────────

    /// Pre-scan the AST of a for-loop body to collect arrays that are accessed
    /// ONLY as `arr[iterVar]` (direct index, exactly the loop iterator) and
    /// have no length-modifying calls (push/pop/array_remove/array_insert) in
    /// the body.  Arrays that also appear as `arr[expr]` (non-trivial index)
    /// or in length-modifying calls are excluded — those arrays continue to
    /// receive normal bounds checks.
    ///
    /// The returned map keys are array variable names.  The bool value is
    /// `true` if the array is also written (IndexAssignExpr) in the loop body,
    /// and `false` if it is only read.  Writing elements does NOT change the
    /// base pointer or length header, so both read-only and written arrays are
    /// eligible for pointer-mode optimization.
    ///
    /// @param body     Root statement of the loop body
    /// @param iterVar  Name of the loop iterator variable
    /// @return  Map from array name → is_written.  Empty if no eligible arrays.
    std::unordered_map<std::string, bool> preScanLoopArrayAccesses(
        const Statement* body, const std::string& iterVar);

    // Optimization methods
    void runOptimizationPasses();
    void optimizeOptMaxFunctions();

    // ── Unified optimization context (owned by CodeGenerator) ─────────────
    /// Created at the start of generate() and populated by the Orchestrator
    /// as pre-passes complete.  Used for fast O(1) queries during IR emission.
    std::unique_ptr<OptimizationContext> optCtx_;

    // ── Shared optimization manager ────────────────────────────────────────
    /// Created at the start of generate() and shared between the AST-level
    /// OptimizationOrchestrator and the IR-level runOptimizationPasses() so
    /// that both layers use the same cost model and legality service.
    std::unique_ptr<OptimizationManager> optMgr_;

    // ── CF-CTRE integration ────────────────────────────────────────────────
    /// Shared CF-CTRE engine.  Initialised once in generateProgram() before
    /// any function body is compiled.  Provides cross-function compile-time
    /// evaluation, memoisation, purity analysis, and pipeline SIMD semantics.
    std::unique_ptr<CTEngine> ctEngine_;

    /// Stash for the CTValue produced by the most-recently evaluated
    /// COMPTIME_EXPR.  Set inside generateExpr(COMPTIME_EXPR) right after a
    /// successful ctEngine_->evalComptimeBlock() call, then consumed
    /// (and cleared) by the VarDecl handler immediately after generateExpr
    /// returns.  This lets the VarDecl handler register the CT result in
    /// constArrayFolds_/constIntFolds_ under the variable's name so that
    /// subsequent comptime blocks in the same function can reference it.
    std::optional<CTValue> lastComptimeCtResult_;

    /// Per-function map of variable names → their compile-time-known integer
    /// values, populated by the VarDecl handler for variables whose
    /// non-comptime initializer was constant-folded to a scalar (e.g.
    /// 'var reduced = lane_reduce(derived)' where derived is CT-known).
    /// Unlike constIntFolds_, entries here are NOT used by generateIdentifier
    /// to replace loads with constants, so they do not interfere with mutable
    /// loop variables or compound assignments.  They are only consulted by
    /// buildComptimeEnv() to populate the env passed to evalComptimeBlock().
    /// Cleared at the start of each function and on assignment (generateAssign).
    llvm::StringMap<int64_t> scopeComptimeInts_;

    /// Convert a CTValue produced by ctEngine_ to the CodeGenerator's internal
    /// ConstValue representation (used for bridging to existing fold helpers).
    ConstValue ctValueToConstValue(const CTValue& v) const;

    /// Convert a ConstValue to a CTValue and load it into ctEngine_.
    CTValue constValueToCTValue(const ConstValue& v) const;

    /// Build a CTValue environment from all compile-time-known local variables
    /// in the current scope (constIntFolds_, constStringFolds_,
    /// constArrayFolds_).  Passed as the 'env' argument to
    /// ctEngine_->evalComptimeBlock() so that comptime{} blocks can reference
    /// variables declared earlier in the same function body.
    std::unordered_map<std::string, CTValue> buildComptimeEnv() const;

  public:
    // Per-function optimization for targeted optimization of individual functions
    void optimizeFunction(llvm::Function* func);
};

} // namespace omscript

#endif // CODEGEN_H
