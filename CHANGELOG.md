# Changelog

All notable changes to the OmScript compiler will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [3.7.0] - 2026-03-23

### Added
- **`lcm(a, b)` built-in function:** Computes the Least Common Multiple using `|a| / gcd(a, b) * |b|` (divide-first to avoid overflow). Handles negative inputs and `lcm(0, x) = 0` correctly.
- **2 new benchmark categories in test.sh:**
  - `float_math` (benchmark 16) — exercises floating-point operations (sqrt, exp2, pow) to test FP optimization rules
  - `bitwise_intrinsics` (benchmark 17) — exercises new v3.6.0 builtins (popcount, clz, ctz, is_power_of_2) against C `__builtin_*` equivalents
- **3 new unit tests** for `lcm` builtin (codegen_test.cpp)
- **New example program** `lcm_test.om` demonstrating LCM with integration test in run_tests.sh

### Changed
- **test.sh benchmark improvements:**
  - Added **geometric mean** of per-benchmark ratios for the overall score — more meaningful than arithmetic aggregate because it treats all benchmarks equally regardless of absolute time
  - Correctness check now doubles as **warmup run** for I-cache and D-cache, improving timing stability
  - Added **cleanup** of generated temp files (bench.om, bench.c, bench_om, bench_c) at script exit
  - Added `-lm` to C compilation flags for math library linking (required by float_math benchmark)
  - Increased benchmark count from 16 to 18

## [3.6.0] - 2026-03-23

### Added
- **6 new built-in functions** for bitwise operations and math, all using LLVM hardware intrinsics for maximum performance:
  - `popcount(x)` — count set bits (uses `ctpop` intrinsic, compiles to single `POPCNT` instruction on x86)
  - `clz(x)` — count leading zeros (uses `ctlz` intrinsic, compiles to `LZCNT`/`BSR`)
  - `ctz(x)` — count trailing zeros (uses `cttz` intrinsic, compiles to `TZCNT`/`BSF`)
  - `bitreverse(x)` — reverse all bits (uses `bitreverse` intrinsic)
  - `exp2(x)` — base-2 exponential, 2^x (uses `exp2` intrinsic for native FP hardware)
  - `is_power_of_2(x)` — efficient power-of-2 check via `x > 0 && (x & (x-1)) == 0`
- **12 new e-graph floating-point optimization rules** for faster FP code generation:
  - Division by power-of-2 constants to multiply by reciprocal: `x / 4.0 → x * 0.25`, `x / 8.0 → x * 0.125`, `x / 16.0 → x * 0.0625` (and reverses for e-graph exploration)
  - FP double negation elimination: `-(-x) → x` (exact in IEEE-754)
  - FP subtract/add zero: `x - 0.0 → x`, `0.0 + x → x`, `x + 0.0 → x`
- **3 new e-graph relational optimization rules:**
  - Modulo by power-of-2 for non-negative values: `x % C → x & (C-1)` (guarded by `isNonNeg`)
  - Algebraic factoring: `x*a + x*b → x*(a+b)` and `x*a - x*b → x*(a-b)` where a, b are constants
- **21 new unit tests** covering all new builtins and optimization rules
- **New example program** `bitwise_builtins_test.om` demonstrating all new built-in functions

## [3.5.0] - 2026-03-23

### Added
- **HGOE instruction fusion detection:** The scheduler now detects and exploits three classes of instruction fusion opportunities:
  - **Compare+Branch (CmpBranch):** `ICmp`/`FCmp` followed by conditional branch — macro-op fusion on Skylake+, Zen+, Apple M-series
  - **Load+Op (LoadOp):** Single-use loads feeding ALU operations — micro-op folding allows the load to be absorbed into the ALU op's memory operand
  - **Address folding (AddrFold):** GEP + Load/Store pairs — address calculation folds into the memory instruction's addressing mode
  - Fusion partners are scheduled adjacently via a new fusion-affinity tier in the priority sort
- **HGOE register pressure tracking:** The scheduler now models approximate register liveness during scheduling:
  - Tracks live-in values from outside the basic block (function arguments, cross-BB definitions)
  - Each scheduled instruction that produces a non-void result increases live count; when all consumers are scheduled, the value dies
  - A **register pressure penalty** tier in the priority sort penalises instructions that would push live values over the target's physical register budget (adjusted for stack/frame pointer)
  - Integrates with the existing register-freeing score for spill-avoidance scheduling
- **HGOE beam search pruning:** For basic blocks with more than 32 ready-to-schedule instructions, only the top 32 candidates (by priority score) are considered each cycle, preventing combinatorial explosion in very large functions while maintaining schedule quality
- **HGOE schedule DAG visualization/debug hooks:**
  - `dumpScheduleDAG()` — prints the dependency DAG with per-instruction latency, critical-path depth, resource class, and predecessor/successor lists
  - `dumpScheduleResult()` — prints the final schedule order with cycle assignments
  - Activated by setting `OMSC_DUMP_SCHEDULE=1` in the environment — zero overhead when disabled (cached check)
- **6 new HGOE unit tests:**
  - `ScheduleReordersInstructions` — verifies instruction reordering for latency hiding
  - `ScheduleRegisterPressureAware` — tests register pressure tracking with many live values
  - `ScheduleHandlesLargeBB` — tests beam search pruning on 100-instruction blocks
  - `ScheduleFusionAware` — tests fusion-aware scheduling with compare+select
  - `ScheduleWithDivisionLatencyHiding` — verifies division is scheduled before independent adds
  - `ScheduleMultiArchConsistency` — tests same BB on Skylake, Zen 4, Apple M1

### Changed
- **HGOE scheduler priority sort upgraded to 9-tier** (from 7-tier):
  1. Critical-path depth (latency hiding)
  2. Long-latency operations first (division, FP divide)
  3. Loads first (hide memory latency)
  4. Stall distance (consumer work remaining)
  5. **Fusion affinity** (schedule fusion partners adjacently — new)
  6. Port pressure (bottleneck resource first)
  7. **Register pressure penalty** (penalise spill-causing schedules — new)
  8. Register-freeing score (reduce live values)
  9. Instruction index (deterministic tie-break)

## [3.4.0] - 2026-03-23

