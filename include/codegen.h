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
#include "diagnostic.h"
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
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

/// Ownership lattice states for compile-time memory safety.
///
/// Every variable tracked by the ownership system transitions through
/// these states:
///
///  Owned → Borrowed → (back to Owned when borrow ends)
///  Owned → Moved → (variable is dead, use-after-move error)
///  Owned/Borrowed/Moved → Invalidated → (explicitly killed)
///
/// Rules:
///  - Owned: full control — may read, write, move, borrow, or invalidate.
///  - Borrowed: read-only alias — may NOT mutate, move, or invalidate.
///  - Moved: ownership transferred — any use is a compile-time error.
///  - Invalidated: explicitly killed — any use is a compile-time error.
enum class OwnershipState {
    Owned,       ///< Variable owns its value — full read/write access
    Borrowed,    ///< Variable is borrowed — read-only, cannot resize/move
    Moved,       ///< Ownership transferred out — use is a compile error
    Invalidated  ///< Explicitly killed — use is a compile error
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

  private:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::Module> module;

    llvm::StringMap<llvm::Value*> namedValues;
    std::vector<std::unordered_map<std::string, llvm::Value*>> scopeStack;

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

    /// Maps array variable name → the AllocaInst of the variable passed as
    /// size to array_fill(sizeVar, val).  Enables bounds check elision when
    /// the same variable is used as both the array size and the loop end bound
    /// (e.g. var arr = array_fill(n, 0); for (i in 0...n) { arr[i] }).
    /// Works for any runtime size, including values from input().
    llvm::StringMap<llvm::AllocaInst*> knownArraySizeAllocas_;

    // Stack of innermost catch-entry basic blocks, pushed/popped by
    // generateTryCatch(). generateThrow() branches directly to the top of this
    // stack when a throw occurs inside a try block, ensuring that control flow
    // reaches the catch handler immediately (rather than relying on a post-loop
    // flag check that could allow dangerous code to execute after the throw).
    std::vector<llvm::BasicBlock*> tryCatchStack_;
    bool inOptMaxFunction;
    bool hasOptMaxFunctions;
    llvm::StringSet<> optMaxFunctions;

    struct ConstBinding {
        bool wasPreviouslyDefined;
        bool previousIsConst;
        // Previous constIntFolds_ value for this variable (if any).
        bool hadPreviousIntFold = false;
        int64_t previousIntFold = 0;
    };
    llvm::StringMap<bool> constValues;
    std::vector<std::unordered_map<std::string, ConstBinding>> constScopeStack;
    llvm::StringMap<llvm::Function*> functions;

    // Store AST function declarations for default parameter lookup at call sites.
    llvm::StringMap<const FunctionDecl*> functionDecls_;

    // Enum constant values (name → integer value), populated from enum declarations.
    llvm::StringMap<long long> enumConstants_;

    // Struct type definitions: struct name → ordered list of field names.
    std::unordered_map<std::string, std::vector<std::string>> structDefs_;
    // Rich struct field metadata: struct name → ordered list of StructField with attributes.
    std::unordered_map<std::string, std::vector<StructField>> structFieldDecls_;
    // Variables known to hold struct values, maps var name → struct type name.
    std::unordered_map<std::string, std::string> structVars_;

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
    // stringLenCache_: maps string variable names to an alloca that caches the
    // current strlen of the variable's value.  Used by str_concat to avoid
    // O(n) strlen calls on growing strings in append loops.
    llvm::StringMap<llvm::AllocaInst*> stringLenCache_;
    // stringCapCache_: maps string variable names to an alloca that caches the
    // allocated buffer capacity.  Used by str_concat to skip realloc calls
    // when the existing buffer has enough space (amortized O(1) appends).
    llvm::StringMap<llvm::AllocaInst*> stringCapCache_;

    /// Ownership lattice: tracks the ownership state of each variable.
    ///
    /// Only populated for variables that participate in ownership annotations
    /// (move, invalidate, borrow).  Variables not in this map are implicitly
    /// Owned with full read/write access.
    ///
    /// Transitions:
    ///   Owned → Borrowed (via borrow expression)
    ///   Owned → Moved (via move expression)
    ///   Any → Invalidated (via invalidate statement)
    ///   Borrowed → Owned (when borrow scope ends)
    std::unordered_map<std::string, OwnershipState> varOwnership_;

    /// Variables that have been explicitly moved or invalidated.
    /// Used to detect use-after-move and use-after-invalidate at compile time.
    /// Only populated when the user writes `move` or `invalidate` — normal
    /// code without ownership annotations is never affected.
    llvm::StringSet<> deadVars_;
    /// Tracks the reason a variable became dead: "moved" or "invalidated".
    std::unordered_map<std::string, std::string> deadVarReason_;

