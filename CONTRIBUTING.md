# Contributing to OmScript

Thank you for considering contributing to OmScript! This document covers everything you need to know to work on the OmScript compiler — from setting up your environment to deeply understanding the compiler internals, adding new language features, writing tests, and submitting pull requests.

---

## Table of Contents

1. [Development Setup](#1-development-setup)
2. [Architecture Overview](#2-architecture-overview)
3. [Source File Reference](#3-source-file-reference)
4. [Adding a New Builtin Function](#4-adding-a-new-builtin-function)
5. [Adding a New Statement Form](#5-adding-a-new-statement-form)
6. [Adding a New Expression Operator](#6-adding-a-new-expression-operator)
7. [TBAA Annotation Guide](#7-tbaa-annotation-guide)
8. [Constant Folding Guide](#8-constant-folding-guide)
9. [Comptime Evaluation Guide](#9-comptime-evaluation-guide)
10. [Test Writing Guide](#10-test-writing-guide)
11. [Code Style Guidelines](#11-code-style-guidelines)
12. [Pull Request Checklist](#12-pull-request-checklist)

---

## 1. Development Setup

### Prerequisites
- CMake 3.13+ (3.16+ recommended for precompiled header support)
- C++17 compiler (GCC ≥ 9 or Clang ≥ 10)
- LLVM 18 development libraries (`llvm-18-dev`, `libclang-18-dev`)
- Google Test (`libgtest-dev`)
- Optional: `ccache` or `sccache` for fast incremental builds

### Basic Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Release Build with LLVM

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DLLVM_DIR=$(llvm-config-18 --cmakedir)
make -j$(nproc)
```

### Faster Builds

The build system automatically uses **ccache** or **sccache** when available, and enables **precompiled headers** (PCH) on CMake 3.16+ to avoid reparsing heavy LLVM headers in every translation unit.

```bash
# Install ccache for faster incremental rebuilds
sudo apt-get install -y ccache        # Debian/Ubuntu
brew install ccache                    # macOS

# First build compiles the PCH (~15s), subsequent builds reuse it
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-18/cmake
make -j$(nproc)
```

### Build Variants

```bash
# LTO build (smaller, faster binary)
cmake .. -DCMAKE_BUILD_TYPE=Release -DLTO=ON
make -j$(nproc)

# PGO — Phase 1: instrument
cmake .. -DPGO_GENERATE=ON -DPGO_PROFILE_DIR=./pgo-data
make -j$(nproc)
./omsc run ../../examples/fibonacci.om
./omsc run ../../examples/sorting.om
# ... run representative workloads ...

# PGO — Phase 2: optimize with profile
cmake .. -DPGO_USE=./pgo-data
make -j$(nproc)

# Sanitizer build (for debugging crashes and UB)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address,undefined
cmake --build build --parallel $(nproc)
```

### Running Tests

```bash
# Unit tests (GTest via CTest)
cd build && ctest --output-on-failure

# Integration tests (full CLI + language suite)
bash run_tests.sh

# Run a single example manually
./build/omsc run examples/fibonacci.om
```

### Coverage Workflow

```bash
# Coverage-instrumented build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCOVERAGE=ON -DLLVM_DIR=/usr/lib/llvm-18/cmake
cmake --build build --parallel $(nproc)

# Execute all tests
cd build && ctest --output-on-failure && cd ..
bash run_tests.sh

# Coverage summary for production sources
gcovr -r . --filter 'src/' --filter 'runtime/' --exclude 'build/' --print-summary
```

---

## 2. Architecture Overview

OmScript is a multi-pass compiler that takes source code from `.om` files to native machine code via LLVM IR. The pipeline in order:

```
Source (.om files)
    │
    ├─ 1. Preprocessor (compiler.cpp)
    │       Macro expansion (#define, #ifdef), conditional compilation,
    │       #include/#import resolution, line joining, counter macros.
    │
    ├─ 2. Lexer (lexer.cpp / lexer.h)
    │       Converts preprocessed source → token stream.
    │       Handles all keywords, operators, string escapes, hex/oct/bin
    │       literals, hex byte-array literals (0x"DEADBEEF"), string
    │       interpolation ($"..."), and multi-line strings ("""...""").
    │
    ├─ 3. Parser (parser.cpp / parser.h)
    │       Recursive-descent parser; produces an AST.
    │       All node types defined in ast.h.
    │       Desugars: method-call syntax, foreach, string interpolation,
    │       chained comparisons, switch → if chains (when appropriate),
    │       slice notation arr[a:b], lambda → named function, etc.
    │
    ├─ 4. AST Pre-Passes (codegen.cpp + codegen_stmt.cpp)
    │       Run before any LLVM IR is generated:
    │         - comptime evaluation (tryConstEvalFull, evalConstBuiltin)
    │         - Cross-function constant propagation (O1+)
    │         - Loop fusion (@loop(fuse=true))
    │         - Escape analysis (stack allocation of non-escaping arrays)
    │
    ├─ 4.5 CF-CTRE — Cross-Function Compile-Time Reasoning Engine
    │       (cfctre.h + cfctre.cpp, called from runCFCTRE() in codegen.cpp)
    │       Runs at O1+ after all AST pre-analysis, before IR emission.
    │       Deterministic SSA-semantics AST interpreter with:
    │         - CTValue: tagged union (i64/u64/f64/bool/string/array handle)
    │         - CTHeap: handle-based compile-time heap (no raw pointers)
    │         - CTFrame: per-call locals + return/break/continue signals
    │         - Memoisation: map<(fn,args_hash) → CTValue>, O(1) lookup
    │         - Fixed-point purity analysis over the whole call graph
    │         - Pipeline SIMD tile execution (kSIMDLaneWidth=8 lanes)
    │         - Depth guard (128 frames) + instruction budget (10M ops)
    │         - Back-propagation into legacy constInt/constString tables
    │       See Language Reference §28 for full specification.
    │
    ├─ 5. Code Generator (codegen.cpp + codegen_expr.cpp +
    │                      codegen_stmt.cpp + codegen_builtins.cpp)
    │       Walks the AST and emits LLVM IR. Key responsibilities:
    │         - Variable allocation (alloca + mem2reg)
    │         - Function codegen with noalias/nonnull/dereferenceable attrs
    │         - Array runtime (malloc-based, [len, elems...] layout)
    │         - String interning
    │         - Ownership tracking (move/borrow/freeze/reborrow/invalidate)
    │         - TBAA metadata on all memory accesses
    │         - Bounds checks with 1:1000 branch weights
    │         - Loop annotations (mustprogress, vectorize, unroll, etc.)
    │         - All 140+ builtin function implementations
    │
    ├─ 6. E-Graph Optimizer (egraph.cpp + egraph_optimizer.cpp)
    │       Applied at O2+ before LLVM passes.
    │       600+ algebraic rewrite rules on the AST/IR.
    │       Uses union-find with path compression + cost extraction.
    │
    ├─ 7. Superoptimizer (superoptimizer.cpp)
    │       Applied at O2+ after LLVM IR generation.
    │       Four passes: idiom recognition, algebraic simplification,
    │       branch-to-select, enumerative synthesis (level 3).
    │
    ├─ 8. LLVM Optimization Pipeline (codegen_opt.cpp)
    │       Standard LLVM PM: SROA, mem2reg, GVN, instcombine, LICM,
    │       loop unroll, inliner, etc. Level determined by -O flag.
    │
    ├─ 9. Hardware Graph Optimization Engine (hardware_graph.cpp)
    │       Activated with -march/-mtune. Builds CPU microarch model,
    │       does cycle-accurate scheduling, FMA fusion, strength reduction,
    │       software pipelining with loop metadata.
    │
    └─ 10. LLVM Backend
            Instruction selection, register allocation, native code emission.
```

### Key Design Decisions

1. **All integers are `i64`** — OmScript has one integer type internally. Width annotations (`u8`, `i32`, etc.) are purely semantic hints that affect type-cast behavior and optimizer hints, not storage size.

2. **Array layout: `[length, elem0, elem1, ...]`** — Every array (heap or stack or comptime global) has the length as the first 8-byte word. This makes `len()` a single load, bounds checks simple, and the layout consistent.

3. **TBAA hierarchy** — OmScript maintains 7 distinct TBAA type nodes to help LLVM's alias analysis distinguish array lengths, array elements, struct fields, string bytes, and map internals.

4. **Branch weights everywhere** — Error paths (bounds check failures, null checks, division-by-zero) use `!branch_weights [1, 1000]` to tell the compiler these are cold. The "good" path uses `[1000, 1]` for hot inner loops.

5. **`noalias` on all pointer params** — The ownership system proves no aliasing at the language level, so every function parameter receives `noalias + nonnull + dereferenceable(8)`.

6. **CF-CTRE is non-fatal** — The CF-CTRE engine always returns `std::optional<CTValue>`.  A `nullopt` result means "I couldn't evaluate this; fall back to runtime".  CF-CTRE never causes a compile error on its own.

---

## 3. Source File Reference

### Compiler Driver
| File | Purpose |
|------|---------|
| `src/compiler.cpp` / `include/compiler.h` | Top-level driver: preprocessing, parsing, codegen invocation, optimization, linking. Entry point for `omsc`. |
| `src/main.cpp` | CLI argument parsing, `omsc run/compile/check/emit-ir` dispatch. |

### Frontend
| File | Purpose |
|------|---------|
| `src/lexer.cpp` / `include/lexer.h` | Tokenizer. All tokens including `comptime`, `parallel`, `reborrow`, `freeze`, `prefetch`, `pipeline`. Handles hex byte-arrays, string interpolation, multi-line strings. |
| `src/parser.cpp` / `include/parser.h` | Recursive-descent parser. Produces AST. Implements all grammar rules including method-call desugaring, slice notation, chained comparisons, lambda → function, string interpolation → concat chain. |
| `include/ast.h` | All AST node types. Key expression nodes: `LITERAL_EXPR`, `IDENTIFIER_EXPR`, `BINARY_EXPR`, `UNARY_EXPR`, `CALL_EXPR`, `INDEX_EXPR`, `ARRAY_EXPR`, `DICT_EXPR`, `ASSIGN_EXPR`, `INDEX_ASSIGN_EXPR`, `TERNARY_EXPR`, `LAMBDA_EXPR`, `COMPTIME_EXPR`, `PIPE_EXPR`, `RANGE_EXPR`, `SPREAD_EXPR`, `SCOPE_RESOLUTION_EXPR`. Key statement nodes: `IF_STMT`, `WHILE_STMT`, `FOR_STMT`, `FOREACH_STMT`, `LOOP_STMT`, `REPEAT_STMT`, `TIMES_STMT`, `FOREVER_STMT`, `RETURN_STMT`, `VAR_STMT`, `CONST_STMT`, `ASSIGN_STMT`, `EXPR_STMT`, `BLOCK_STMT`, `SWITCH_STMT`, `TRY_STMT`, `THROW_STMT`, `DEFER_STMT`, `WITH_STMT`, `SWAP_STMT`, `PIPELINE_STMT`, `PREFETCH_STMT`, `BORROW_STMT`, `MOVE_STMT`, `FREEZE_STMT`, `REBORROW_STMT`, `INVALIDATE_STMT`, `PARALLEL_STMT`. |

### Code Generation
| File | Purpose |
|------|---------|
| `src/codegen.cpp` / `include/codegen.h` | Main code generator class. Contains: map/dict runtime, string interning (`internString_`), ownership tracking (`ownershipMap_`, `frozenVars_`, `borrowMap_`), TBAA setup (`setupTBAA()`), `tryConstEvalFull`, `tryFoldExprToConst`, `evalConstBuiltin`, `emitComptimeArray`. Also: `nonNegValues_`, `loopArrayLenCache_`, `arrayReturningFunctions_`. Hosts `ctEngine_` (unique_ptr<CTEngine>), `runCFCTRE()`, and bridge helpers `ctValueToConstValue`/`constValueToCTValue`. |
| `include/cfctre.h` / `src/cfctre.cpp` | CF-CTRE (Cross-Function Compile-Time Reasoning Engine). Core types: `CTValue` (tagged union), `CTHeap` (handle-based deterministic heap), `CTFrame` (per-call locals + signals), `CTMemoKey`, `CTGraph`, `CTEngine`. `runPass()` registers all functions + enums, runs fixed-point purity analysis, pre-evaluates zero-arg pure functions, builds call graph. `executeFunction()` evaluates any pure function with CT-known args. `evalComptimeBlock()` evaluates `comptime {}` blocks. Both methods return `optional<CTValue>` — `nullopt` means "fall back to runtime". |
| `src/codegen_expr.cpp` | Expression code generation: `generateExpression`, `generateBinaryOp`, `generateComparisonOp`. Bounds checks with TBAA and `!noundef` metadata. |
| `src/codegen_stmt.cpp` | Statement code generation: all loop forms (`while`, `for`, `foreach`, `loop`, `repeat`, `times`, `forever`, `parallel`), `try`/`catch`, `defer`, `with`, `swap`, `pipeline`, `prefetch`. |
| `src/codegen_builtins.cpp` | All builtin function implementations. Contains `builtinLookup` table mapping name → `BuiltinId` enum, and `generateBuiltin` switch with one case per builtin. Also the integer type-cast dispatch for `u8`/`i8`/`u16`/`i16`/`u32`/`i32`/`u64`/`i64`/`bool`. |
| `src/codegen_opt.cpp` | LLVM pass pipeline setup. Attribute inference (adding `readonly`, `speculatable`, `willreturn` to pure builtins). Memory effect inference for user functions. |

### Optimizers
| File | Purpose |
|------|---------|
| `src/egraph.cpp` / `src/egraph_optimizer.cpp` / `include/egraph.h` | E-graph equality saturation. Union-find data structure, 600+ algebraic rewrite rules, cost-based extraction. |
| `src/superoptimizer.cpp` / `include/superoptimizer.h` | Four-pass superoptimizer: idiom recognition (popcount, bswap, rotate, min/max, etc.), 300+ algebraic simplifications, branch-to-select conversion, enumerative synthesis. |
| `src/hardware_graph.cpp` / `include/hardware_graph.h` | HGOE: hardware profiles for 15+ CPU microarchitectures, cycle-accurate instruction scheduler, FMA generation, integer strength reduction, software pipelining. |

### Runtime
| File | Purpose |
|------|---------|
| `runtime/aot_profile.cpp` | Adaptive JIT runtime. Hot function detection, tier-2 recompilation at O3 with PGO hints. |
| `runtime/deopt.cpp` | Guard-based deoptimization. Fallback to baseline code when speculative assumptions fail. |
| `runtime/jit_profiler.cpp` | Runtime profiling: call counts, branch probabilities, argument type tracking. |
| `runtime/refcounted.h` | Reference-counted string type. Thread-safe ref-counting with atomic ops. |
| `runtime/value.cpp` / `runtime/value.h` | Dynamic value representation for the JIT interpreter. |

### TBAA Hierarchy

OmScript maintains **7 TBAA type nodes** under a single root node:

| TBAA Node | Used For | Memory Region |
|---|---|---|
| `tbaaArrayLen_` | Array length field | `arr[0]` (first i64 word) |
| `tbaaArrayElem_` | Array elements | `arr[1..N]` |
| `tbaaStructField_` | Struct fields | per-field nodes in `tbaaStructFieldCache_` |
| `tbaaStringData_` | String byte data | raw C string content |
| `tbaaMapKey_` | Map key storage | hash table key buckets |
| `tbaaMapVal_` | Map value storage | hash table value buckets |
| `tbaaMapHash_` | Map hash slot | hash table slot array |

Using distinct TBAA nodes prevents alias analysis from conservatively assuming that writing to an array element might change the array's length — which would block loads of `arr.length` from being hoisted out of loops.

---

## 4. Adding a New Builtin Function

Adding a new builtin follows a deterministic 12-step process. We'll use a hypothetical `popcount2(x, y)` builtin (sum of popcounts) as a running example.

### Step 1: Add the Lexer Token (if needed)

If the builtin name needs a reserved keyword (to prevent it from being shadowed by user variables), add it to `lexer.h`:

```cpp
// include/lexer.h — in the TokenType enum:
TOKEN_POPCOUNT2,  // only needed if "popcount2" must be a keyword
```

And in `lexer.cpp`'s keyword table:
```cpp
{"popcount2", TOKEN_POPCOUNT2},
```

For most builtins, this step is **not needed** — builtins are recognized as regular identifiers in `codegen_builtins.cpp`. Only add a keyword if the builtin name must be reserved (i.e., users cannot have a function or variable with the same name).

### Step 2: Add the AST Node Type (if new statement/expression form)

For new **expressions** that are just function calls, no AST change is needed — use `CALL_EXPR`. For truly new expression forms (e.g., a ternary-like syntax), add to `include/ast.h`:

```cpp
// Only if this is a new syntactic form, not just a function call:
MY_NEW_EXPR,
```

### Step 3: Add BuiltinId to the Lookup Table

In `src/codegen_builtins.cpp`, find the `builtinLookup` table (a `std::unordered_map<std::string, BuiltinId>`) and add an entry:

```cpp
// In the BuiltinId enum:
BUILTIN_POPCOUNT2,

// In builtinLookup initialization:
{"popcount2", BUILTIN_POPCOUNT2},
```

### Step 4: Implement IR Generation in `generateBuiltin`

In the `generateBuiltin` switch statement in `src/codegen_builtins.cpp`:

```cpp
case BUILTIN_POPCOUNT2: {
    // Validate argument count
    if (args.size() != 2) {
        throw CompileError("popcount2 requires exactly 2 arguments");
    }

    // Generate IR for each argument
    llvm::Value* a = generateExpression(args[0]);
    llvm::Value* b = generateExpression(args[1]);

    // Call LLVM's intrinsic for popcount
    llvm::Function* popcntFn = llvm::Intrinsic::getDeclaration(
        module_.get(), llvm::Intrinsic::ctpop, {builder_.getInt64Ty()});

    llvm::Value* pa = builder_.CreateCall(popcntFn, {a}, "popcount_a");
    llvm::Value* pb = builder_.CreateCall(popcntFn, {b}, "popcount_b");

    // Return their sum
    return builder_.CreateAdd(pa, pb, "popcount2");
}
```

### Step 5: Add Compile-Time Fold in `evalConstBuiltin`

If the builtin is pure (same output for same input, no side effects), add a case to `evalConstBuiltin` in `src/codegen.cpp`:

```cpp
case BUILTIN_POPCOUNT2: {
    if (args.size() != 2) break;
    auto va = tryFoldExprToConst(args[0]);
    auto vb = tryFoldExprToConst(args[1]);
    if (va && vb) {
        int64_t a = va->getConstantIntVal();
        int64_t b = vb->getConstantIntVal();
        // Count bits in both values
        int64_t result = __builtin_popcountll(a) + __builtin_popcountll(b);
        return ConstValue::makeInt(result);
    }
    break;
}
```

### Step 6: Add to `kPureBuiltins`

Pure builtins are registered so the compiler can emit `readonly`, `speculatable`, and `willreturn` attributes on the generated IR call (and so the cross-function constant propagator can reason about them). In `src/codegen_opt.cpp`:

```cpp
static const std::unordered_set<std::string> kPureBuiltins = {
    // ... existing entries ...
    "popcount2",
};
```

### Step 7: Register in `stdlibFunctions`

So the OPTMAX validator knows `popcount2` is a valid function to call inside OPTMAX blocks (and not a user-defined function that breaks the type rules), add it to the stdlib registry in `src/codegen.cpp` or `src/compiler.cpp`:

```cpp
stdlibFunctions_.insert("popcount2");
```

### Step 8: Add `!noundef` Metadata on Element Loads (O1+)

If your builtin reads from an array parameter, add `!noundef` to all element loads at O1+:

```cpp
llvm::LoadInst* load = builder_.CreateAlignedLoad(
    builder_.getInt64Ty(), elemPtr, llvm::Align(8), "elem");
if (optLevel_ >= 1) {
    load->setMetadata(llvm::LLVMContext::MD_noundef,
        llvm::MDNode::get(ctx_, {}));
}
```

### Step 9: Add Branch Weights on Error Paths

If your builtin has an error path (e.g., bounds check, null check), add branch weights. Error paths should be weighted `1:1000` (cold); success paths `1000:1` (hot):

```cpp
// Bounds check example:
llvm::Value* inBounds = builder_.CreateICmpULT(idx, len, "in_bounds");
// Hot branch (inBounds = true) gets weight 1000, cold (out of bounds) gets 1
llvm::MDNode* weights = llvm::MDBuilder(ctx_).createBranchWeights(1000, 1);
builder_.CreateCondBr(inBounds, okBlock, errorBlock, weights);
```

### Step 10: Add TBAA Annotation on Memory Accesses

If your builtin reads or writes to an array or struct, annotate the load/store with the appropriate TBAA node:

```cpp
// Array element read:
llvm::LoadInst* elemLoad = builder_.CreateAlignedLoad(...);
elemLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);

// Array length read:
llvm::LoadInst* lenLoad = builder_.CreateLoad(...);
lenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
```

### Step 11: Add an Example Test

Create `examples/test_popcount2.om`:
```omscript
fn main() {
    var a = popcount2(0xFF, 0xF0);   // 8 + 4 = 12
    println(a);  // 12

    // Compile-time fold:
    const K = comptime { popcount2(255, 240); };
    println(K);  // 12

    return 0;
}
```

### Step 12: Add Test to `run_tests.sh`

In `run_tests.sh`, add an entry to the test table:

```bash
run_test "examples/test_popcount2.om" "12\n12"
```

---

## 5. Adding a New Statement Form

We'll use a hypothetical `assert_eq(a, b)` statement as an example (asserts two values are equal, with a descriptive error message).

### Step 1: Add the Keyword Token

In `include/lexer.h` (TokenType enum):
```cpp
TOKEN_ASSERT_EQ,
```

In `src/lexer.cpp` (keyword table):
```cpp
{"assert_eq", TOKEN_ASSERT_EQ},
```

### Step 2: Add the AST Node

In `include/ast.h`:
```cpp
// New statement node type:
ASSERT_EQ_STMT,

// New AST node struct:
struct AssertEqStmt : public StmtNode {
    ExprNode* lhs;
    ExprNode* rhs;
    std::string message;  // optional message
    SourceLocation loc;
    AssertEqStmt(ExprNode* l, ExprNode* r, const std::string& msg, SourceLocation loc)
        : StmtNode(ASSERT_EQ_STMT), lhs(l), rhs(r), message(msg), loc(loc) {}
};
```

### Step 3: Parse the New Statement

In `src/parser.cpp`, in the statement-parsing function (usually `parseStatement()`):

```cpp
case TOKEN_ASSERT_EQ: {
    advance();  // consume 'assert_eq'
    expect(TOKEN_LPAREN, "'(' after assert_eq");
    ExprNode* lhs = parseExpression();
    expect(TOKEN_COMMA, "',' between assert_eq arguments");
    ExprNode* rhs = parseExpression();
    std::string msg = "";
    if (current().type == TOKEN_COMMA) {
        advance();
        // Optional third argument: message string
        msg = parseStringLiteral();
    }
    expect(TOKEN_RPAREN, "')' after assert_eq");
    expectSemicolon();
    return new AssertEqStmt(lhs, rhs, msg, currentLoc());
}
```

### Step 4: Generate LLVM IR

In `src/codegen_stmt.cpp`, in `generateStatement()`:

```cpp
case ASSERT_EQ_STMT: {
    auto* stmt = static_cast<AssertEqStmt*>(node);
    llvm::Value* lhsVal = generateExpression(stmt->lhs);
    llvm::Value* rhsVal = generateExpression(stmt->rhs);

    // Compare the two values
    llvm::Value* eq = builder_.CreateICmpEQ(lhsVal, rhsVal, "assert_eq_cond");

    // Create the pass/fail blocks
    llvm::BasicBlock* okBlock   = createBlock("assert_eq.ok");
    llvm::BasicBlock* failBlock = createBlock("assert_eq.fail");

    // Hot branch: equality holds (1000:1 weight)
    llvm::MDNode* weights = llvm::MDBuilder(ctx_).createBranchWeights(1000, 1);
    builder_.CreateCondBr(eq, okBlock, failBlock, weights);

    // Fail block: print error message and abort
    builder_.SetInsertPoint(failBlock);
    emitRuntimeError("assert_eq failed: " + stmt->message);
    builder_.CreateUnreachable();

    // Continue in ok block
    builder_.SetInsertPoint(okBlock);
    return;
}
```

### Step 5: Document and Test

- Add `assert_eq` to `LANGUAGE_REFERENCE.md` in the control-flow section.
- Add an example test: `examples/test_assert_eq.om`.
- Add to `run_tests.sh`.

---

## 6. Adding a New Expression Operator

We'll use a hypothetical `<=>` three-way comparison (spaceship) operator as an example.

### Step 1: Add the Token

In `include/lexer.h`:
```cpp
TOKEN_SPACESHIP,  // <=>
```

In `src/lexer.cpp`, in the operator-scanning section:
```cpp
case '<':
    if (peek() == '=' && peekNext() == '>') {
        advance(); advance();
        return Token{TOKEN_SPACESHIP, "<=>"};
    }
    // ... existing < and <= handling ...
```

### Step 2: Add the AST Node (optional)

If `<=>` desugars to existing binary-op nodes, you can handle it entirely in the parser without a new AST node. Otherwise:

```cpp
SPACESHIP_EXPR,
```

### Step 3: Parse the Operator

In `src/parser.cpp`, in the comparison-level parsing (usually between `parseBitwise` and `parseLogical`):

```cpp
// In the comparison parsing level:
if (current().type == TOKEN_SPACESHIP) {
    advance();
    ExprNode* rhs = parseAddSub();
    // <=> desugars to: (a > b) - (a < b)
    // Or can be a distinct AST node
    return new BinaryExpr(SPACESHIP_EXPR, lhs, rhs, loc);
}
```

### Step 4: Generate LLVM IR

In `src/codegen_expr.cpp`, in `generateBinaryOp`:

```cpp
case SPACESHIP_EXPR: {
    llvm::Value* lhsVal = generateExpression(expr->lhs);
    llvm::Value* rhsVal = generateExpression(expr->rhs);
    // Emit: (a > b) ? 1 : (a < b) ? -1 : 0
    llvm::Value* gt = builder_.CreateICmpSGT(lhsVal, rhsVal, "gt");
    llvm::Value* lt = builder_.CreateICmpSLT(lhsVal, rhsVal, "lt");
    llvm::Value* gtInt = builder_.CreateZExt(gt, builder_.getInt64Ty());
    llvm::Value* ltInt = builder_.CreateZExt(lt, builder_.getInt64Ty());
    return builder_.CreateSub(gtInt, ltInt, "spaceship");
}
```

### Step 5: Add to Operator Precedence Table

Update §10.12 in `LANGUAGE_REFERENCE.md` with the new operator's precedence level.

---

## 7. TBAA Annotation Guide

Type-Based Alias Analysis (TBAA) metadata allows LLVM to prove that loads and stores to different memory regions cannot alias each other. OmScript maintains 7 TBAA nodes under a single `omscript_root` TBAA node.

### Setting Up TBAA

TBAA nodes are initialized in `CodeGenerator::setupTBAA()` in `src/codegen.cpp`:

```cpp
void CodeGenerator::setupTBAA() {
    llvm::MDBuilder mdb(ctx_);
    auto* root = mdb.createTBAARoot("omscript_root");

    tbaaArrayLen_   = mdb.createTBAAScalarTypeNode("array_len",    root);
    tbaaArrayElem_  = mdb.createTBAAScalarTypeNode("array_elem",   root);
    tbaaStringData_ = mdb.createTBAAScalarTypeNode("string_data",  root);
    tbaaMapKey_     = mdb.createTBAAScalarTypeNode("map_key",      root);
    tbaaMapVal_     = mdb.createTBAAScalarTypeNode("map_val",      root);
    tbaaMapHash_    = mdb.createTBAAScalarTypeNode("map_hash",     root);
    // Struct field nodes are created on demand in tbaaStructFieldCache_
}
```

### Applying TBAA to Loads and Stores

Every load and store instruction that accesses a known memory region should carry the appropriate TBAA node:

```cpp
// Array length load:
llvm::LoadInst* lenLoad = builder_.CreateLoad(builder_.getInt64Ty(), lenPtr, "arr.len");
lenLoad->setMetadata(llvm::LLVMContext::MD_tbaa,
    llvm::MDBuilder(ctx_).createTBAAStructTagNode(tbaaArrayLen_, tbaaArrayLen_, 0));

// Array element load:
llvm::LoadInst* elemLoad = builder_.CreateAlignedLoad(
    builder_.getInt64Ty(), elemPtr, llvm::Align(8), "arr.elem");
elemLoad->setMetadata(llvm::LLVMContext::MD_tbaa,
    llvm::MDBuilder(ctx_).createTBAAStructTagNode(tbaaArrayElem_, tbaaArrayElem_, 0));

// Struct field load (per-field node from cache):
llvm::MDNode* fieldNode = getOrCreateStructFieldTBAA(structName, fieldIndex);
llvm::LoadInst* fieldLoad = builder_.CreateAlignedLoad(...);
fieldLoad->setMetadata(llvm::LLVMContext::MD_tbaa,
    llvm::MDBuilder(ctx_).createTBAAStructTagNode(fieldNode, fieldNode, 0));
```

### Why TBAA Matters

Without TBAA, a loop like this:

```omscript
for (i in 0...len(arr)) {
    arr[i] += 1;
}
```

would be unvectorizable because the compiler can't prove that writing to `arr[i]` doesn't change `len(arr)`. With TBAA, the `arr.len` load (tagged `tbaaArrayLen_`) and the `arr.elem` store (tagged `tbaaArrayElem_`) are provably non-aliasing, allowing the loop to be vectorized and the length load to be hoisted.

---

## 8. Constant Folding Guide

OmScript performs constant folding at two levels: **AST-level** (via `evalConstBuiltin` and `tryConstEvalFull`) and **LLVM IR-level** (via the standard InstCombine + ConstantFolding passes).

### AST-Level Folding: `evalConstBuiltin`

`evalConstBuiltin` is the main dispatch function for compile-time evaluation of builtin calls. Located in `src/codegen.cpp`.

**How it works:**
1. Called from `tryFoldExprToConst` when a `CALL_EXPR` node is encountered.
2. Identifies the builtin by name.
3. Recursively calls `tryFoldExprToConst` on each argument.
4. If all arguments resolved to constants, applies the builtin's pure semantics and returns a `ConstValue`.
5. If any argument is non-constant, returns `std::nullopt` (no fold possible).

**Adding a new constant fold:**

```cpp
// In evalConstBuiltin's switch:
case BUILTIN_MY_NEW_BUILTIN: {
    if (args.size() < 1) break;
    auto v0 = tryFoldExprToConst(args[0]);
    if (!v0) break;  // argument is non-constant — cannot fold

    int64_t x = v0->getIntVal();
    // Compute the result:
    int64_t result = my_pure_computation(x);
    return ConstValue::makeInt(result);
}
```

**For string-returning builtins:**
```cpp
case BUILTIN_MY_STRING_BUILTIN: {
    auto v0 = tryFoldExprToConst(args[0]);
    if (!v0 || !v0->isString()) break;
    std::string s = v0->getStringVal();
    std::string result = my_string_transform(s);
    // Intern the result string and return it:
    return ConstValue::makeString(internString(result));
}
```

**For array-returning builtins** (v4.1.1+):
```cpp
case BUILTIN_MY_ARRAY_BUILTIN: {
    auto v0 = tryFoldExprToConst(args[0]);
    if (!v0) break;
    std::vector<int64_t> elems = compute_array(v0->getIntVal());
    // Call emitComptimeArray to create the global constant:
    return ConstValue::makeArray(emitComptimeArray(elems));
}
```

### `tryConstEvalFull` — Full Function Body Evaluation

`tryConstEvalFull` evaluates an entire function body given a set of constant argument values. It handles:
- `var` declarations with constant initializers
- Arithmetic and logic on constants
- `if`/`else` with known-constant conditions
- `for` loops with known bounds
- Calls to other pure functions (recursive evaluation)
- All builtin functions recognized by `evalConstBuiltin`
- Integer type-cast builtins: `u8()`, `i8()`, etc.

This function is called when `comptime { userFunc(constArgs...); }` is encountered and the function hasn't been seen before.

### Chain Folding

Many folding optimizations are **chains** where one fold enables another:

```omscript
const N = comptime { 1 << 6; };   // Step 1: N = 64
const S = sum(range(1, N + 1));    // Step 2: range(1, 65) → len=64; sum → 64*65/2=2080
```

The key is that `tryFoldExprToConst` is called recursively on sub-expressions. When `N` is a `const` with known value 64, `N + 1` folds to 65, `range(1, 65)` creates a constant array of 64 elements, and `sum` over that constant array folds to 2080.

---

## 9. Comptime Evaluation Guide

The `comptime {}` evaluation system is the most powerful constant-folding mechanism in OmScript. Understanding how it works internally helps you write functions that can be evaluated at compile time.

### How `comptime` Works Internally

When the code generator encounters a `COMPTIME_EXPR` node:

1. It calls `tryConstEvalFull(comptimeBlock)`.
2. `tryConstEvalFull` interprets the block statement-by-statement in a virtual machine:
   - `VAR_STMT` / `CONST_STMT`: adds the variable to the local constant map.
   - `ASSIGN_STMT`: updates the variable in the local constant map.
   - `IF_STMT`: evaluates the condition; if constant, takes the appropriate branch.
   - `FOR_STMT` (range): expands the loop iterations up to a step limit.
   - `EXPR_STMT` with a function call: dispatches to `evalConstBuiltin` or `tryConstEvalFull` (recursive).
   - `RETURN_STMT`: returns the constant value to the caller.
   - Last expression statement (implicit return, v4.1.1+): returns that expression's value.
3. The return value is a `ConstValue` — either an integer, a string, or an array.
4. For integer/string results: `emitLiteralConst(val)` is called to create an LLVM constant and substitute it at the call site.
5. For array results: `emitComptimeArray(elems)` creates a `[N+1 x i64] private unnamed_addr constant` global.

### `emitComptimeArray` in Detail

```cpp
// src/codegen.cpp
llvm::Value* CodeGenerator::emitComptimeArray(const std::vector<int64_t>& elems) {
    // elems = the array elements (NOT including the length prefix)
    int64_t N = elems.size();

    // Build LLVM constant array type: [N+1 x i64]
    std::vector<llvm::Constant*> constants;
    constants.push_back(llvm::ConstantInt::get(builder_.getInt64Ty(), N));
    for (int64_t v : elems) {
        constants.push_back(llvm::ConstantInt::get(builder_.getInt64Ty(), v));
    }

    llvm::ArrayType* arrTy = llvm::ArrayType::get(builder_.getInt64Ty(), N + 1);
    llvm::Constant* initArr = llvm::ConstantArray::get(arrTy, constants);

    // Create the global variable
    auto* gv = new llvm::GlobalVariable(
        *module_,
        arrTy,
        /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage,
        initArr,
        ".comptime_arr"
    );
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    gv->setAlignment(llvm::Align(8));

    // Return pointer to first element as i64* (OmScript array pointer convention)
    return builder_.CreateBitCast(gv, builder_.getInt64Ty()->getPointerTo());
}
```

### Writing comptime-Compatible User Functions

For a user function to be callable inside `comptime`, it must:

1. **Use only constant-foldable operations** — arithmetic, comparisons, array operations on constant arrays, string operations on string literals, integer type-casts.
2. **Have no side effects** — no I/O, no global state mutation, no threading.
3. **Have computable control flow** — `if` conditions must be evaluatable from the arguments; `for` loop bounds must be constant.
4. **Not exceed the evaluation step limit** — the evaluator aborts after 10,000 steps to prevent infinite loops.

**Pattern: loop with `array_fill` + index writes:**
```omscript
fn make_table(n:int) -> int[] {
    var t:int[] = array_fill(n, 0);   // comptime-foldable: creates const array
    for (i:int in 0...n) {
        t[i] = i * i;                  // comptime-foldable: index assignment to const array
    }
    return t;
}
const SQUARES:int[] = comptime { make_table(10); };
```

**Pattern: string → byte array using `u64()` casts:**
```omscript
fn str_bytes(s:string) -> u8[] {
    var n:int = len(s);               // comptime: strlen(s)
    var out:u8[] = array_fill(n, 0);
    for (i:int in 0...n) {
        out[i] = u8(s[i]);            // comptime: u8(charcode)
    }
    return out;
}
const BYTES:u8[] = comptime { str_bytes("hello"); };
// BYTES == [104, 101, 108, 108, 111]
```

---

## 10. Test Writing Guide

### Integration Tests

Integration tests live in `examples/` and are driven by `run_tests.sh`. Each test is an OmScript program whose expected output (stdout) is specified in the test script.

**Test file format** (`examples/test_myfeature.om`):
```omscript
fn main() {
    // Test code here
    println(result);   // Output compared against expected
    return 0;
}
```

**Adding to `run_tests.sh`:**
```bash
run_test "examples/test_myfeature.om" "expected output line 1\nexpected output line 2"
```

The `run_test` function compiles and runs the program, then compares stdout to the expected string. A test fails if:
- Compilation fails
- The program exits with a non-zero exit code
- Stdout doesn't match the expected string

### Unit Tests

Unit tests are GTest suites in `tests/`. Each `.cpp` file is a test suite testing a specific compiler component:

```cpp
// tests/test_constant_folding.cpp
#include <gtest/gtest.h>
#include "codegen.h"

TEST(ConstantFolding, PopcountFolds) {
    // Set up a minimal compiler context
    OmScriptCompiler compiler;
    auto result = compiler.evalConst("popcount(0xFF)");
    EXPECT_EQ(result, 8);
}

TEST(ConstantFolding, TypeCastFolds) {
    OmScriptCompiler compiler;
    EXPECT_EQ(compiler.evalConst("u8(300)"), 44);
    EXPECT_EQ(compiler.evalConst("i8(200)"), -56);
    EXPECT_EQ(compiler.evalConst("bool(0)"), 0);
    EXPECT_EQ(compiler.evalConst("bool(42)"), 1);
}
```

### What to Test

For every new feature, write tests for:
1. **Basic functionality** — does it produce the correct output?
2. **Edge cases** — zero values, negative values, overflow, empty arrays/strings.
3. **Compile-time folding** — does `comptime { myBuiltin(literal); }` produce the right constant?
4. **Error cases** — does wrong argument count produce a clear compile error?
5. **Interaction with existing features** — does it work inside OPTMAX blocks? Inside lambdas? With the pipe operator?
6. **Performance** — for performance-critical builtins, add a benchmark in `benchmark_pgo.sh`.

---

## 11. Code Style Guidelines

### General

- **C++17** throughout. Use structured bindings, `if constexpr`, `std::optional`, `std::string_view`, range-for freely.
- **No raw owning pointers** for new code — use `std::unique_ptr` or `std::shared_ptr`. Existing AST node code uses raw pointers for performance reasons.
- **No `using namespace std`** in headers. OK in `.cpp` files within function bodies.
- **`const`-correctness** — all read-only parameters should be `const&` or `const*`.
- **Error handling** — throw `DiagnosticError(Diagnostic{...})` for all user-facing errors (not `std::runtime_error`). Use `llvm::report_fatal_error` for internal invariant violations.
- **Unreachable paths** — use `llvm_unreachable("reason")` (not `assert(false)` or `throw std::logic_error`).

### Formatting

The project uses a `.clang-format` configuration. Format before committing:
```bash
clang-format -i src/*.cpp include/*.h runtime/*.cpp runtime/*.h
```

Key style rules:
- 4-space indentation
- Braces on same line for control flow (`if (x) {`), but class/struct/function definitions have opening brace on same line too
- `// Comment` style (no `/* */` for single-line comments in new code)
- LLVM naming conventions: `camelCase` for local variables, `CamelCase` for types, `camelCase_` with trailing underscore for member variables

### LLVM IR Quality Checklist

When emitting LLVM IR, follow these invariants:
- All loads from variables and array elements at O1+ must carry `!noundef` metadata
- All struct field loads at O1+ must use `CreateAlignedLoad(8)` and carry `!noundef`
- All struct field stores at O1+ must use `CreateAlignedStore(8)`
- All bounds-check conditional branches must carry `!branch_weights [1000, 1]` (success:failure)
- All error conditional branches must carry `!branch_weights [1, 1000]`
- All pointer parameters must receive `noalias + nonnull + dereferenceable(8)` attributes
- All memory accesses to known regions must carry the appropriate TBAA node
- Map probe loops must have `mustprogress` metadata on the loop backedge

---

## 12. Pull Request Checklist

Before submitting a PR, verify all items:

### Functional
- [ ] All existing tests pass: `cd build && ctest --output-on-failure && cd .. && bash run_tests.sh`
- [ ] New functionality has integration tests in `examples/`
- [ ] New builtins have compile-time fold tests (if applicable)
- [ ] New language features are documented in `LANGUAGE_REFERENCE.md`
- [ ] `CHANGELOG.md` has an entry for this change

### Code Quality
- [ ] Code is formatted: `clang-format -i src/*.cpp include/*.h`
- [ ] No new compiler warnings at `-Wall -Wextra`
- [ ] No new sanitizer errors: build and test with `-DSANITIZE=address,undefined`
- [ ] All user-facing errors use `DiagnosticError`, not `std::runtime_error`
- [ ] All unreachable paths use `llvm_unreachable`, not `assert(false)`

### LLVM IR Quality (for codegen changes)
- [ ] New loads from array elements carry `!noundef` at O1+
- [ ] New loads from struct fields use `CreateAlignedLoad(8)` and carry `!noundef` at O1+
- [ ] New error branches carry `!branch_weights [1, 1000]`
- [ ] New memory accesses carry TBAA annotations
- [ ] New pointer parameters carry `noalias + nonnull + dereferenceable(8)`

### For New Builtins Specifically
- [ ] Added `BuiltinId` entry and `builtinLookup` entry in `codegen_builtins.cpp`
- [ ] Implemented IR generation in `generateBuiltin` switch
- [ ] Added compile-time fold in `evalConstBuiltin` (if pure)
- [ ] Added to `kPureBuiltins` (if pure)
- [ ] Added to `stdlibFunctions` registry
- [ ] Added example in `examples/`
- [ ] Added test to `run_tests.sh`
- [ ] Added to builtin table in `LANGUAGE_REFERENCE.md` § 19
- [ ] Added to `README.md` built-in functions tables

### For New Type-Cast Builtins Specifically
- [ ] Added to the integer type-cast dispatch in `generateCall`
- [ ] Added `evalConstBuiltin` fold case
- [ ] Added to the type-cast table in `LANGUAGE_REFERENCE.md` § 5.7, § 19.5.1, and § 27
- [ ] Added examples demonstrating all edge cases (zero, negative, overflow)

### Performance
- [ ] For performance-critical changes, run the PGO benchmark: `bash benchmark_pgo.sh`
- [ ] OptStats counters are updated if this optimization tracks a new metric
- [ ] No new unnecessary heap allocations in the compiler hot path