### Added
- **13 new e-graph relational rewrite rules** using guard predicates for targeted constant optimisation:
  - **6 multiply-by-constant strength reduction rules:** `x * 3 → (x << 1) + x`, `x * 5 → (x << 2) + x`, `x * 7 → (x << 3) - x`, `x * 9 → (x << 3) + x`, `x * 15 → (x << 4) - x`, `x * 17 → (x << 4) + x` — each guarded by a relational predicate that matches only the specific constant, converting expensive multiplies to 2-cycle shift+add sequences
  - **2 shift-combining rules:** `(x << a) << b → x << (a + b)` and `(x >> a) >> b → x >> (a + b)` — guarded to require both shift amounts be constants with sum < 64
  - **5 ternary/select optimisation rules:**
    - `cond ? x : x → x` (redundant select elimination)
    - `cond ? 1 : 0 → cond != 0` (boolean select)
    - `cond ? 0 : 1 → cond == 0` (inverted boolean select)
    - `!(cond) ? a : b → cond ? b : a` (negate condition = swap arms)
    - `(a == b) ? a : b → b` (select on equality)

### Changed
- **HGOE scheduler: long-latency operation hoisting** — Integer and FP divisions are now scheduled before other ready instructions at the same critical-path depth, allowing independent ALU/load work to proceed while the divider pipeline is busy. This outperforms LLVM's generic MachineScheduler which lacks per-CPU division latency data (our profiles model exact divider latency: 25 cycles on Skylake, 15 on Zen 5, 12 on Graviton3)
- **HGOE scheduler: stall-distance heuristic** — When multiple instructions are ready at the same priority level, the scheduler now prefers instructions whose consumers have the most remaining critical-path work, maximising the opportunity to fill stall cycles with independent computation
- **HGOE scheduler: 7-tier priority** (up from 5) — New priority levels:
  1. Critical-path depth (latency hiding)
  2. **Long-latency operations first** (division, FP divide — new)
  3. Loads first (hide memory latency)
  4. **Stall distance** (consumer work remaining — new)
  5. Port pressure (bottleneck resource first)
  6. Register-freeing score (reduce live values)
  7. Instruction index (deterministic tie-break)

## [3.3.0] - 2026-03-22

### Added
- **Relational e-graph pattern matching:** The e-graph equality saturation engine now supports **guard predicates** on rewrite rules, enabling relational optimizations where structural patterns are refined by semantic constraints on matched values. New infrastructure:
  - `RuleGuard` predicate type on `RewriteRule` — checked before applying a rewrite
  - `EGraph::getConstValue()` / `getConstFValue()` — extract constant values from e-classes for guard evaluation
- **23 new e-graph rewrite rules:**
  - **3 relational power-of-2 rules:** `x * C → x << log2(C)` and `x % C → x & (C-1)` for ANY power-of-2 constant (generalises the hardcoded rules for 2..1024)
  - **4 comparison merging rules:** `(a < b) || (a == b) → a <= b`, `(a > b) || (a == b) → a >= b`, and commuted variants — eliminates redundant comparison pairs
  - **4 redundant comparison elimination rules:** `(a != b) && (a < b) → a < b`, etc. — removes tautological conjuncts
  - **4 ternary common-subexpression factoring rules:** `cond ? (a+c) : (b+c) → (cond ? a : b) + c`, and similarly for Sub, Mul, Neg — hoists common operations out of branches
  - **4 complement-based bitwise rules:** `(~a) & (a | b) → (~a) & b`, `(~a) | (a & b) → (~a) | b`, and commuted variants — simplifies complement interactions using Boolean algebra
  - **4 additional commuted variants** for the above rules
- **13 additional e-graph rules:**
  - **5 Sqrt identities:** `sqrt(0)→0`, `sqrt(1)→1`, `sqrt(x)*sqrt(x)→x`, `sqrt(x²)→x`, `sqrt(x)*sqrt(y)→sqrt(x*y)`
  - **5 Power rules:** `x^1→x`, `x^(-1)→1/x`, `x^a * x^b → x^(a+b)`, `(x^a)^b → x^(a*b)`, `x^a / x^b → x^(a-b)`
  - **2 guard-predicated rules** using the new relational infrastructure
- **4 new HGOE CPU microarchitecture profiles:**
  - **AWS Graviton3** (Arm Neoverse V1, 8-wide, SVE 256-bit, DDR5)
  - **AWS Graviton4** (Arm Neoverse V2, 8-wide, SVE2 256-bit, 2 MB L2, DDR5-5600)
  - **Intel Lunar Lake** (Lion Cove P-cores, 8-wide decode/dispatch, 6 ALU, 3 load ports, 2.5 MB L2)
  - **AMD Zen 5** — dedicated profile (replaces the previous zen4-derived approximation) with accurate 8-wide dispatch, 6 ALU pipes, 4 vector units, 4 FMA units, 4 load ports, AVX-512, 12-cycle branch mispredict penalty

### Changed
- **E-graph `applyRules()` now checks guard predicates** before applying each rewrite match, enabling fine-grained relational filtering without false merges
- **AMD Zen 5 HGOE scheduling** now uses its own dedicated profile function (`zen5Profile()`) instead of a minimal derivation from Zen 4, giving more accurate instruction scheduling, port assignment, and throughput modelling
- **HGOE scheduler uses alias-aware memory dependencies** instead of conservatively serialising all memory operations — independent loads can now execute in parallel on separate load ports, significantly improving IPC on wide-issue CPUs (Graviton3/4, Zen 5, Lunar Lake)
- **HGOE scheduler prioritises loads** in the scheduling sort to hide memory latency — loads are scheduled as early as possible before dependent ALU operations

## [3.2.0] - 2026-03-22

### Changed
- **Zero-cost abstraction guarantees:**
  - Lambda functions (`|x| expr`) are now marked `alwaysinline` at O2+, guaranteeing zero call overhead when passed to `array_map`, `array_filter`, `array_reduce`, and other higher-order functions
  - Small helper functions (≤4 deep statements) get `alwaysinline` at O2 (previously only at O3 with ≤8 threshold), ensuring struct accessors, predicates, and single-operation helpers are fully inlined
  - `InferFunctionAttrsPass` lowered from O2+ to O1+, so `nocapture`, `readonly`, `nounwind`, and other library-function attributes are inferred at all optimization levels
