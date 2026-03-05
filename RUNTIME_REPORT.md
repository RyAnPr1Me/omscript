# OmScript Runtime & Pipeline Architecture — Extreme-Detail Technical Report

> This document provides a complete, line-by-line analysis of every execution
> mode, every compilation pipeline stage, every runtime component, and every
> thread-safety mechanism in OmScript.  It is intended for compiler engineers,
> runtime developers, and anyone seeking a ground-truth reference for how the
> system works internally.

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Frontend Pipeline (Shared)](#2-frontend-pipeline-shared)
3. [AOT Compilation Pipeline (`omsc build`)](#3-aot-compilation-pipeline-omsc-build)
4. [JIT Execution Pipeline (`omsc run`)](#4-jit-execution-pipeline-omsc-run)
5. [Adaptive JIT Runtime — Tier Architecture](#5-adaptive-jit-runtime--tier-architecture)
6. [Dispatch Prolog — Code Injection & Hot-Patching](#6-dispatch-prolog--code-injection--hot-patching)
7. [Runtime Profiling System](#7-runtime-profiling-system)
8. [Background Recompilation — Thread Pool & Task Queue](#8-background-recompilation--thread-pool--task-queue)
9. [PGO-Guided Tiered Recompilation (`doRecompile()`)](#9-pgo-guided-tiered-recompilation-dorecompile)
10. [Deoptimization Framework](#10-deoptimization-framework)
11. [Value System & Reference Counting](#11-value-system--reference-counting)
12. [Memory Ordering & Thread Safety Guarantees](#12-memory-ordering--thread-safety-guarantees)
13. [Non-JIT `run` Path (AOT Fallback)](#13-non-jit-run-path-aot-fallback)
14. [Full Pipeline Diagrams](#14-full-pipeline-diagrams)

---

## 1. System Overview

OmScript has **two primary execution modes**, both producing native machine
code through LLVM:

| Mode | Command | Pipeline | Use Case |
|------|---------|----------|----------|
| **AOT** | `omsc build foo.om` | Source → Lex → Parse → LLVM IR → IPO/O2/O3 → Object → Link → Binary | Production binaries |
| **JIT** | `omsc run foo.om` | Source → Lex → Parse → LLVM IR → Per-function O1-O3 → MCJIT → In-process execution → Background O3+PGO tiered recompilation | Development / interactive |

There is **no interpreter or bytecode tier** — every function is compiled to
native machine code from the very first call.

### Source Files

| File | Role |
|------|------|
| `src/lexer.cpp` | Tokeniser |
| `src/parser.cpp` | Recursive descent parser → AST |
| `src/codegen.cpp` (6427 lines) | AST → LLVM IR, stdlib builtins, optimisation passes |
| `src/compiler.cpp` | AOT driver: codegen → object file → linker |
| `src/main.cpp` (2870 lines) | CLI entry: `build`, `run`, `emit-ir`, `pkg`, etc. |
| `runtime/aot_profile.cpp` / `.h` | Adaptive JIT runner, dispatch prolog injection, background recompilation |
| `runtime/jit_profiler.cpp` / `.h` | Runtime profiling (branches, args, loops, call-sites) |
| `runtime/deopt.cpp` / `.h` | Deoptimization (guard failures → fallback to baseline) |
| `runtime/value.cpp` / `.h` | Dynamically-typed value (int/float/string/none) |
| `runtime/refcounted.h` | Reference-counted string with atomic refcount |

---

## 2. Frontend Pipeline (Shared)

Both AOT and JIT modes share the same frontend:

### 2.1 Lexer (`src/lexer.cpp`)

**Input:** Source string  
**Output:** `std::vector<Token>`

The lexer processes source text character-by-character in a single forward pass:
- Pre-allocates token vector with heuristic `source.length() / 4 + 16`.
- Recognises: integers (decimal, hex `0x`, octal `0o`, binary `0b`), floats,
  strings (single `"..."` and triple-quoted `"""..."""`), identifiers, keywords,
  operators, range operator `...`, null-coalesce `??`, fat arrow `=>`, and
  OPTMAX pragma blocks (`OPTMAX=:` / `OPTMAX!:`).
- Tracks `line` / `column` for error reporting.
- Handles single-line (`//`) and multi-line (`/* */`) comments with nesting.

### 2.2 Parser (`src/parser.cpp`)

**Input:** Token vector  
**Output:** `std::unique_ptr<Program>` (AST)

Recursive-descent parser implementing:
- Function declarations with parameters, default values, type annotations
- Variable declarations (`var` / `const`)
- Control flow: `if/else`, `while`, `do-while`, `for` (C-style, range, for-each), `switch/case`
- Error handling: `try/catch/throw`
- Enums, lambdas, pipe operator (`|>`), spread operator (`...`), ternary `?:`
- Full operator precedence via Pratt parsing (assignment < ternary < logical < comparison < arithmetic < unary < postfix)

### 2.3 AST Nodes (`include/ast.h`)

The AST is a classical tree of `Statement` and `Expression` node types:
- `FunctionDecl`, `VarDecl`, `ReturnStmt`, `IfStmt`, `WhileStmt`, `ForStmt`,
  `ForEachStmt`, `DoWhileStmt`, `SwitchStmt`, `TryCatchStmt`, `ThrowStmt`,
  `BlockStmt`, `ExprStmt`
- `LiteralExpr`, `IdentifierExpr`, `BinaryExpr`, `UnaryExpr`, `CallExpr`,
  `AssignExpr`, `PostfixExpr`, `PrefixExpr`, `TernaryExpr`, `ArrayExpr`,
  `IndexExpr`, `IndexAssignExpr`

Every AST node carries source location (`line`, `column`) for diagnostics.

---

## 3. AOT Compilation Pipeline (`omsc build`)

### 3.1 Entry Point

`main.cpp` → `Compiler::compile()` → `CodeGenerator::generate()` →
`runOptimizationPasses()` → `writeObjectFile()` → system linker.

### 3.2 Code Generation (`codegen.cpp`, `generate()`)

**Step 1: IR Generation**

The `generate()` method:
1. Creates an `llvm::Module` with a fresh `LLVMContext`.
2. Declares 69+ stdlib built-in functions as native LLVM IR (print, strlen,
   malloc, array operations, math functions, I/O, etc.).
3. Pre-analyses string types across all functions (`preAnalyzeStringTypes()`)
   to populate `stringReturningFunctions_` and `funcParamStringTypes_` so that
   string values (stored as `i64`-casted pointers) cross function boundaries correctly.
4. Generates LLVM IR for each user-defined function via `generateFunction()`:
   - Creates `llvm::Function` with `i64` return type (all values are boxed as i64).
   - Generates each statement and expression recursively.
   - Manages scope stacks for variable bindings and const tracking.
   - Enforces a compile-time IR instruction budget (`kMaxIRInstructions = 1,000,000`).
5. Applies OPTMAX per-function optimisation (if any OPTMAX-annotated functions exist).

**Step 2: Optimisation (`runOptimizationPasses()`)** — only in AOT mode (`dynamicCompilation_ == false`)

1. Initialises the native LLVM target.
2. Creates a `TargetMachine` for the host CPU (or `-march` / `-mtune` overrides).
3. Sets the module's target triple and data layout.
4. At O0: returns immediately (no optimisation).
5. At O1/O2/O3: Uses the LLVM new pass manager (`PassBuilder`):
   - `PipelineTuningOptions`:
     - `LoopVectorization` / `SLPVectorization` / `LoopInterleaving` controlled by `-fvectorize`.
     - `LoopUnrolling` controlled by `-funroll-loops`.
     - `MergeFunctions` / `CallGraphProfile` at O2+.
     - `InlinerThreshold = 400` at O3.
   - PGO support:
     - `--pgo-gen=<path>`: Inserts LLVM IR instrumentation (`PGOOptions::IRInstr`).
     - `--pgo-use=<path>`: Reads `.profdata` for profile-guided optimisation.
   - LoopDistributePass registered at O3 with `-floop-optimize`.
   - LTO: Uses `buildLTOPreLinkDefaultPipeline()` instead of
     `buildPerModuleDefaultPipeline()` when LTO is enabled.
   - Standard pipeline: `buildPerModuleDefaultPipeline(O1/O2/O3)` runs the
     full inter-procedural pipeline (IPSCCP, inlining, GlobalDCE, loop
     vectorisation, SLP vectorisation, etc.).

**Step 3: Object Code Emission (`writeObjectFile()` / `writeBitcodeFile()`)**

- `writeObjectFile()`: Creates a `TargetMachine`, adds a machine-code emission
  pass to a legacy PM, writes a `.o` file with `CGFT_ObjectFile`.
- `writeBitcodeFile()`: Writes raw LLVM bitcode (`.bc`) for LTO mode. The
  linker (gcc/clang with `-flto`) performs whole-program optimisation at link time.

**Step 4: Linking (`compiler.cpp`)**

`Compiler::compile()` invokes the system linker:
1. Searches for `gcc`, `cc`, or `clang` (prefers `clang` when LTO is enabled).
2. Passes flags: `-O1`/`-O2`/`-O3`, `-flto`, `-static`, `-s`, `-fstack-protector-strong`, `-lm`.
3. Links the object/bitcode file into a final executable.
4. Cleans up the temporary `.o` / `.bc` file.

### 3.3 AOT Optimisation Levels

| Level | IR Pipeline | Linker Flag | Key Passes |
|-------|-------------|-------------|------------|
| O0 | None | (none) | Target triple + data layout only |
| O1 | `buildPerModuleDefaultPipeline(O1)` | `-O1` | Basic inlining, SimplifyCFG, mem2reg, InstCombine |
| O2 | `buildPerModuleDefaultPipeline(O2)` | `-O2` | + loop vectorisation, SLP vectorisation, MergeFunctions |
| O3 | `buildPerModuleDefaultPipeline(O3)` | `-O3` | + InlinerThreshold=400, LoopDistribute, aggressive unrolling |
| O3+LTO | `buildLTOPreLinkDefaultPipeline(O3)` | `-O3 -flto` | Defers heavy IPO to link-time |

---

## 4. JIT Execution Pipeline (`omsc run`)

### 4.1 Entry Point

`main.cpp` line 2654: When `command == Command::Run && flagJIT`:

```
Source → Lexer → Parser → CodeGenerator::generateHybrid() →
  AdaptiveJITRunner::run() → MCJIT in-process execution
```

### 4.2 Code Generation for JIT (`generateHybrid()`)

**File:** `codegen.cpp` lines 6399-6425

1. Sets `dynamicCompilation_ = true`.
2. Calls `generate(program)` — same IR generation as AOT, but when it reaches
   the optimisation decision point (line 1491), it detects `dynamicCompilation_`
   and **skips the module-wide IPO pipeline**.  This preserves function boundaries
   so the JIT can hot-patch individual functions.
3. Calls `runJITBaselinePasses()` which applies **intra-procedural** (per-function)
   optimisation scaled to the user's optimisation level:

   **O0:** Skips everything for fastest startup.

   **O1:** mem2reg, SROA, EarlyCSE, InstCombine, InstSimplify, Reassociate,
   SimplifyCFG, GVN, DCE, TailCallElimination.

   **O2:** All of O1 + LoopSimplify, LoopRotate (LLVM<19), LICM, LoopStrengthReduce,
   LoopUnroll, LoopDataPrefetch, MergedLoadStoreMotion (LLVM<18), Sinking.

   **O3:** All of O2 + StraightLineStrengthReduce, NaryReassociate, ConstantHoisting,
   SeparateConstOffsetFromGEP, SpeculativeExecution, FlattenCFG. **Two iterations**
   of the entire pass pipeline for cascading optimisation.

4. The resulting module is passed to `AdaptiveJITRunner::run()`.

### 4.3 `AdaptiveJITRunner::run()` — The Complete JIT Launch Sequence

**File:** `runtime/aot_profile.cpp` lines 216-396

This is the heart of the JIT execution. Every step is detailed below:

#### Step 1: Serialise Clean Bitcode (lines 222-230)

```cpp
llvm::SmallVector<char, 0> buf;
llvm::raw_svector_ostream os(buf);
llvm::WriteBitcodeToFile(*baseModule, os);
cleanBitcode_.assign(buf.begin(), buf.end());
```

The module's **clean IR** (no dispatch prologs, no counters) is serialised to
bitcode and stored as `cleanBitcode_`.  This is the **source material** for all
future tiered recompilations — it preserves the exact IR the codegen produced,
including any per-function optimisation from `runJITBaselinePasses()`.

#### Step 2: Create Working Copy (lines 234-240)

```cpp
auto instrCtx = std::make_unique<llvm::LLVMContext>();
auto memBuf = llvm::MemoryBuffer::getMemBuffer(...cleanBitcode_...);
auto modOrErr = llvm::parseBitcodeFile(memBuf->getMemBufferRef(), *instrCtx);
auto instrMod = std::move(*modOrErr);
```

A fresh `LLVMContext` + `Module` is created by re-parsing the bitcode.  This
ensures `baseModule` (the caller's codegen output) is not modified, and the
working copy lives in its own context for thread safety (LLVM contexts are
NOT thread-safe).

#### Step 3: Inject Dispatch Prologs (lines 243-245)

```cpp
injectCounters(*instrMod);
```

This is the critical transformation that enables tiered compilation.
See [Section 6](#6-dispatch-prolog--code-injection--hot-patching) for the full
details of what `injectCounters()` does.

#### Step 4: JIT-Compile Instrumented Module (lines 257-296)

```cpp
llvm::EngineBuilder builder(std::move(instrMod));
builder.setOptLevel(llvm::CodeGenOptLevel::None);  // O0 backend!
llvm::ExecutionEngine* rawEngine = builder.create();
rawEngine->addGlobalMapping("__omsc_adaptive_recompile", ...);
rawEngine->addGlobalMapping("__omsc_deopt_guard_fail", ...);
rawEngine->finalizeObject();
```

Key details:
- Uses **O0 backend optimisation** (not O0 IR passes — the IR was already
  optimised by `runJITBaselinePasses()`).  O0 backend means MCJIT translates
  IR → machine code as fast as possible, with no extra ISel optimisation.
  Since Tier-1 code is replaced after just 5 calls, any backend optimisation
  would be wasted.
- Explicitly maps C-linkage callbacks (`__omsc_adaptive_recompile`,
  `__omsc_deopt_guard_fail`) so MCJIT can resolve them without `-rdynamic`.

#### Step 5: Start Background Thread Pool (lines 314-317)

```cpp
shutdownRequested_.store(false, std::memory_order_relaxed);
bgThreads_.reserve(kNumBgThreads);  // kNumBgThreads = 4
for (int i = 0; i < kNumBgThreads; i++)
    bgThreads_.emplace_back(&AdaptiveJITRunner::backgroundWorker, this);
```

Four background compilation threads are launched **before** main() starts
executing.  These threads wait on a condition variable for recompilation tasks.

#### Step 6: Eager Compilation Enqueue (lines 319-364)

```cpp
auto eagerMod = llvm::parseBitcodeFile(...cleanBitcode_...);
for (auto& fn : **eagerMod) {
    // skip declarations, main, __ prefixed
    void** fnPtrSlot = reinterpret_cast<void**>(slotAddr);
    functionTier_[name] = 2;  // Tentatively mark as Tier-2
    taskQueue_.push({name, kTier2Threshold, fnPtrSlot});
    queueCV_.notify_one();
}
```

**ALL** user-defined functions are immediately enqueued for background O3+PGO
recompilation.  This is "eager compilation" — by the time the program starts
executing, the background threads are already compiling optimised versions.
Most hot functions will have their optimised code ready before they are even
called for the first time.

The clean bitcode is re-parsed in a fresh context just to discover function
names and resolve the `__omsc_fn_<name>` global addresses from the Tier-1 engine.

#### Step 7: Execute main() (lines 381-396)

```cpp
g_activeRunner.store(this, std::memory_order_release);
using OmscMainFn = int64_t (*)();
int64_t exitVal = reinterpret_cast<OmscMainFn>(mainAddr)();
return static_cast<int>(exitVal & 0xFF);
```

- The active runner is stored atomically so the C-linkage callback can find it.
- `main()` is called directly via a function pointer cast.
- RAII guard (`RunnerGuard`) ensures the active-runner pointer is cleared and
  `drainBackgroundThread()` is called on exit (even if main() throws).

---

## 5. Adaptive JIT Runtime — Tier Architecture

### 5.1 Tier Definitions

| Tier | Trigger | Optimisation | Backend | Purpose |
|------|---------|--------------|---------|---------|
| **Tier 1 (Baseline)** | First call | User's opt level via `runJITBaselinePasses()` | MCJIT O0 backend | Fast startup; every function runs immediately |
| **Tier 2 (Warm)** | 5 calls OR eager enqueue | O3 + PGO annotations | MCJIT Aggressive backend | Profile-guided hot-path optimisation |
| **Tier 3 (Hot)** | 1,000,000,000 calls | O3 + deep PGO | MCJIT Aggressive backend | **Effectively disabled** (threshold = 1B) |

### 5.2 Tier Thresholds

```cpp
static constexpr int64_t kTier2Threshold = 5;
static constexpr int64_t kTier3Threshold = 1000000000LL; // effectively disabled
static constexpr int kMaxTier = 2;
```

In practice, every function goes through exactly **one** recompilation
(Tier 1 → Tier 2).  Tier 3 exists in the code but is set to an unreachable
threshold.

### 5.3 `tierForCallCount()` Logic

```cpp
int AdaptiveJITRunner::tierForCallCount(int64_t count) {
    if (count >= kTier3Threshold) return 3;
    if (count >= kTier2Threshold)  return 2;
    return 0;
}
```

---

## 6. Dispatch Prolog — Code Injection & Hot-Patching

### 6.1 What `injectCounters()` Does

**File:** `aot_profile.cpp` lines 93-211

For **every** non-main, non-declaration, non-`__`-prefixed function in the module:

#### Per-Function Globals Created

```llvm
@__omsc_calls_<name> = internal global i64 0, align 8   ; call counter
@__omsc_fn_<name>    = internal global ptr null, align 8 ; hot-patch slot
@__omsc_name_<name>  = private constant [N x i8] c"<name>\00" ; name string
```

#### Per-Function Attribute Stripping

The function's `nosync`, `memory(...)`, `readnone`, `readonly` attributes are
removed because the dispatch prolog reads/writes globals and calls external
profiling functions.

#### Basic Block Structure Injected

The original function entry block is renamed and four new blocks are prepended:

```
omsc.dispatch:
  %omsc.fp = load atomic ptr, ptr @__omsc_fn_<name>, align 8, acquire
  %omsc.is_hot = icmp ne ptr %omsc.fp, null
  br i1 %omsc.is_hot, label %omsc.hot_call, label %omsc.count
  ; branch weights: 99:1 (heavily biased toward hot path)

omsc.hot_call:                          ; Fast path (after recompilation)
  %omsc.hot_r = call i64 %omsc.fp(same args as original function)
  ret i64 %omsc.hot_r

omsc.count:                             ; Cold path (first 5 calls)
  %omsc.old = load i64, ptr @__omsc_calls_<name>
  %omsc.new = add i64 %omsc.old, 1
  store i64 %omsc.new, ptr @__omsc_calls_<name>
  %omsc.hit = icmp eq i64 %omsc.new, 5   ; kTier2Threshold
  br i1 %omsc.hit, label %omsc.recompile, label %<original entry>

omsc.recompile:
  call void @__omsc_adaptive_recompile(
    ptr @__omsc_name_<name>,  ; function name string
    i64 %omsc.new,            ; call count
    ptr @__omsc_fn_<name>     ; pointer to hot-patch slot
  )
  br label %<original entry>

<original entry>:                       ; Original function body (untouched)
  ...
```

### 6.2 The Hot-Patch Mechanism

The hot-patch slot (`@__omsc_fn_<name>`) starts as `null`.  When the background
thread finishes recompiling:

1. **Writer (background thread):**
   ```cpp
   __atomic_store_n(fnPtrSlot, newPtr, __ATOMIC_RELEASE);
   ```

2. **Reader (main thread, dispatch prolog):**
   ```llvm
   %omsc.fp = load atomic ptr, ptr @__omsc_fn_<name>, align 8, acquire
   ```

The **acquire-release** pair guarantees:
- On **x86/TSO**: Both compile to plain `MOV` — zero overhead.
- On **ARM/PowerPC**: The acquire load emits an appropriate barrier, the
  release store emits a release fence. The reader is guaranteed to see the
  fully-written function pointer (no torn reads or stale nulls).

### 6.3 Call Counter

The call counter uses a **plain (non-atomic) load-increment-store** because
the main thread is the only writer.  The counter only matters for the first
5 calls; after Tier-2 hot-patching, the `omsc.dispatch` block takes the
`omsc.hot_call` fast path and never touches the counter again.

---

## 7. Runtime Profiling System

### 7.1 Architecture

**File:** `runtime/jit_profiler.cpp` / `.h`

The `JITProfiler` is a process-wide singleton that collects four categories of
runtime profile data:

| Category | Callback | Data Collected | Sampling |
|----------|----------|----------------|----------|
| Branch probabilities | `__omsc_profile_branch()` | taken/not-taken counts per branch site | 1/16 (relaxed atomic counter) |
| Argument types | `__omsc_profile_arg()` | ArgType enum + integer constant/range tracking | None (try_lock) |
| Loop trip counts | `__omsc_profile_loop()` | average/min/max/constant trip count per loop | 1/8 (relaxed atomic counter) |
| Call-site frequency | `__omsc_profile_call_site()` | caller → callee frequency map | 1/16 (relaxed atomic counter) |

**Important:** As of the current implementation, profiling callbacks are **NOT
injected** into Tier-1 code (see `injectCounters()` comment at line 142-152).
The overhead of per-branch/per-loop callbacks was too high relative to the
short Tier-1 execution window (5 calls).  Instead, Tier-2 recompilation uses
**conservative PGO heuristics** (equal branch weights, aggressive inlining of
all callees, default loop unroll factors).

### 7.2 `FunctionProfile` Structure

```cpp
struct FunctionProfile {
    std::string name;
    uint64_t callCount;
    std::vector<BranchProfile> branches;   // per-branch taken/not-taken counts
    std::vector<ArgProfile> args;          // per-parameter type/constant/range tracking
    std::vector<LoopProfile> loops;        // per-loop trip count statistics
    std::unordered_map<std::string, uint64_t> callSites; // callee frequency
    bool promoted;
};
```

### 7.3 `ArgProfile` — Constant & Range Specialisation

Each `ArgProfile` tracks:
- **Type distribution:** counts per `ArgType` (Unknown/Integer/Float/String/Array/None).
- **Constant specialisation:** If >80% of calls pass the same integer value
  (`hasConstantSpecialization()`), the Tier-2 recompiler injects
  `llvm.assume(arg == constant)` enabling constant folding.
- **Range specialisation:** If >90% of integer values fall in a tight range
  (max−min ≤ 1024), `hasRangeSpecialization()` enables `llvm.assume(arg >= min)`
  and `llvm.assume(arg <= max)` for bounds-check elimination.
- **Top-K tracking:** Misra-Gries space-saving algorithm with 4 slots tracks
  the most frequent integer values.  At Tier-3, if a single value dominates
  >60%, the recompiler uses it for specialisation.

### 7.4 `LoopProfile` — Trip Count Tracking

Each `LoopProfile` tracks:
- Average trip count: `totalIterations / executionCount`.
- Constant trip count: If every execution has the same trip count.
- Narrow range: If max−min ≤ 4 (enables aggressive unrolling).

### 7.5 Thread Safety

All profiling data is guarded by a single `std::mutex` inside `JITProfiler`.
Callbacks use `std::try_lock` to be non-blocking — if the mutex is contended,
the sample is silently dropped.  This is statistically safe because profiling
is inherently statistical; dropping a few samples has negligible impact on
branch-weight accuracy.

---

## 8. Background Recompilation — Thread Pool & Task Queue

### 8.1 Architecture

**File:** `aot_profile.cpp` lines 510-554

```
                    Main Thread                    Background Thread Pool (4 threads)
                        │                                     │
   onHotFunction() ───► │ ──── try_lock(recompiledMtx_) ──►  │
                        │      try_lock(queueMtx_)            │
                        │      taskQueue_.push(task)           │
                        │      queueCV_.notify_one()  ──────► │ ──── wait(queueCV_)
                        │                                     │      task = taskQueue_.front()
                        │                                     │      doRecompile(task)
                        │                                     │      __atomic_store_n(fnPtrSlot)
                        │ ◄── (fn-ptr visible via acquire) ── │
```

### 8.2 `onHotFunction()` — Non-Blocking Enqueue

**File:** `aot_profile.cpp` lines 472-508

Called from the dispatch prolog when `__omsc_calls_<name> == kTier2Threshold`.

1. Computes `targetTier = tierForCallCount(callCount)`.
2. **First try_lock** (`recompiledMtx_`): Checks `functionTier_[funcName]`.
   If already at target tier, returns. Otherwise, **tentatively promotes** the
   tier and saves `prevTier` for rollback.
3. **Second try_lock** (`queueMtx_`): Pushes `{funcName, callCount, fnPtrSlot}`
   onto `taskQueue_`.  **If this lock fails**, the tier promotion is **rolled
   back** (the previous tier is restored under `recompiledMtx_`) so the function
   can retry on a future threshold hit.
4. `queueCV_.notify_one()` wakes one background thread.

**Critical invariant:** If the enqueue succeeds, the tier is promoted. If the
enqueue fails, the tier is rolled back. This prevents "lost recompilations"
where a function is marked as promoted but never actually enqueued.

### 8.3 `backgroundWorker()` — Worker Loop

```cpp
void AdaptiveJITRunner::backgroundWorker() {
    while (true) {
        RecompileTask task;
        {
            std::unique_lock<std::mutex> lk(queueMtx_);
            queueCV_.wait(lk, [this] {
                return !taskQueue_.empty() || shutdownRequested_.load();
            });
            if (taskQueue_.empty() && shutdownRequested_.load())
                return;
            task = std::move(taskQueue_.front());
            taskQueue_.pop();
        }
        try {
            doRecompile(task.funcName, task.callCount, task.fnPtrSlot);
        } catch (...) {
            // Log error, don't crash the background thread
        }
    }
}
```

### 8.4 `drainBackgroundThread()` — Graceful Shutdown

Called by the RAII `RunnerGuard` when `main()` returns:
1. Sets `shutdownRequested_ = true`.
2. Notifies all worker threads via `queueCV_.notify_all()`.
3. Joins all threads.
4. Clears the thread vector.

This ensures all pending compilations complete before JIT module destruction.

---

## 9. PGO-Guided Tiered Recompilation (`doRecompile()`)

### 9.1 Overview

**File:** `aot_profile.cpp` lines 562-1147

This is the most complex function in the codebase.  It runs on a background
thread and performs the following steps for each hot function:

### 9.2 Step-by-Step Breakdown

#### Step 1: RAII Scope Guard (lines 579-597)

```cpp
bool succeeded = false;
int prevTier = functionTier_[funcName];
struct ScopeExit { ... ~ScopeExit() { if (!flag) tierMap[key] = rollbackTier; } };
```

If `doRecompile()` fails at any point, the scope guard reverts `functionTier_`
to `prevTier`, allowing future threshold hits to retry.

#### Step 2: Parse Clean Bitcode (lines 599-624)

```cpp
auto newCtx = std::make_unique<llvm::LLVMContext>();
// Suppress diagnostics in non-verbose mode for speed
auto mod = parseBitcodeFile(cleanBitcode_, *newCtx);
```

Each recompilation gets a fresh `LLVMContext` + `Module`.  This is critical
because LLVM contexts are NOT thread-safe — each background thread must
have its own context.

#### Step 3: PGO Entry-Count Annotation (lines 626-647)

```cpp
fn->setEntryCount(static_cast<uint64_t>(callCount));
fn->addFnAttr(llvm::Attribute::Hot);
for (auto& other : *mod)
    if (!other.getEntryCount()) other.setEntryCount(1);
```

- The hot function gets its actual call count as the entry count.
- All other functions get entry count 1 (cold by comparison).
- The `hot` attribute enables hot/cold splitting and aggressive inlining.

#### Step 4: Branch Weight Metadata (lines 660-690)

If the `JITProfiler` has branch data for this function, branch weights are
attached as LLVM metadata (`MD_prof`) on conditional branches.  This guides:
- Code layout (hot side → fall-through path for I-cache density)
- Branch predictor hints
- Hot/cold splitting

#### Step 5: Argument Type Attributes (lines 692-719)

If profiling shows >90% of calls pass integer values, parameters get:
- `noundef`: Value is always well-defined (enables range propagation)
- `signext`: Value is sign-extended (better instruction selection)

#### Step 6: Constant Specialisation via `llvm.assume` (lines 721-764)

For parameters where >80% of calls pass the same constant:
```llvm
%cmp = icmp eq i64 %arg, <constant>
call void @llvm.assume(i1 %cmp)
```

LLVM's IPSCCP and instcombine propagate the assumed constant through the
function body, enabling dead-branch elimination and constant folding.

At Tier-3, the top-K tracker is also consulted: if a value dominates >60%
of calls, it gets the same `llvm.assume` treatment.

#### Step 7: Range Specialisation (lines 766-807)

For parameters where >90% of integer values fall within [min, max] (width ≤ 1024):
```llvm
%ge = icmp sge i64 %arg, <min>
call void @llvm.assume(i1 %ge)
%le = icmp sle i64 %arg, <max>
call void @llvm.assume(i1 %le)
```

This enables bounds-check elimination and narrower integer operations.

#### Step 8: Aggressive Callee Inlining (lines 809-833)

ALL direct callees of the hot function are marked `alwaysinline`:
```cpp
callee->addFnAttr(llvm::Attribute::AlwaysInline);
callee->removeFnAttr(llvm::Attribute::Cold);
callee->removeFnAttr(llvm::Attribute::NoInline);
```

This is safe because OmScript programs are typically small, and inlining
exposes cross-function constant folding, dead code elimination, and
loop optimisation opportunities.

#### Step 9: Strip Unreachable Functions (lines 835-865)

A BFS from the hot function discovers all transitively reachable callees.
All other functions are turned into declarations (bodies deleted).  This
dramatically reduces the work the O3 pipeline has to do (often 5-10×
fewer functions to analyze).

#### Step 10: Cold-Mark Non-Hot Functions (lines 867-876)

Reachable but non-hot functions are marked `cold`, guiding LLVM to:
- Avoid inlining them into the hot function
- Place their code away from the hot region (better I-cache density)

#### Step 11: Loop Vectorisation & PGO-Guided Unrolling Metadata (lines 878-1003)

For every loop back-edge in the hot function:
1. Attaches `llvm.loop.mustprogress`.
2. Attaches `llvm.loop.interleave.count = 8`.
3. Attaches `llvm.loop.vectorize.width = 8`.
4. If loop trip-count profile data is available:
   - **Constant trip count ≤ 32:** Full unroll (`llvm.loop.unroll.full` + `unroll.count`).
   - **Constant trip count 33-64:** Unroll by 8.
   - **Narrow range ≤ 32:** Full unroll to max trip count.
   - **Average ≤ 16:** Full unroll.
   - **Average 17-64:** Unroll by 8.
   - **Average > 64:** Unroll by 16 + `llvm.loop.vectorize.enable = true`.

#### Step 12: Fast-Math Flag Propagation (lines 1005-1025)

If `-ffast-math` is enabled, sweeps all FP instructions in the hot function
and sets `nnan/ninf/nsz/arcp/contract/afn/reassoc` flags.  This enables FMA
fusion, reciprocal estimation, and reassociation.

#### Step 13: O3 Pipeline Execution (lines 1027-1092)

```cpp
llvm::PipelineTuningOptions PTO;
PTO.InlinerThreshold = 2000;  // Very aggressive (vs AOT's 400)
PTO.LoopVectorization = vectorize_;
PTO.SLPVectorization = vectorize_;
PTO.LoopUnrolling = unrollLoops_;

auto TM = createTargetMachine();
mod->setDataLayout(TM->createDataLayout());

llvm::PassBuilder PB(TM.get(), PTO);
// Register LoopDistributePass if -floop-optimize
auto pipeline = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
pipeline.run(*mod, MAM);
```

Key differences from AOT O3:
- **InlinerThreshold = 2000** (vs AOT's 400): JIT can afford this because it
  only processes the hot function + callees (stripped module).
- **LoopDistributePass:** Splits loops with independent memory streams for
  better vectorisation.
- **Per-function TargetMachine:** Each background thread creates its own
  TargetMachine (they are NOT thread-safe). Host CPU/features are cached
  via `std::call_once` and reused.

#### Step 14: JIT-Compile (lines 1103-1131)

```cpp
llvm::EngineBuilder eb(std::move(mod));
eb.setOptLevel(llvm::CodeGenOptLevel::Aggressive);  // Maximum backend opt
engine->finalizeObject();
uint64_t addr = engine->getFunctionAddress(funcName);
modules_.push_back({std::move(newCtx), std::move(engine)});
```

The recompiled module is stored in `modules_` to keep the native code alive
for the process lifetime.

#### Step 15: Atomic Hot-Patch (lines 1137-1144)

```cpp
void* newPtr = reinterpret_cast<void*>(addr);
__atomic_store_n(fnPtrSlot, newPtr, __ATOMIC_RELEASE);
```

The release store synchronises with the dispatch prolog's acquire load.
After this write, the very next call to the function will take the
`omsc.hot_call` fast path and execute the optimised code.

---

## 10. Deoptimization Framework

### 10.1 Purpose

**File:** `runtime/deopt.cpp` / `.h`

When Tier-2 code is specialised based on profiling assumptions (e.g. "argument
0 is always an integer"), guard checks are inserted:

```
if (typeof(arg0) != INTEGER) goto deopt;
```

If the guard fails at runtime, the deoptimisation callback fires.

### 10.2 `DeoptManager` Mechanism

```cpp
void DeoptManager::onGuardFailure(const char* funcName, void** fnPtrSlot) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& count = failures_[funcName];
    count++;
    if (count >= kDeoptThreshold) {  // kDeoptThreshold = 10
        *fnPtrSlot = nullptr;  // Reset hot-patch slot
        // Function reverts to Tier-1 baseline code
    }
}
```

After 10 guard failures:
1. The hot-patch slot is reset to `nullptr`.
2. The dispatch prolog's `omsc.dispatch` block sees `null`, takes the
   `omsc.count` path, and falls through to the original Tier-1 function body.
3. The function is **never re-promoted** (no mechanism to clear the deopt flag).

### 10.3 C-Linkage Callback

```cpp
extern "C" void __omsc_deopt_guard_fail(const char* funcName, void** fnPtrSlot);
```

Registered with MCJIT via `addGlobalMapping()` so JIT-compiled guard code can
call it directly.

---

## 11. Value System & Reference Counting

### 11.1 Value Type (`runtime/value.h`)

All OmScript values are represented by the `Value` class, which uses a tagged
union:

```cpp
class Value {
    enum class Type { INTEGER, FLOAT, STRING, NONE };
    Type type;
    union {
        int64_t intValue;
        double floatValue;
        RefCountedString stringValue;
    };
};
```

- `INTEGER`: 64-bit signed integer.
- `FLOAT`: 64-bit IEEE 754 double.
- `STRING`: Reference-counted string (see below).
- `NONE`: Null/undefined.

At the LLVM IR level, all values are represented as `i64`.  String values are
pointers to `RefCountedString` data cast to `i64`.

### 11.2 Reference Counting (`runtime/refcounted.h`)

```cpp
struct StringData {
    std::atomic<size_t> refCount;
    size_t length;
    char chars[1];  // Flexible array member
};
```

- **Allocation:** `malloc(sizeof(StringData) + length)` — single allocation for
  metadata + character data.
- **Retain:** `refCount.fetch_add(1, std::memory_order_relaxed)` — relaxed is
  safe because the caller already holds a reference (guaranteed non-zero count).
- **Release:** `refCount.fetch_sub(1, std::memory_order_acq_rel)` — acquire-
  release ensures all prior writes are visible before deallocation.  If the
  decremented count reaches 0, `free(data)` is called.
- **Empty strings:** Represented as `data == nullptr` (no allocation needed).
  `c_str()` returns `""` for null data.

### 11.3 Value Semantics

- Copy constructor: For non-STRING types, a simple `intValue` copy (covers
  INTEGER, FLOAT, NONE via union aliasing). For STRING, placement-new copies
  the `RefCountedString` (incrementing refcount).
- Move constructor: Transfers string ownership, nullifies source.
- Destructor: Only calls `stringValue.~RefCountedString()` for STRING type
  (branch predicted unlikely with `__builtin_expect`).

---

## 12. Memory Ordering & Thread Safety Guarantees

### 12.1 Hot-Patch Slot

| Operation | Thread | Ordering | Guarantee |
|-----------|--------|----------|-----------|
| Load fn-ptr | Main (dispatch prolog) | `acquire` | Sees the release store's value |
| Store fn-ptr | Background (doRecompile) | `release` | Publishes the pointer fully |
| Store nullptr | Deopt (onGuardFailure) | Plain (under mutex) | Single-threaded deopt path |

### 12.2 Function Tier Map

`functionTier_` is guarded by `recompiledMtx_` (a regular mutex).  Both the
main thread (via `onHotFunction()`) and background threads (via `doRecompile()`)
access it under lock.

### 12.3 Task Queue

`taskQueue_` is guarded by `queueMtx_`.  `onHotFunction()` uses `try_lock`
to be non-blocking; background workers use blocking `wait()`.

### 12.4 Modules Vector

`modules_` is guarded by `recompiledMtx_`.  Both the main thread (Tier-1
module registration) and background threads (Tier-2 module registration)
append under lock.

### 12.5 Profiler Data

The `JITProfiler` uses a single `std::mutex` with `try_lock` on the hot
path.  Dropped samples are statistically insignificant.

### 12.6 Refcount Operations

- `retain()`: `fetch_add(1, relaxed)` — safe because caller holds a live reference.
- `release()`: `fetch_sub(1, acq_rel)` — ensures visibility before deallocation.

---

## 13. Non-JIT `run` Path (AOT Fallback)

When `omsc run` is invoked with `-fno-jit`, the fallback AOT path
(the `Command::Run` branch inside the `Compiler::compile()` fallback in `main.cpp`) is used:

1. `Compiler::compile()` performs the full AOT pipeline (codegen → object → linker).
2. The compiled binary is written to a temporary file.
3. `llvm::sys::ExecuteAndWait()` spawns the binary as a subprocess.
4. Exit code is propagated.
5. Temporary files are cleaned up (unless `--keep-temps`).

This is the same path as `omsc build` + run, but automated.

---

## 14. Full Pipeline Diagrams

### 14.1 AOT Pipeline (`omsc build`)

```
  ┌──────────┐    ┌───────┐    ┌────────┐    ┌────────────────┐
  │ Source   │───>│ Lexer │───>│ Parser │───>│ AST            │
  │ (.om)    │    │       │    │        │    │                │
  └──────────┘    └───────┘    └────────┘    └───────┬────────┘
                                                     │
                                                     ▼
                                             ┌────────────────┐
                                             │ CodeGenerator  │
                                             │ .generate()    │
                                             │ (LLVM IR)      │
                                             └───────┬────────┘
                                                     │
                                                     ▼
                                             ┌────────────────────────┐
                                             │ runOptimizationPasses()│
                                             │ (New PM: O1/O2/O3)    │
                                             │ + Inlining, IPSCCP,   │
                                             │   GlobalDCE, LoopVec, │
                                             │   SLP Vec, Unrolling  │
                                             │ + PGO (if --pgo-use)  │
                                             │ + LoopDistribute (O3) │
                                             └───────┬────────────────┘
                                                     │
                                              ┌──────┴──────┐
                                              │             │
                                              ▼             ▼
                                        ┌──────────┐  ┌──────────┐
                                        │ .o file  │  │ .bc file │
                                        │ (native) │  │ (LTO)    │
                                        └────┬─────┘  └────┬─────┘
                                             │             │
                                             ▼             ▼
                                        ┌─────────────────────┐
                                        │ System Linker       │
                                        │ (gcc/cc/clang)      │
                                        │ + -O2/-O3, -lm      │
                                        │ + -flto (if enabled) │
                                        │ + -static, -s       │
                                        └────────┬────────────┘
                                                 │
                                                 ▼
                                           ┌──────────┐
                                           │ Binary   │
                                           │ (ELF)    │
                                           └──────────┘
```

### 14.2 JIT Pipeline (`omsc run`)

```
  ┌──────────┐    ┌───────┐    ┌────────┐    ┌────────────────┐
  │ Source   │───>│ Lexer │───>│ Parser │───>│ AST            │
  │ (.om)    │    │       │    │        │    │                │
  └──────────┘    └───────┘    └────────┘    └───────┬────────┘
                                                     │
                                                     ▼
                                             ┌────────────────────┐
                                             │ CodeGenerator      │
                                             │ .generateHybrid()  │
                                             │ (LLVM IR, no IPO)  │
                                             └───────┬────────────┘
                                                     │
                                                     ▼
                                             ┌──────────────────────────┐
                                             │ runJITBaselinePasses()   │
                                             │ (Per-function only:     │
                                             │  mem2reg, GVN, LICM,    │
                                             │  LoopUnroll, DCE, etc.) │
                                             └───────┬──────────────────┘
                                                     │
                                         ┌───────────┴───────────┐
                                         │                       │
                                         ▼                       ▼
                              ┌────────────────────┐  ┌──────────────────┐
                              │ Serialise to       │  │ Clone module     │
                              │ cleanBitcode_      │  │ (fresh context)  │
                              │ (for Tier-2 recomp)│  │                  │
                              └────────────────────┘  └────────┬─────────┘
                                                               │
                                                               ▼
                                                    ┌────────────────────┐
                                                    │ injectCounters()   │
                                                    │ (dispatch prologs) │
                                                    └────────┬───────────┘
                                                             │
                                                             ▼
                                                    ┌────────────────────┐
                                                    │ MCJIT Compile      │
                                                    │ (O0 backend)       │
                                                    │ = Tier-1 native    │
                                                    └────────┬───────────┘
                                                             │
                                        ┌────────────────────┤
                                        │                    │
                                        ▼                    ▼
                               ┌──────────────┐    ┌──────────────────┐
                               │ Start 4 BG   │    │ Eager Enqueue    │
                               │ worker threads│    │ ALL functions    │
                               │              │    │ for Tier-2       │
                               └──────┬───────┘    └────────┬─────────┘
                                      │                     │
                                      │    ┌────────────────┘
                                      │    │
                                      ▼    ▼
                               ┌──────────────────┐
                               │ Execute main()   │  ◄── Tier-1 code running
                               │ in-process       │
                               └──────────────────┘
                                      │
                                      │ (background, concurrent)
                                      ▼
                               ┌──────────────────────────────────────┐
                               │ doRecompile() per function:          │
                               │  1. Parse cleanBitcode_              │
                               │  2. PGO entry-count annotation       │
                               │  3. Branch weight metadata           │
                               │  4. Arg type attributes              │
                               │  5. Constant specialisation          │
                               │  6. Range specialisation             │
                               │  7. Aggressive callee inlining       │
                               │  8. Strip unreachable functions      │
                               │  9. Cold-mark non-hot functions      │
                               │ 10. Loop vec/unroll metadata         │
                               │ 11. Fast-math propagation            │
                               │ 12. O3 pipeline (InlinerThreshold=2000)│
                               │ 13. MCJIT compile (Aggressive backend)│
                               │ 14. Atomic store fn-ptr (RELEASE)    │
                               └──────────────────────────────────────┘
                                      │
                                      ▼
                               ┌──────────────────┐
                               │ Dispatch prolog   │
                               │ sees non-null     │
                               │ fn-ptr (ACQUIRE)  │
                               │ → hot_call path   │
                               │ → optimised code  │
                               └──────────────────┘
```

### 14.3 Per-Function Dispatch State Machine

```
         ┌─────────────────────────────────────────────────────────┐
         │                    FUNCTION LIFETIME                     │
         │                                                         │
         │  Call 1-4:  omsc.dispatch → omsc.count → omsc.body     │
         │             (Tier-1 baseline code, counter incrementing) │
         │                                                         │
         │  Call 5:    omsc.dispatch → omsc.count → omsc.recompile │
         │             → __omsc_adaptive_recompile() enqueued       │
         │             → omsc.body (still Tier-1 this call)        │
         │                                                         │
         │  [Background: doRecompile() running...]                 │
         │                                                         │
         │  Call 6+:   omsc.dispatch → omsc.hot_call               │
         │  (if recompile finished)  → optimised Tier-2 code       │
         │                                                         │
         │  OR                                                     │
         │                                                         │
         │  Call 6+:   omsc.dispatch → omsc.count → omsc.body     │
         │  (if recompile still running) → Tier-1 baseline         │
         │                                                         │
         │  [Guard failure × 10:]                                  │
         │  __omsc_deopt_guard_fail() → fnPtrSlot = nullptr        │
         │  → Reverts to Tier-1 permanently                        │
         └─────────────────────────────────────────────────────────┘
```

---

## Summary of Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| No interpreter/bytecode tier | LLVM MCJIT at O0 backend is fast enough for first-call latency |
| 5-call Tier-2 threshold | Balances startup speed vs. wasted Tier-1 execution |
| Eager compilation of ALL functions | Eliminates threshold-triggered latency for most functions |
| 4 background threads | Parallelises compilation across available cores |
| Per-function (not IPO) baseline passes | Preserves function boundaries for hot-patching |
| Acquire/Release (not Monotonic) for fn-ptr | Correct on all architectures (ARM, PowerPC, x86) |
| try_lock for non-blocking enqueue | Never stalls the main execution thread |
| Tier rollback on failed enqueue | Prevents lost recompilations |
| Strip unreachable functions before O3 | 5-10× fewer functions → faster recompilation |
| InlinerThreshold=2000 for JIT | Can afford aggressive inlining on stripped module |
| Deoptimization via guard failures | Safety net for speculative optimisations |
| Reference-counted strings with acq_rel | Thread-safe deallocation without full mutex |