    /// Variables currently borrowed — these cannot be mutated or moved.
    /// Populated when a borrow expression creates an alias; cleared when
    /// the borrowing variable goes out of scope.
    llvm::StringSet<> borrowedVars_;

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
    llvm::MDNode* tbaaStructField_ = nullptr; ///< TBAA access tag for struct field loads/stores
    llvm::MDNode* tbaaStringData_ = nullptr;  ///< TBAA access tag for string character data
    llvm::MDNode* tbaaMapKey_ = nullptr;      ///< TBAA access tag for map key slots
    llvm::MDNode* tbaaMapVal_ = nullptr;      ///< TBAA access tag for map value slots
    llvm::MDNode* tbaaMapHash_ = nullptr;     ///< TBAA access tag for map hash slots
    llvm::MDNode* tbaaMapMeta_ = nullptr;     ///< TBAA access tag for map header (capacity/size)

    /// !range metadata for array length loads: [0, INT64_MAX).
    /// Array lengths are always non-negative (they're sizes).
    llvm::MDNode* arrayLenRangeMD_ = nullptr;

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

    /// Variables with SIMD vector types for operator dispatch.
    llvm::StringSet<> simdVars_;

    /// Variables that hold dict/map values (created via dict literal, map_new,
    /// map_set, map_remove, or declared with type "dict").  Used to route
    /// dict["key"] index expressions through map_get IR instead of array IR.
    llvm::StringSet<> dictVarNames_;

    /// Per-function loop unrolling hints from @unroll / @nounroll annotations.
    bool currentFuncHintUnroll_ = false;
    bool currentFuncHintNoUnroll_ = false;
    bool currentFuncHintVectorize_ = false;
    bool currentFuncHintNoVectorize_ = false;
    bool currentFuncHintParallelize_ = false;
    bool currentFuncHintNoParallelize_ = false;
    bool currentFuncHintHot_ = false;  ///< Current function has @hot annotation
    unsigned loopNestDepth_ = 0; ///< Current for-loop nesting depth (0 = not in a loop)
    bool bodyHasInnerLoop_ = false; ///< Set when a while/for loop is found inside a for-loop body
    bool bodyHasNonPow2Modulo_ = false; ///< Set when a for-loop body has non-power-of-2 modulo
    bool bodyHasNonPow2ModuloValue_ = false; ///< Set when non-pow2 modulo result is used as a VALUE (not just in a comparison). Combined with bodyHasNonPow2Modulo_, suppresses vectorize.enable=false when true — the profitable abs/min/max vectorization outweighs the cost of vector urem.
    bool bodyHasNonPow2ModuloArrayStore_ = false; ///< Set when a non-pow2 modulo result is stored to an array element (arr[i] = expr%K). Disables forced vectorization because urem <N x i64> scalarizes on x86-64, and the extra extract/insert round-trip is slower than scalar ILP from unrolled code.
    bool inIndexAssignValueContext_ = false; ///< True while generating the VALUE expression of an IndexAssignExpr (arr[i] = VALUE). Used to detect modulo operations that produce array-element values, which enables bodyHasNonPow2ModuloArrayStore_ tracking.
    bool bodyHasBackwardArrayRef_ = false; ///< Set when a for-loop body has a backward array reference (arr[i-K] where K>0). Suppresses parallel_accesses metadata — such loops have loop-carried dependencies that prevent LLVM from promoting arr[i-1] to a register accumulator.
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

    // Statement generators
    void generateVarDecl(VarDecl* stmt);
    void generateReturn(ReturnStmt* stmt);
    void generateIf(IfStmt* stmt);
    void generateWhile(WhileStmt* stmt);
    void generateDoWhile(DoWhileStmt* stmt);
    void generateFor(ForStmt* stmt);
    void generateForEach(ForEachStmt* stmt);
    void generateBlock(BlockStmt* stmt);
    void generateExprStmt(ExprStmt* stmt);
    void generateSwitch(SwitchStmt* stmt);
    void generateTryCatch(TryCatchStmt* stmt);
    void generateThrow(ThrowStmt* stmt);
    void generateInvalidate(InvalidateStmt* stmt);
    void generateMoveDecl(MoveDecl* stmt);
    void generatePrefetch(PrefetchStmt* stmt);
    llvm::Value* generateMoveExpr(MoveExpr* expr);
    llvm::Value* generateBorrowExpr(BorrowExpr* expr);

    /// Mark a variable as moved: emit lifetime.end + store undef on its alloca,
    /// and record it in deadVars_ for use-after-move detection.
    void markVariableMoved(const std::string& varName);

    /// Mark a variable as borrowed: records it in the ownership lattice so
    /// that mutations and moves are rejected at compile time.
    void markVariableBorrowed(const std::string& varName);

    /// Check if a variable is currently borrowed (read-only).
    bool isVariableBorrowed(const std::string& varName) const;

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
    // preAnalyzeStringTypes: iterative pre-pass over the full program AST to
    //   populate stringReturningFunctions_ and funcParamStringTypes_ before
    //   any function body is generated.
    void preAnalyzeStringTypes(Program* program);
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
    void emitBoundsCheck(llvm::Value* idxVal, llvm::Value* basePtr,
                         bool isStr, bool isBorrowed, const char* prefix);

    // Optimization methods
    void runOptimizationPasses();
    void optimizeOptMaxFunctions();

  public:
    // Per-function optimization for targeted optimization of individual functions
    void optimizeFunction(llvm::Function* func);
};

} // namespace omscript

#endif // CODEGEN_H