- **Fixed cross-platform release build failures:** Removed incorrect `LLVM_VERSION_MAJOR >= 21` version guards that assumed a `ThinOrFullLTOPhase` parameter in `PassBuilder` EP callbacks — this parameter was never added to the LLVM API, causing compilation failures on macOS (LLVM 22) and Windows (MSYS2 LLVM)
- **Fixed deprecated `moveBefore(Instruction*)` call** in hardware graph instruction scheduler to use the iterator-based API, eliminating deprecation warnings on LLVM 18+

## [3.1.0] - 2026-03-22

### Added
- **6 new superoptimizer algebraic simplification patterns:**
  - **De Morgan's laws:** `~(a & b) → (~a) | (~b)` and `~(a | b) → (~a) & (~b)` — exposes further simplification opportunities when one of the operands is a known constant or can be folded
  - **Boolean select→zext:** `select(cond, 1, 0) → zext(cond)` and `select(cond, 0, 1) → zext(!cond)` — eliminates conditional branches for boolean-to-integer conversions
  - **Redundant truncation elimination:** `and(zext(x), mask) → zext(x)` when x's source type fits within the mask — removes no-op masking operations
  - **Absolute value recognition:** `select(a > 0, a, -a)` and `select(a < 0, -a, a)` → branchless abs via `(x ^ (x >> 31)) - (x >> 31)` — eliminates a conditional branch per abs() call
  - **Low-bit comparison simplification:** `icmp ne (and x, 1), 0 → trunc x to i1` — reduces instruction count for boolean testing patterns
- **29 new e-graph rewrite rules:**
  - **Modulo-by-power-of-2 conversion:** `x % 2 → x & 1`, `x % 4 → x & 3`, ..., `x % 1024 → x & 1023` — replaces expensive remainder operations with cheap bitwise AND for power-of-2 divisors (10 rules)
  - **Comparison-with-subtraction elimination:** `(x - y) < 0 → x < y`, `(x - y) > 0 → x > y`, `(x - y) <= 0 → x <= y`, `(x - y) >= 0 → x >= y` — removes intermediate subtraction in comparison chains (4 rules)
  - **Zero-comparison to boolean:** `x == 0 → !x`, `x != 0 → !!x` — normalizes zero-testing to logical operations for better downstream simplification (2 rules)
  - **Multiply-add coalescing:** `x*2+x → x*3`, `x*4+x → x*5`, `x*8-x → x*7`, `x*8+x → x*9`, `x*16-x → x*15`, `x*16+x → x*17` — folds multiply+add/sub sequences into a single multiply, enabling subsequent shift-based strength reduction (9 rules with commutative variants)
  - **Shift-mask interaction:** `(x >> a) << a → x & ~((1<<a)-1)` and `(x << a) >> a → x & ((1<<(64-a))-1)` — replaces double-shift sequences with a single AND against a constant mask, exposing further AND-merging opportunities (12 rules for shifts 1–6)
- **Count Leading Zeros (CLZ) idiom detection** in superoptimizer — recognizes the bit-smear + popcount CLZ pattern (`64 - popcount(x | (x>>1) | ...)`) and replaces it with the `llvm.ctlz` intrinsic
- **Count Trailing Zeros (CTZ) idiom replacement** — the existing `x & (-x)` isolation detection now emits `llvm.cttz` intrinsic for direct hardware CTZ support
- **Bit-field extract idiom replacement** — shift-and-mask patterns now emit optimized shift+AND sequences

### Changed
- **Post-recursive-inlining cleanup (O3):** After bounded recursive inlining, a GVN + InstCombine + CFGSimp + DCE pass now runs to eliminate redundant computations and dead code created by manual inlining. This catches duplicate computations that appear in both the original call site and inlined body.
- **Early InferFunctionAttrs registration (O2+):** `InferFunctionAttrsPass` now runs early in the pipeline to annotate library functions with known attributes (noalias, nocapture, readonly, etc.) before the inliner and alias analysis passes, improving cost estimates and enabling more aggressive optimizations downstream.
- **Dead argument elimination (O3):** `DeadArgumentEliminationPass` now runs late in the O3 pipeline to remove unused function parameters after inlining and constant propagation, reducing register pressure and enabling further inlining with smaller call signatures.
- **Enhanced final cleanup pass:** GVN pass added before the final DCE + InstCombine + CFGSimp cleanup to catch redundant computations exposed by the superoptimizer and HGOE passes.
- **Stronger OPTMAX function optimization:** Added `AggressiveInstCombine` and `BDCE` (Bit-tracking Dead Code Elimination) passes to the OPTMAX pipeline, catching multi-instruction patterns and unused-bit elimination that regular InstCombine misses.
- **Reassociate + EarlyCSE at O1+:** Expression trees are canonicalized and trivially redundant computations are eliminated early in the pipeline via `ReassociatePass` and `EarlyCSEPass`, reducing IR size for downstream passes.
- **SCCP at O2+:** Sparse Conditional Constant Propagation now runs late in the scalar optimizer to discover constants flowing through control-flow edges, proving some branches unreachable and some PHI values constant.

## [3.0.0] - 2026-03-22

### Added
- **String interpolation** `$"..."` — embed expressions directly in string literals using `$"hello {name}, you are {age} years old"`. Expressions inside `{...}` are evaluated and auto-converted to strings. Supports nested expressions, function calls, ternary operators, and escaped braces `\{`/`\}`. Desugars into `+` concatenation at the lexer level for zero runtime overhead.
- **Multi-value switch case** `case 1, 2, 3:` — match multiple comma-separated values in a single case arm. All values map to the same basic block. Duplicate detection spans across multi-value cases.
- **Constant switch condition elimination** — when the switch condition is a compile-time constant, the compiler eliminates all dead case branches and directly generates only the matched case body (or default), removing the switch instruction entirely.
- **`str_join(arr, delim)` built-in** — join an array of strings into a single string with a delimiter between elements. Inverse of `str_split`. Supports empty delimiters and multi-character delimiters.
- **`str_count(s, sub)` built-in** — count non-overlapping occurrences of a substring in a string. Returns 0 for empty substring or when not found.
- New integration tests: `string_join_count_test.om`, `string_interp_test.om`, `multicase_interp_test.om`
- New integration test: `register_test.om` — dedicated test for register keyword covering accumulation loops, countdown, typed register variables, and in-loop register reassignment
- 9 new lexer unit tests for string interpolation, 1 parser + 3 codegen tests for multi-value case and constant switch elimination
- Standard library count increased from 119 to 121 built-in functions

### Changed
- **`register` keyword forces register allocation** — `register var` now runs an immediate mem2reg pass after function codegen, guaranteeing that annotated variables are promoted to SSA registers regardless of the global optimization level. Variables remain mutable — the keyword forces register allocation, not immutability.
- **`register` emits `llvm.lifetime.start`** — register variables now emit tight lifetime scoping via `llvm.lifetime.start` intrinsic, giving LLVM's register allocator precise live-range information for better register utilization.
- **Compile-time warning for non-promotable register variables** — If a `register` variable has a type that cannot be promoted to a CPU register (arrays, structs, pointers), the compiler now emits a diagnostic warning instead of silently ignoring the annotation.
- **Documentation corrections** — Updated built-in function count to 121 across README.md and LANGUAGE_REFERENCE.md; fixed stale compiler version references; documented `**=` and `??=` compound assignment operators; added `register` to the keywords table in LANGUAGE_REFERENCE.md
- **Version bump** to 3.0.0

### Fixed
- **`register var` reassignment** — `register var counter = 0; counter = counter + 1;` now compiles correctly. Previously emitted a spurious "Cannot reassign 'register' variable" error that prevented using register variables in loops and accumulators.
- **`str_replace` documentation** — Fixed documentation that incorrectly stated `str_replace` only replaces the first occurrence. The implementation has always replaced all occurrences.

## [2.9.0] - 2026-03-22

### Added
- **SIMD vector types** for handwritten SIMD programming:
  - 8 vector type annotations: `f32x4`, `f32x8`, `f64x2`, `f64x4`, `i32x4`, `i32x8`, `i64x2`, `i64x4`
  - Map directly to LLVM fixed-vector types (`<4 x float>`, `<2 x double>`, etc.)
  - Full arithmetic operator support: `+`, `-`, `*`, `/`, `%` on vectors
  - Bitwise operators on integer vectors: `&`, `|`, `^`, `<<`, `>>`
  - Scalar-to-vector broadcast (splat) when mixing scalar and vector operands
  - Element access via indexing: `v[i]` (extract), `v[i] = x` (insert)
  - Array literal initialization: `var v: f32x4 = [1.0, 2.0, 3.0, 4.0];`
- **`register` keyword** for register allocation hints:
  - Syntax: `register var x = 0;` — hints that the variable should be kept in a CPU register
  - Sets high alignment on allocas to encourage LLVM's SROA/mem2reg promotion to SSA registers
- **`**=` compound assignment operator** — power-assign (e.g., `x **= 3` is `x = x ** 3`)
- **`??=` compound assignment operator** — null-coalesce-assign (e.g., `x ??= 42` assigns 42 if x is falsy)
- New integration tests: `simd_register_test.om`, `compound_assign_test.om`
- Unit tests for all new features (lexer, parser, codegen)

### Fixed
- **Struct field compound assignment** — `s.x += 1`, `s.y *= 2`, etc. now work correctly. Previously failed with cryptic "Invalid compound assignment target" error because the parser only handled identifiers and array elements, not struct field access expressions.
- **Error messages** — improved diagnostic messages for invalid assignment and compound assignment targets, now listing supported forms (variables, array elements, struct fields)

### Changed
- **Refactored argument validation** in `codegen_builtins.cpp` — extracted `validateArgCount()` helper to replace 107 duplicated argument-count check blocks, reducing boilerplate by ~400 lines
- **Version bump** to 2.9.0

## [2.8.0] - 2026-03-22

### Added
- **12 new math built-in functions** returning floating-point values:
  - Trigonometric: `sin(x)`, `cos(x)`, `tan(x)`, `asin(x)`, `acos(x)`, `atan(x)`, `atan2(y, x)` — radians-based trigonometry with LLVM intrinsics (`sin`, `cos`) and C library calls (`tan`, `asin`, `acos`, `atan`, `atan2`)
  - Exponential/Logarithmic: `exp(x)`, `log(x)`, `log10(x)` — using LLVM intrinsics for native hardware acceleration
  - Other: `cbrt(x)` (cube root), `hypot(x, y)` (hypotenuse without overflow) — via C library calls
- **6 new array utility built-in functions**:
  - `array_min(arr)` — minimum element (0 for empty arrays)
  - `array_max(arr)` — maximum element (0 for empty arrays)
  - `array_find(arr, value)` — index of first match, or -1 if not found
  - `array_any(arr, "fn")` — returns 1 if predicate matches any element
  - `array_every(arr, "fn")` — returns 1 if predicate matches all elements (vacuous truth for empty)
  - `array_count(arr, "fn")` — count of elements matching predicate
- New integration tests: `trig_math_test.om`, `array_utility_test.om`
- 20 new unit tests covering all new builtins
- Standard library count increased from 97 to 115 built-in functions

### Changed
- **Version bump** to 2.8.0
- **LANGUAGE_REFERENCE.md** updated with documentation for all 18 new built-in functions
- **README.md** updated with new function tables and total count

## [2.7.9] - 2026-03-21

### Fixed
- **Critical: E-graph logical operator miscompilation** — Six e-graph rewrite rules for `&&`/`||` incorrectly returned raw operand values instead of boolean (0/1) results. For example, `0 || 7` produced `7` instead of `1` at O2 optimization level. Affected rules: `x && x`, `x || x`, `x || 0`, `0 || x`, `x && 1`, `1 && x` — all now correctly produce boolean conversions via `x != 0`.
- **Undefined behavior in left-shift constant folding** — `(-1) << 3` triggered C++ signed left-shift UB at compile time. Now uses unsigned arithmetic for the shift, producing correct results (-8 in this case).

### Added
- **Complete `&&`/`||` constant folding** — When both operands of `&&` or `||` are compile-time constants, the result is now folded to a single constant without any ICmp/ZExt/branch instructions.
- New integration test `neg_shift_test.om` covering negative shift and logical constant folding
- 6 new unit tests: `LeftShiftNegativeConstFold`, `LeftShiftLargeNegativeConstFold`, `LogicalAndConstantFold`, `LogicalOrConstantFold`, `LogicalAndFalseConstantFold`, `LogicalOrBothZeroConstantFold`

### Changed
- **Version bump** to 2.7.9
- Updated e-graph unit tests to verify correct boolean semantics

## [2.4.1] - 2026-03-21

### Added
- **AST-level self-identity optimizations**: `x == x → 1`, `x != x → 0`, `x < x → 0`, `x > x → 0`, `x <= x → 1`, `x >= x → 1`, `x / x → 1`, `x % x → 0` in OPTMAX constant folder
- **AST-level negation identities**: `0 - x → -x`, `x * (-1) → -x`, `x / (-1) → -x`, `(-1) * x → -x` in OPTMAX constant folder
- **AST-level modulo identity**: `x % 1 → 0` in OPTMAX constant folder
- **IR-level float constant folding**: `%` (fmod) and `**` (pow) now folded at compile time when both operands are constants
- **IR-level float negation identities**: `x * (-1.0) → -x`, `x / (-1.0) → -x`, `(-1.0) * x → -x`
- **Null coalescing constant folding**: `42 ?? y → 42`, `0 ?? y → y` — eliminates branches when the left operand is a compile-time constant
- New unit tests: `FloatConstantFoldMod`, `FloatConstantFoldPow`, `NullCoalesceConstFoldNonzero`, `NullCoalesceConstFoldZero`, `FloatNegOneMultFold`

### Changed
- **LANGUAGE_REFERENCE.md** updated to document new constant folding and identity optimizations (sections 19–21)
- **Version bump** to 2.4.1

## [2.4.0] - 2026-03-20

### Added
- **Use-after-move/invalidate detection**: Compile-time enforcement of ownership safety — accessing a variable after `move` or `invalidate` now produces a compile error with clear diagnostic messages
- **Ownership system codegen wiring**: `deadVars_`/`deadVarReason_` tracking infrastructure connected to `generateIdentifier`, `generateInvalidate`, `generateMoveDecl`, `generateMoveExpr`, and `generateAssign`
- **Variable revival**: Re-assigning to a moved/invalidated variable revives it (clears dead state)
- **CLI flags**: `-fegraph`/`-fno-egraph`, `-fsuperopt`/`-fno-superopt`, `-fhgoe`/`-fno-hgoe` for controlling optimization passes

### Changed
- **Documentation consolidation**: Merged 6 separate documentation files (EGRAPH_OPTIMIZATION.md, MEMORY_MANAGEMENT.md, OPTIMIZATIONS.md, etc.) into 3 files: LANGUAGE_REFERENCE.md (comprehensive reference), README.md (overview), CHANGELOG.md (history)
- **LANGUAGE_REFERENCE.md** expanded with:
  - Section 14: Ownership System (move/invalidate/borrow semantics, variable state model, compile-time enforcement)
  - Section 11.6: Struct field-level optimization attributes
  - Section 21: E-Graph & Superoptimizer (consolidated from deleted files)
  - Updated grammar (section 28) with ownership productions
  - Updated token reference with `move`, `invalidate`, `borrow` keywords
- **Version bump** to 2.4.0

### Removed
- EGRAPH_OPTIMIZATION.md (content merged into LANGUAGE_REFERENCE.md section 21)
- MEMORY_MANAGEMENT.md (content merged into LANGUAGE_REFERENCE.md section 23)
- OPTIMIZATIONS.md (content merged into LANGUAGE_REFERENCE.md sections 19-21)

## [2.3.9] - 2026-03-07

### Added
- **Struct/record type support**: `struct Point { x, y }` with field access (`p.x`), field assignment (`p.x = 10`), and struct literal creation (`Point { x: 10, y: 20 }`)
- **Module/import system**: `import "path/to/module"` with automatic `.om` extension, relative path resolution from source file, and circular import detection
- **Generic function type parameter syntax**: `fn identity<T>(x: T) -> T { ... }` — type parameters are parsed for documentation and annotation (type-erased at runtime since all values are i64)
- **File I/O builtins**: `file_read(path)`, `file_write(path, content)`, `file_append(path, content)`, `file_exists(path)`
- **Map/dictionary builtins**: `map_new()`, `map_set(map, key, value)`, `map_get(map, key, default)`, `map_has(map, key)`, `map_size(map)`, `map_keys(map)`, `map_values(map)`, `map_remove(map, key)`
- **Range builtins**: `range(start, end)`, `range_step(start, end, step)`
- **Concurrency primitives** (pthreads): `thread_create("fn_name")`, `thread_join(handle)`, `mutex_new()`, `mutex_lock(m)`, `mutex_unlock(m)`, `mutex_destroy(m)`
- **DWARF debug info**: `DIBuilder` integration with source line and column tracking attached to IR instructions (Dwarf Version 4)
- **Structured error codes** (E001–E012) with diagnostic severity levels (error, warning, note, hint) and Levenshtein-based "did you mean?" suggestions
- **Compile-time string constant folding**: adjacent string literal concatenations merged at compile time
- **OPTMAX pragma expansion** for more aggressive per-function optimization
- Utility builtins: `char_code()`, `number_to_string()`, `string_to_number()`
- CodeGenerator refactored from monolithic `codegen.cpp` into 5 focused files: `codegen.cpp`, `codegen_builtins.cpp`, `codegen_expr.cpp`, `codegen_stmt.cpp`, `codegen_opt.cpp`
- Standard library expanded to **97 built-in functions** (from 69)
- New integration tests: `struct_test.om`, `import_test.om`, `generic_test.om`, `file_io_test.om`, `map_test.om`, `range_test.om`, `string_fold_test.om`, `thread_test.om`

### Changed
- **Massive JIT speed improvements**:
  - Tier-2 recompilation threshold lowered from 5 to **2 calls** — optimized code activates almost immediately
  - Tier-1 baseline upgraded from **O0 to O1** codegen — 2–3× faster baseline execution during the brief warm-up period
  - Background compilation threads increased from **2 to 4** — parallel compilation bandwidth doubled
  - **Double O3 pass** for both whole-module and per-function recompilation — cascading optimizations exploit opportunities revealed by the first pass
  - Whole-module InlinerThreshold raised from 5000 to **10000** and per-function from 2000 to **5000** — more aggressive cross-function inlining
  - SIMD vectorization width and interleave count raised from 8 to **16** — wider vector operations on modern CPUs
  - Loop full-unroll limit raised from 32 to **64 iterations** — eliminates loop overhead for medium-sized loops; large loops unrolled by 32 (up from 16)
  - Constant specialization threshold lowered from 80% to **60%** — triggers constant folding earlier with less profile data

### Fixed
- For-loop descending range now auto-detects step `-1` when `start > end`
- `range_step` division by zero when step is 0
- `push()` memory leak when reallocating arrays
- `generateIncDec` type normalization for increment/decrement operations
- Null byte escape inconsistency between `\0` and `\x00`
- `NoSync`/`NoFree`/`WillReturn` LLVM attributes incorrectly applied to functions calling concurrency primitives
- `str_repeat` with negative count no longer causes undefined behavior
- Constant folding undefined behavior edge cases

## [2.3.2] - 2026-03-06

### Added
- **Production-grade exception hierarchy**: `FileError`, `ValidationError`, `LinkError` in `diagnostic.h` — replaces raw `std::runtime_error` throws with domain-specific exception types

### Changed
- `[[nodiscard]]`/`noexcept` annotations added to all getter methods
- `explicit` keyword on all single-argument constructors to prevent implicit conversions
- Named constants replacing magic numbers throughout codebase
- Fixed include order and header guards to match style guide

## [2.3.1] - 2026-03-06

### Changed
- `noexcept` on Value copy operations for move-optimization in STL containers
- Length-aware `RefCountedString` constructor avoids redundant `strlen` calls
- `memcmp`-based string comparisons replacing character-by-character loops
- `DeoptManager` merge optimization for deoptimization logic

### Fixed
- `dump()` method const-correctness
- `ArgProfile` data structure correctness

## [2.3.0] - 2026-03-06

### Added
- Thread-local profiler counters for reduced contention in JIT runtime
- Benchmark tests for profiling overhead measurement
- Single-allocation mixed-type string concatenation in Value class

### Changed
- Improved code quality throughout: `[[nodiscard]]` annotations, exception safety, named constants, `noexcept` specifiers

## [2.2.9] - 2026-03-05

### Added
- **Multi-tier JIT system** with adaptive recompilation — Tier-1 baseline through Tier-5 mega-hot (thresholds at 50, 500, 5000, and 50000 calls) with increasingly aggressive optimization
- **ORC LLJIT** replaces MCJIT — modern, thread-safe JIT compilation engine
- **Parallel background compilation** with 4 dedicated threads and atomic function pointer hot-patching for zero-stall recompilation
- **Eager background compilation** with aggressive inlining — JIT achieves **2.6× faster than AOT**
- **Non-blocking runtime**: atomic loads and try_lock callbacks eliminate stalls during compilation
- **Hot-function priority queue** for compilation scheduling — hottest functions recompiled first
- **Constant-argument specialization**: clone and constant-propagate hot functions with stable argument patterns
- **Loop unroll hints** and compile-time function evaluation via LLVM Interpreter
- **LTO pre-link pipeline** with CMake LTO configuration
- JIT vs AOT benchmark suite with runner script and per-function timing
- Comprehensive `RUNTIME_REPORT.md` documenting all runtimes and JIT/AOT pipelines

### Fixed
- String + float concatenation and `to_string(float)` output format
- `typeof` type tags for correct runtime type identification
- `throw` propagation through nested `try`/`catch` blocks
- `len()` on strings (previously returned array length instead of string length)
- `s[i]` and for-each on strings were using array layout semantics instead of string character access
- `pow(float)` incorrect results for fractional exponents
- `str_replace` now replaces **all** occurrences instead of only the first
- `to_int`/`to_float` string parsing edge cases
- `to_char` segfault when called with certain values
- String variable ordering comparisons (`<`, `>`, `<=`, `>=`)
- `string * int` concatenation crash
- String array element type-loss, for-each string iterator, and `sort` on string arrays
- `print_char` crash when given `to_char()` result
- Profile data race, atomic undefined behavior, exception safety, and RAII ordering in JIT runtime
- Memory ordering upgraded from Monotonic to Acquire for correctness
- JIT compilation failures: disabled CallGraphProfile pass, use null TargetMachine for O3, erase main before recompile, whole-module compilation
- PHI node ordering: use `getFirstNonPHI()` for loop exit block instrumentation

## [2.2.0] - 2026-03-04

### Added
- **Loop trip count profiling** in JIT optimizer — records iteration counts per loop for unroll and vectorization decisions
- **Call-site frequency tracking** — per-callsite counters feeding inlining heuristics
- **Value range profiling** — tracks argument value distributions for specialization opportunities
- JIT constant specialization for hot call sites
- Cold function marking with LLVM `cold` attribute for improved code layout
- Loop vectorization metadata on JIT-recompiled functions

### Fixed
- O(n²) back-edge detection replaced with linear algorithm

## [2.1.9] - 2026-03-04

### Changed
- JIT now respects user-specified optimization level (`-O0` through `-O3`)

### Fixed
- `createMergedLoadStoreMotionPass` guarded with LLVM version check for cross-version compatibility

## [2.1.3] - 2026-03-04

### Added
- Intra-procedural optimization passes applied in JIT baseline (`generateHybrid`)
- Branch profiling wired into `injectCounters` with C-linkage callback tests
- Loop rotation, simplification, and sinking passes added to JIT pipeline

### Changed
- Tier-2 JIT threshold lowered to 100 calls for earlier reoptimization

### Fixed
- LLVM API compatibility: guarded `UnsafeFPMath`, `PGOOptions` VFS parameter, and `createLoopRotatePass` for multi-version support

## [2.1.1] - 2026-03-03

### Added
- **Verbose JIT output**: print recompile events when `--verbose` flag is set

### Fixed
- JIT recompilation not firing for hot functions
- OPTMAX pass ordering (optimization passes now applied in correct sequence)

## [2.1.0] - 2026-03-03

### Added
- **NSW (No-Signed-Wrap) flags** on integer `add`, `sub`, `mul`, `shl` for better range analysis and optimization
- **`norecurse` attribute** on non-recursive user functions for interprocedural optimization
- **`noundef` attribute** on function parameters and return values for value-range propagation
- **Loop vectorization hints**: `llvm.loop.vectorize.enable` and `llvm.loop.interleave.count` metadata on loops
- **Power-of-2 division strength reduction**: `x / 2^n` compiled as arithmetic right shift `x >> n`
- **Power-of-2 modulo strength reduction**: `x % 2^n` compiled as bitwise mask `x & (2^n - 1)`
- Large-scale loops + math benchmark suite with C and Rust reference implementations

### Changed
- Runtime overhead reduced with branch prediction hints (`__builtin_expect`), optimized copy/move paths, and lock-free profiling

### Fixed
- Auto-convert int/float to string in string concatenation (previously caused SIGSEGV)
- Wrapping arithmetic and direct `SDiv`/`SRem` for constant divisors in codegen

## [2.0.0] - 2026-03-01

### Added
- **Lambda expressions**: `|x| x * 2` syntax for anonymous functions — desugared at parse time to named functions; works seamlessly with `array_map`, `array_filter`, and `array_reduce`
  - Single parameter: `|x| x * 2`
  - Multiple parameters: `|a, b| a + b`
  - Zero parameters: `|| 42`
- **Pipe operator** (`|>`): left-to-right function chaining — `expr |> fn` desugars to `fn(expr)`; supports both stdlib and user-defined functions; left-associative for chaining: `x |> f |> g`
- **Spread operator** (`...`): array unpacking in array literals — `[1, ...arr, 2]` creates a new array with elements from `arr` spliced in; supports multiple spreads and dynamic runtime length computation
- New lexer tokens: `PIPE_FORWARD` (`|>`), `FAT_ARROW` (`=>`), `SPREAD` (reserved)
- New AST node types: `LAMBDA_EXPR`, `SPREAD_EXPR`, `PIPE_EXPR`
- 21 new unit tests (6 lexer, 7 parser, 8 codegen)
- Integration test: `examples/lambda_pipe_spread_test.om`

## [1.9.0] - 2026-03-01

### Added
- **`array_map(arr, "fn_name")`** built-in — applies a named function to each element and returns a new array; the function name must be a string literal resolved at compile time
- **`array_filter(arr, "fn_name")`** built-in — returns a new array containing only elements for which the named predicate function returns a non-zero value
- **`array_reduce(arr, "fn_name", initial)`** built-in — reduces an array to a single value by applying a named two-argument function (accumulator, element) across all elements
- Standard library count increased from 66 to 69 built-in functions

### Fixed
- **LLVM 17 compatibility**: `CodeGenOpt::Level` vs `CodeGenOptLevel` API difference now handled via `#if LLVM_VERSION_MAJOR >= 18` guards in `src/codegen.cpp` and `runtime/jit.cpp`

## [1.8.0] - 2026-03-01

### Added
- **LLVM intrinsics for math builtins**: `abs`, `sqrt`, `min`, `max`, `floor`, `ceil`, `round`, and `clamp` now emit native LLVM intrinsics (e.g. `llvm.abs.i64`, `llvm.sqrt.f64`, `llvm.smin`, `llvm.maxnum`) instead of manual compare-and-select or loop-based implementations
- **Binary exponentiation**: `**` operator and `pow()` builtin use O(log n) exponentiation by squaring instead of O(n) linear multiply loop
- **O1 pipeline upgrade**: replaced legacy `FunctionPassManager` (6 local passes, no IPO) with `PassBuilder::buildPerModuleDefaultPipeline(O1)`, gaining inlining, IPSCCP, GlobalDCE, and jump threading
- **CodeGenOptLevel mapping**: `createTargetMachine()` now passes the correct `llvm::CodeGenOptLevel` so backend instruction selection, scheduling, and register allocation match the requested `-O` level
- **JIT passes**: added SROA, EarlyCSE, DCE, and TailCallElimination to JIT optimization pipeline
- Target triple and data layout are now always set on the LLVM module (previously skipped at O0)
- Inline hint threshold increased from 8 to 16 statements at O3
- Linker now receives `-O` level and `-lm` flags

### Changed
- **VM CALL handler**: deferred type profiling after JIT cache checks so hot JIT paths skip `classifyArgTypes` + `recordTypes` overhead; eliminated redundant `functions.find()` hash-map lookup by reusing a single iterator
- **`Value::toString()` float path**: replaced `std::ostringstream` (heap-allocating) with `snprintf` into a stack buffer
- **`Value::operator==` int fast path**: direct `intValue` comparison when both operands are INTEGER, avoiding `toDouble()` promotion
- **`JITCompiler::recordCall()` hot path**: single `find()` on `callCounts_` for repeat calls instead of three `.count()` lookups per invocation
- **Register restore**: `std::move` instead of `std::copy` when restoring registers from call frames, avoiding refcount churn on string Values

### Fixed
- `CodegenTest.NoInliningAtO1` test updated to `InliningAtO1` — O1 standard pipeline now includes inlining, so the assertion was corrected to match actual behavior

## [1.7.0] - 2026-03-01

### Added
- **Default function parameters**: `fn foo(a, b = 10) { ... }` — parameters with default values can be omitted at call sites; non-default parameters must precede default ones; defaults must be literal expressions (integer, float, or string)
- **`array_copy(arr)`** built-in — creates a heap-allocated deep copy of an array, leaving the original unmodified
- **`array_remove(arr, idx)`** built-in — removes the element at the given index, shifts remaining elements left, decrements length, and returns the removed value; includes bounds checking
- **`input_line()`** built-in — reads a full line from stdin as a heap-allocated string (strips trailing newline); returns empty string on EOF
- Standard library count increased from 63 to 66 built-in functions

## [1.6.0] - 2026-03-01

### Added
- **Error handling**: `try`/`catch`/`throw` statements — structured error handling with integer error codes
- **Enum declarations**: `enum Name { A, B = 10, C }` — named integer constants with auto-increment, accessed as `Name_A`, `Name_B`, `Name_C`
- **I/O built-ins**: `println(x)` (print with newline), `write(x)` (print without newline)
- **System built-ins**: `exit_program(code)`, `random()` (auto-seeded), `time()` (Unix timestamp), `sleep(ms)` (milliseconds)
- **String parsing**: `str_to_int(s)`, `str_to_float(s)` — convert strings to numeric values
- **String/Array utilities**: `str_split(s, delim)` (split into array), `str_chars(s)` (string to char code array)
- Standard library count increased from 51 to 63 built-in functions

## [1.5.0] - 2026-03-01

### Added
- **Null coalescing operator** `??` — returns left operand if non-zero, otherwise right operand (e.g., `x ?? default_value`)
- **Multi-line string literals** via triple-quoted strings (`"""..."""`) — supports embedded newlines without escape sequences
- **Math built-ins**: `floor(x)`, `ceil(x)`, `round(x)` — rounding operations returning integers
- **Type conversion built-ins**: `to_int(x)` (float→int truncation), `to_float(x)` (int→float)
- **String built-ins**: `str_substr(s, start, len)`, `str_upper(s)`, `str_lower(s)`, `str_contains(s, sub)`, `str_replace(s, old, new)`, `str_trim(s)`, `str_starts_with(s, prefix)`, `str_ends_with(s, suffix)`, `str_index_of(s, sub)`, `str_repeat(s, n)`, `str_reverse(s)`
- **Array built-ins**: `push(arr, val)`, `pop(arr)`, `index_of(arr, val)`, `array_contains(arr, val)`, `sort(arr)`, `array_fill(n, val)`, `array_concat(a, b)`, `array_slice(arr, start, end)`
- Standard library count increased from 29 to 51 built-in functions

## [1.4.0] - 2026-03-01

### Added
- `log2(n)` stdlib function — integer base-2 logarithm (floor), returns -1 for n ≤ 0
- `gcd(a, b)` stdlib function — greatest common divisor using Euclidean algorithm, works with negative numbers
- `to_string(n)` stdlib function — converts integer to heap-allocated string representation
- `str_find(s, ch)` stdlib function — finds first occurrence of character code in string, returns index or -1
- Standard library count increased from 25 to 29 built-in functions

## [1.3.0] - 2026-02-23

### Added
- Exponentiation operator `**` (right-associative: `2 ** 3 ** 2` = `2 ** 9` = `512`) with constant folding, float support, and bytecode backend
- `str_concat(a, b)` stdlib function for explicit string concatenation

### Fixed
- Arrays returned from functions no longer cause use-after-free; `generateArray()` now uses heap allocation (`malloc`) instead of stack allocation (`alloca`), so arrays survive cross-function returns

## [1.2.0] - 2026-02-23

### Added
- Prefix/postfix increment/decrement on array elements (`arr[i]++`, `++arr[i]`, `arr[i]--`, `--arr[i]`) with bounds checking
- Hex escape sequences in string literals (`"\x41"` → `"A"`)
- Underscore separators in numeric literals for readability (`1_000_000`, `0xFF_FF`, `0b1010_0101`, `0o7_7`)

### Fixed
- `arr[0]++` and `++arr[0]` no longer produce a parse error ("requires an identifier operand"); prefix/postfix operators now accept any lvalue including array element expressions

## [1.1.0] - 2026-02-23

### Added
- Hexadecimal integer literals (`0xFF`, `0x1A`)
- Octal integer literals (`0o77`, `0o10`)
- Binary integer literals (`0b1010`, `0b11111111`)
- Clear error messages for malformed number literals (e.g. `0x` with no digits)

### Fixed
- Constant folding of `INT64_MIN / -1` and `INT64_MIN % -1` no longer causes undefined behavior (both AST and LLVM IR paths)
- Constant folding of unary negation on `INT64_MIN` no longer causes undefined behavior (both AST and LLVM IR paths)
- Shift operations (`<<`, `>>`) with out-of-range amounts (≥ 64) no longer cause undefined behavior in compiled code; shift amount is now masked to [0, 63]

## [1.0.0] - 2026-02-22

### Fixed
- `print_char(c)` now returns the character code `c` as documented, instead of always returning `0`
- `swap(arr, i, j)` now performs bounds checking on both indices and aborts with a runtime error on out-of-bounds access (was silently corrupting stack memory)
- `char_at(s, i)` now performs bounds checking and aborts with a runtime error on out-of-bounds access (was undefined behaviour)
- `continue` inside a `switch` nested within a loop now correctly jumps to the enclosing loop's continue target instead of segfaulting
- `switch case` with a float literal (e.g. `case 1.5:`) now raises a compile error "case value must be an integer constant, not a float" instead of silently truncating to an integer
- Error messages for `assert` failures and array-index-out-of-bounds were swapped; both now display the correct message

### Added
- `str_len`, `char_at`, `str_eq`, `typeof`, and `assert` are now fully documented in `LANGUAGE_REFERENCE.md` (sections 11.5–11.6)
- Standard library count corrected to 24 built-in functions in all documentation

## [0.3.1] - 2026-02-15

### Added
- `switch`/`case`/`default` statement support
- `typeof` and `assert` built-in functions
- `break` support inside switch cases
- VM function call system with `CALL`, `LOAD_LOCAL`, `STORE_LOCAL` opcodes
- Bytecode switch statement support
- GitHub Actions release workflow for versioned binary publishing
- Comprehensive test coverage across all modules

### Fixed
- VM nested call stack corruption (caller stack preserved across recursive calls)
- OPTMAX optimizer now constant-folds inside switch statements
- `toDefaultType()` properly zero-extends i1 (boolean) values to i64
- Bytecode switch default case ordering (default emitted last)
- Unique temp variable names in bytecode switch for nested switch support

## [0.3.0] - 2026-02-15

### Added
- LLVM OPTMAX optimization pragma for per-function aggressive optimization
- Array literals and indexing with bounds checking
- String concatenation via `+` operator
- Standard library: `abs`, `min`, `max`, `sign`, `clamp`, `pow`, `sqrt`
- Standard library: `len`, `sum`, `swap`, `reverse`, `is_even`, `is_odd`
- Standard library: `print_char`, `input`, `to_char`, `is_alpha`, `is_digit`
- Float type support with automatic int/float coercion
- Constant declarations with `const` keyword
- Do-while loops
- Ternary operator (`? :`)
- Compound assignment operators (`+=`, `-=`, `*=`, `/=`, `%=`)
- Prefix and postfix increment/decrement (`++`, `--`)
- Bitwise operators (`&`, `|`, `^`, `~`, `<<`, `>>`)
- Short-circuit evaluation for `&&` and `||`
- Block comments (`/* ... */`)
- Error messages with line and column numbers
- Division-by-zero and modulo-by-zero runtime checks
- Integer overflow detection at compile time
- Forward function references
- Reference-counted memory management

## [0.2.0] - 2026-02-14

### Added
- Bytecode VM interpreter for dynamic code paths
- For-range loops with configurable step
- Break and continue statements
- Scoped variable declarations with block scope
- Optimization levels O0 through O3

## [0.1.0] - 2026-02-14

### Added
- Initial compiler with LLVM backend
- Function declarations and calls
- If/else conditionals
- While loops
- Variable declarations
- Integer arithmetic (`+`, `-`, `*`, `/`, `%`)
- Comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`)
- `print` built-in function
- CLI with lex, parse, emit-ir, build, and run commands
