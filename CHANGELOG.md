# Changelog

All notable changes to the OmScript compiler will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **CI/Release build fix** (`src/alg_simp_pass.cpp`):
  - Added missing `#include <climits>` that caused `LLONG_MIN` to be undeclared on GCC (all Linux CI jobs and the Release PGO build were failing). Clang finds `LLONG_MIN` via implicit includes but GCC strictly requires the explicit header.
  - Fixed a misleading-indentation warning in `src/opt_orchestrator.cpp:853` that was flagged by `tidy-check` (the `break` now lives on its own line inside the REBORROW_EXPR case).

### Added

- **Round-26: nuw+nsw on GCD/LCM Stein shifts + boolean PHI metadata** (`src/codegen_builtins.cpp`):
  - **`nuw+nsw` on `gcd.shifted`**: In binary Stein's GCD, `shifted = lo << k` where `lo` is the minimum of two positive odd integers derived from `llvm.abs` of the inputs (is_int_min_poison=true ⟹ inputs ≤ INT64_MAX) and `k = ctz(|a|∣|b|) ≤ 62`. The product equals `gcd(a,b) ≤ min(|a|,|b|) ≤ INT64_MAX`, so neither unsigned nor signed overflow is possible.
  - **`nuw+nsw` on `lcm.gcdval`**: Identical reasoning applies to the embedded GCD step inside the `lcm` builtin.
  - **`!range [0,2)` + `nonNegValues_` on `aany.result`**: `array_any` always returns 0 (not found) or 1 (found); the result PHI is given the same boolean-range metadata as `array_count`, `file_exists`, `is_nan`, etc.
  - **`!range [0,2)` + `nonNegValues_` on `aevery.result`**: Same treatment for `array_every`, which also returns a strict 0/1 boolean.
  - **`nonNegValues_` on `scount.result`**: `str_count` accumulates a match count starting at zero with `nuw+nsw` increments; the final PHI (merging the zero-on-empty path with the loop exit count) is now tracked as non-negative.


  - **`nonNegValues_.insert` on `join.celemlen`**: the concatenation loop's per-element strlen (`join.celemlen`) had `arrayLenRangeMD_` but was not added to `nonNegValues_`, unlike its sizing-loop counterpart `join.elemlen` (which already had both). Now consistent.
  - **`nonNegValues_.insert` on `fwrite.len` / `fappend.len`**: strlen results in `file_write` and `file_append` had `arrayLenRangeMD_` but missed `nonNegValues_` tracking. Adding it keeps value-range state consistent so downstream passes can exploit non-negativity if the block is inlined.
  - **`nonNegValues_.insert` on `strpad.slen`**: strlen result in `str_pad_left` / `str_pad_right` had `arrayLenRangeMD_` but no `nonNegValues_` entry. The value feeds a `ULT(slen, effectiveWidth)` branch guard and a `nuw+nsw` subtract — proper tracking helps LLVM propagate the non-negativity through both.
  - **`nonNegValues_.insert` on `cmd.clen`**: strlen result in `command` had no metadata at all. Added `nonNegValues_` to match the pattern of other strlen-derived values.
  - **`nuw+nsw` on `cmd.ns1`**: `needOne = newSize + 1` where `newSize = curSize + chunkLen` already carries `nuw+nsw` (curSize is a non-negative byte counter, chunkLen is a non-negative strlen). Adding `nuw+nsw` to the `+1` lets LLVM track the full chain through `ICmpUGT(needOne, curCap)` without an artificial upper-bound hypothesis.

- **Round-24: boolean metadata sweep + no-wrap arithmetic** (`src/codegen_builtins.cpp`):
  - **`emitBoolZExt` on `bigint_is_zero` / `bigint_is_negative`**: C library returns i32 0/1; `CreateIsNotNull` converts to i1, then `emitBoolZExt` attaches `zext nneg` + `!range [0,2)` + `nonNegValues_` (consistent with `bigint_eq/lt/le/gt/ge` from Round-23).
  - **`charRangeMD_` + `nonNegValues_` on `char_code` result**: i8 loaded from string extended to i64 now gets the same `!range [0,256)` metadata and non-negative tracking as `char_at`.
  - **`zext nneg` on `str_format` probe length**: snprintf returns a non-negative count on success; `CreateZExt(probeResult, i64, IsNonNeg=true)` expresses this so LLVM can infer `[0, 2^31)` on the widened value without a separate analysis pass.
  - **`nuw+nsw` on `range_step` slot index**: `slot = i + 1` where `i` is ULT-bounded in `[0, count)` (non-negative by clamp) → `i+1` is in `[1, count+1]`, no unsigned or signed overflow. Companion `rstep.next` already had these flags.
  - **`nuw+nsw` on `array_slice` length**: `endArg - startArg` where all code paths above the subtraction clamp `endArg ≥ startArg` (via `select(endArg < startArg, startArg, endArg)`) — no underflow possible.

- **Round-23: `emitBoolZExt` sweep + arithmetic precision** (`src/codegen_builtins.cpp`):
  - **`emitBoolZExt` on `file_exists`**: `access()==0` comparison result gains `zext nneg` + `!range [0,2)`.
  - **`emitBoolZExt` on `is_power_of_2`**: bare `CreateZExt + nonNegValues_.insert` replaced by `emitBoolZExt` (which does both, plus the `nneg` flag).
  - **`emitBoolZExt` on `env_set`**: `setenv()==0` comparison gains the full boolean metadata.
  - **`emitBoolZExt` on `is_nan`**: `fcmp uno` result gains `zext nneg` + `!range [0,2)`.
  - **`emitBoolZExt` on `is_inf`**: `or(fcmp oeq pos, fcmp oeq neg)` result gains full boolean metadata.
  - **`zext nneg` on array sort comparator**: the two `zext i1→i32` in `__omsc_cmp_arr_asc` now carry the `nneg` flag (LLVM can propagate the `[0,1]` range into the `sub` that computes the 3-way result).
  - **`emitBoolZExt` on `bigint_eq/lt/le/gt/ge`**: the C library returns i32 0/1; `CreateIsNotNull` converts to i1, then `emitBoolZExt` adds `zext nneg` + `!range [0,2)` + `nonNegValues_` tracking for all five comparison builtins.
  - **`nuw+nsw` on `lcm.diff`**: the Stein-GCD inner loop for `lcm` has the same `hi-lo` (select-proven) structure as `gcd.diff` fixed in Round 22; now tagged `nuw+nsw`.

- **Round-22: Unsigned bounds checks + arithmetic precision** (`src/codegen_builtins.cpp`):
  - **ULT/ULE single-check bounds checks**: the double-check pattern `SGE(idx, 0) && SLT(idx, len)` is replaced with a single `ULT(idx, len)` (or `ULE` for insert-at-end). Since array/string lengths are always non-negative (proven by `nonNegValues_`), `ULT(idx, len)` is equivalent to the two-check pattern but emits one fewer `icmp` and eliminates the `and` instruction. Applied to: `swap`, `char_at`, `array_remove`, `array_insert`.
  - **`emitBoolZExt` for `array_count`**: the bare `CreateZExt(isNonZero, ...)` predicate accumulator increment is migrated to `emitBoolZExt`, adding `zext nneg` + `!range [0,2)` + `nonNegValues_` tracking.
  - **`nuw+nsw` on `gcd.diff`**: `hi - lo` where `hi = max(a, b_odd)` and `lo = min(a, b_odd)` (proven by select) — no underflow/overflow possible.
  - **`nuw+nsw` on `log2.val`**: `63 - ctlz(n)` where `ctlz(n) ∈ [0, 63]` (guarded by `isPositive` + `is_zero_poison=true`) — result is in `[0, 63]`.

- **Round-21: `zext nneg` flag + no-wrap arithmetic** (`src/codegen.cpp`, `src/codegen_expr.cpp`, `src/codegen_builtins.cpp`):
  - `emitBoolZExt()` now passes `IsNonNeg=true` to `CreateZExt`, emitting `zext nneg` (LLVM 18+). This lets LLVM's value-range inference skip a separate sign-bit analysis on all boolean 0/1 values.
  - `toDefaultType()` passes `IsNonNeg=true` for `i1` sources and values in `nonNegValues_`. Non-negative source → `zext nneg`; unsigned-tagged but potentially high-bit sources remain plain `zext` (correct semantics preserved).
  - `str_ends_with`: `strLen - sufLen` tagged `nuw+nsw` (proven by the `tooLong` guard above it).
  - `str_substr`: `strLen - clamped_start` tagged `nuw+nsw` (after start is clamped to `[0, strLen]`).
  - `str_replace`: loop tail `strLen - consumed` tagged `nuw+nsw` (`bSrc` is bounded by `strPtr+strLen`).
  - `str_trim`/`str_trim_left`/`str_trim_right`: `trimEnd - trimStart` tagged `nuw+nsw` (scan invariant).
  - String subscript `str[i]`: `zext i8→i64` result now carries `!range [0, 256)` via `charRangeMD_` and is inserted into `nonNegValues_`.
  - Last bare `CreateZExt` in the mul-by-zero comparison fold migrated to `emitBoolZExt`.

- **Round-20: builtin boolean metadata** (`src/codegen_builtins.cpp`):
  - `emitBoolZExt` applied to all boolean-returning builtins: `is_alpha`, `is_digit`, `is_upper`, `is_lower`, `is_space`, `is_alnum`, `str_eq`, `str_contains`, `str_starts_with`, `str_ends_with`, `is_even`, `bool(x)` cast.
  - `map_has` call result tagged `!range [0, 2)` + `nonNegValues_`.
  - `char_at` `zext` result tagged `!range [0, 256)` via `charRangeMD_`.

- **Round-19: `emitBoolZExt` helper + `or disjoint`** (`src/codegen.cpp`, `src/codegen_expr.cpp`):
  - New helper `emitBoolZExt(i1 val, name)`: emits `zext nneg` + `!range [0, 2)` + `nonNegValues_` tracking in one call. Replaced all 20 bare `CreateZExt` in `codegen_expr.cpp` (float comparisons, integer comparisons, `&&`/`||`, `!`, scmp/pcmp, mul-by-zero).
  - `or disjoint` flag at O1+: when KnownBits proves the LHS and RHS have no shared 1-bits, the `or` instruction is flagged `disjoint`, enabling LLVM to treat it as `add`.

- **Round-18: scalar alloca metadata** (`src/codegen.cpp`, `src/codegen_expr.cpp`, `src/codegen_stmt.cpp`):
  - `!tbaa` (`tbaaScalar_`) on all scalar alloca loads/stores: identifier loads in `generateIdentifier`, increment/decrement loads in `generateIncDec`, `VarDecl` init stores, for-loop `condBB`/`incBB`/assume loads and stores, foreach index loads/stores, and parameter init stores.
  - `lshr exact` flag via KnownBits: when the shift amount is a known-positive constant and the input has that many known-zero trailing bits, `CreateLShr` is emitted with `isExact=true`.
  - While-loop preheader assumption loads gain `!noundef` + `!tbaa` + `!range [0, INT64_MAX)`.

- **Round-17: IR quality overhaul** (`src/codegen.cpp`, `src/codegen_expr.cpp`, `src/codegen_stmt.cpp`):
  - `toDefaultType`: `i1` and unsigned values use `ZExt`; all other signed narrow integers (`i2`–`i63`) use `SExt` so that `i32 -1` extends to `i64 -1`, not `2^32-1`.
  - `splatScalarToVector`: uses `PoisonValue` as the undef lane (avoids unnecessary materialization).
  - `putchar`/`puts`/`fputs`: marked `nosync`.
  - `qsort`: marked `nounwind`, `willreturn`, `nocapture(3)`.
  - `emitShiftAdd`: all generated `shl`/`add` instructions carry `nuw`+`nsw`.
  - `NSWNeg` emitted when the negated operand is in `nonNegValues_`.
  - `range_step` multiply and add carry `nsw`+`nuw`.
  - All alloca/global-init stores carry `tbaaScalar_`.

- **Round-16: IR quality improvements** (`src/codegen_expr.cpp`):
  - **Identity cast elimination**: `as`-cast on same-size integer types (e.g. `i32 as i32`) now returns the value directly instead of emitting a `bitcast` instruction. Opaque pointer→pointer casts also return directly (all pointers share the `ptr` type). Fallback `as`-cast path guards `srcTy == dstTy` before creating a `bitcast`.
  - **`nonNegValues_` propagation for all boolean/comparison results**: Float comparison results (`==`, `!=`, `<`, `<=`, `>`, `>=`), short-circuit constant-fold paths (`true && x`, `false || x`), mul-by-zero strength-reduction comparisons, subtraction-comparison folds, string comparison results (including the fast/slow PHI), and pointer comparison results are now all tracked in `nonNegValues_`. This allows downstream arithmetic (add/sub/mul) on boolean values to pick up `nsw`/`nuw` flags and unsigned `icmp` variants, improving LLVM's SCEV analysis and loop vectorization.

- **Round-15: compound condition range narrowing** (`src/var_range_analysis.cpp`):
  - `narrowFromCondition` now handles `&&` (AND) and `||` (OR) compound conditions.
  - `if (x > 0 && y > 0)` — both `x` and `y` ranges are narrowed in the true branch.
  - `if (x > 0 || y > 0)` else branch — both ranges narrowed (both conditions false).
  - Correctly conservative for the cases where narrowing cannot be proven (OR taken, AND not-taken).

- **Round-14B: parser diagnostic guards** (`src/parser.cpp`):
  - `parseShift()`: warns when a shift count literal exceeds 63 (undefined behaviour in LLVM IR).
  - `parseMultiplication()`: warns when the right-hand literal of `/` or `%` is zero (division by zero).

- **Round-14B: algebraic simplification rules** (`src/alg_simp_pass.cpp`):
  - `x + x → x * 2` for same-identifier operands (always valid for any numeric type).
  - `(-x) + (-y) → -(x + y)` — factoring out the negation from addition.
  - `x - (-y) → x + y` — double-negation cancellation on the subtrahend.
  - `(-x) * (-y) → x * y` — product of two negations is positive.

- **Round-14B: LICM for FOR loops** (`src/hgoe_egraph.cpp`):
  - `visitBlock()` now performs a conservative loop-invariant code motion pass after expression
    optimisation: `VarDecl` statements in a `FOR_STMT` body whose initializer is pure and does
    not reference the loop iterator or any variable written inside the loop are hoisted to
    immediately before the loop, ensuring they are computed only once.
  - Three new static helpers support the analysis: `exprRefersToAny`, `exprIsPure`, and
    `collectWrittenVars`.

- **`eliminateDeadFunctions` API** (`src/program_analysis.cpp`, `include/program_analysis.h`):
  - New public function `eliminateDeadFunctions(Module&, ProgramFactsSnapshot&)` that removes
    internal/private functions identified as unreachable by `computeProgramFacts()`.
  - Avoids re-running BFS by reusing `snapshot.unreachableFunctions`; only removes functions
    with `hasLocalLinkage()` and no remaining users.

- **Escape analysis** (`src/program_analysis.cpp`, `include/program_analysis.h`):
  - New `FunctionSnapshot::hasEscapedLocals` field; set to `true` when any alloca address is
    stored to memory, returned, or passed to a non-pure callee.
  - Enables downstream passes (mem2reg, alias analysis) to skip escape-free functions.

- **`definitelyNonNeg` helper** (`src/var_range_analysis.cpp`, `include/var_range_analysis.h`):
  - Returns `true` when `evalExprRange(expr, env)` yields a range with `lo >= 0`.
  - Used by `AlgSimpPass` and `CodeGenerator` for strength reductions and comparison folding.

### Fixed

- **Round-14 optimization-pass ordering** (`src/optimization_manager.cpp`):
  - Added `g.addDependency(F::kCSE, F::kCopyProp)`: copy-propagation now runs before CSE,
    creating more CSE opportunities by substituting canonical copies.
  - Added `g.addDependency(F::kEGraph, F::kAlgSimp)`: algebraic simplification now runs before
    the e-graph pass, reducing the e-graph search space and avoiding redundant rewrites.

- **Round-14 builtin constant folds** (`src/codegen_builtins.cpp`):
  - `abs(x)` when `x` is known non-negative: folded to identity (returns `x` directly, no intrinsic).
  - `min(x, x)` / `max(x, x)`: folded to identity (returns the argument directly).
  - `clamp(val, lo, lo)` when both bounds are the same value: folded to `lo` directly.
  - `pow(2, n)` integer fast path: replaced O(log n) binary-exponentiation loop with a
    single `shl` + `select`; negative exponents yield 0 (consistent with runtime semantics).

## [4.5.0] - 2026-05-08

### Changed

- **`invalidate` deferred-free semantics** (`src/codegen_stmt.cpp`, `include/codegen.h`):
  - Logical invalidation is still instantaneous: any use of an invalidated variable after the `invalidate` statement is a compile-time error (borrow checker + `deadVars_` detection unchanged).
  - Physical `free()` is now **deferred** to the function's exit point instead of being emitted at the `invalidate` site.  Every heap pointer queued by `invalidate` statements is freed in a single **batch** just before `ret` (or `throw` dispatch), giving the allocator a dense sequence of `free()` calls that it can merge and LLVM's optimizer a wider window to reorder, hoist, or sink them.
  - Implemented via `deferredFreeQueue_` (a per-function `vector<Value*>` in `CodeGenerator`) and a new `emitDeferredFrees()` helper called from `generateReturn()` and `generateThrow()`.
  - Arena-backed and stack-backed pointers continue to be handled exactly as before (no `free()` for arena, `lifetime.end` only for stack).

### Fixed

- **Round-6 statement/expression traversal** (`src/copy_prop_pass.cpp`, `src/alg_simp_pass.cpp`, `src/hgoe_egraph.cpp`, `src/egraph_optimizer.cpp`, `src/var_range_analysis.cpp`, `src/uniqueness_analysis.cpp`, `src/borrow_checker.cpp`, `src/opt_orchestrator.cpp`):
  - All remaining AST node types now handled in every analysis and transform pass: `MOVE_DECL`, `PREFETCH_STMT`, `PIPELINE_STMT`, `ASSUME_STMT` (deoptBody), `INVALIDATE_STMT`, and all expression types including `ARRAY_EXPR`, `STRUCT_LITERAL_EXPR`, `SPREAD_EXPR`, `PIPE_EXPR`, `MOVE_EXPR`, `BORROW_EXPR`, `REBORROW_EXPR`, `DICT_EXPR`, `RANGE_ANNOT_EXPR`.

- **Round-7 optimization-pass coverage** (`src/width_opt_pass.cpp`, `src/cse_pass.cpp`, `src/dce_pass.cpp`):
  - `width_opt_pass.cpp` `transformStmtInPlace`: added `THROW_STMT`, `DEFER_STMT`, `CATCH_STMT`, `ASSUME_STMT`, `PREFETCH_STMT`, and `PIPELINE_STMT` so that sub-expressions inside those constructs are now subject to integer-width narrowing.
  - `cse_pass.cpp` `collectOpaqueVarsInStmt`: added `ASSUME_STMT` (recurses into deoptBody), `PREFETCH_STMT` (registers volatile/atomic prefetch-declared vars), and `PIPELINE_STMT` (recurses into each stage body) so CSE correctly avoids sinking loads across opaque barriers inside those constructs.
  - `dce_pass.cpp` `transformStmt`: added `ASSUME_STMT` (recurses into deoptBody), `PREFETCH_STMT` (explicit leaf case), and `PIPELINE_STMT` (recurses into each stage's `BlockStmt` via `transformBlock`) so dead-code elimination propagates into all reachable sub-statements.

- **Round-8 correctness audit** (`src/cfctre.cpp`, `src/alg_simp_pass.cpp`, `src/borrow_checker.cpp`, `src/codegen_expr.cpp`):
  - `cfctre.cpp`: Fixed 6 C++ undefined-behavior issues in the compile-time evaluator:
    - `>>` and `>>=` operators used C++ arithmetic (signed) right-shift instead of logical (unsigned), diverging from OmScript semantics; fixed by casting operands to `uint64_t` before shifting.
    - Unary negation of `INT64_MIN` is signed overflow (UB); fixed with `uint64_t` two's-complement negation.
    - `abs(INT64_MIN)` in the built-in `abs` evaluator overflows; guarded with early return of `INT64_MIN`.
    - `std::abs(INT64_MIN)` in `gcd` and `lcm` built-in evaluators is UB; replaced with explicit unsigned-cast negation.
    - `rotate_left`/`rotate_right` with shift amount 0 caused `x >> 64` / `x << 64` (UB for 64-bit integers); guarded with early return for `sh == 0`.
  - `alg_simp_pass.cpp`: Constant-fold guard for `INT64_MIN / -1` and `INT64_MIN % -1` (SIGFPE on x86-64) was missing from the literal-folding path; added explicit guards matching `cfctre.cpp`.
  - `borrow_checker.cpp`: `ASSUME_STMT` was not handled in `checkStmt`, so use-after-move errors inside `assume(cond)` conditions and deopt bodies were silently missed; added explicit case that checks both the condition expression and the optional deopt body.
  - `codegen_expr.cpp`: SIMD vector `>>` emitted `CreateAShr` (arithmetic shift) while the scalar path and language semantics mandate `CreateLShr` (logical shift); changed to `CreateLShr`.

- **Round-11 optimization improvements** (`src/var_range_analysis.cpp`, `src/width_legalization.cpp`, `src/cfctre.cpp`, `src/copy_prop_pass.cpp`):
  - `var_range_analysis.cpp` `scanStmt`: Added `EXPR_STMT` case so that reassignment expressions (e.g. `x = a & 0xFF`) update the live range map; previously only `VAR_DECL` declarations triggered range narrowing, causing subsequent reassignments to leave stale over-wide ranges.
  - `width_legalization.cpp`: `x & literal` now returns `fromUnsignedValue(literal)` when the literal is a non-negative integer, producing 8/16/32-bit widths for masks like `0xFF`/`0xFFFF`/`0xFFFFFFFF` instead of the over-wide bitwise default. `x % N` now returns `fromUnsignedValue(N - 1)` for a known positive literal divisor, bounding the width to fit `[0, N-1]`.
  - `cfctre.cpp`: Added same-value identity folds for built-in calls — `min(x, x) → x` and `max(x, x) → x` fire before the concrete-integer path so they apply to symbolic operands; `pow(x, 0) → 1` checks the exponent before requiring a concrete base, enabling the fold when only the exponent is a literal `0`.
  - `copy_prop_pass.cpp` `collectOpaqueVarsInStmt`: Added `ASSUME_STMT` case so that volatile/atomic variable declarations inside `assume` deopt bodies are correctly added to the opaque set; previously such declarations were invisible to copy propagation, allowing incorrect substitution across opaque barriers.

- **Round-13 optimization audit** (`src/egraph.cpp`, `src/hgoe_egraph.cpp`, `src/synthesize.cpp`, `src/opt_orchestrator.cpp`):
  - `egraph.cpp`: Confirmed all 16 target algebraic rewrite rules are present — `sub_self` (x−x→0), `xor_self` (x^x→0), `and_self`/`or_self` (idempotent AND/OR), `or_zero`/`xor_zero` (identity with 0), `and_all_ones` (x&−1→x), `add_zero_right`/`add_zero_left` (x+0→x), `mul_one_right`/`mul_one_left` (x∗1→x), `shl_zero`/`shr_zero` (x⟨⟨0→x), `zero_sub_to_neg` (0−x→−x), `sub_zero` (x−0→x), `double_bitnot` (~(~x)→x), `double_neg` (−(−x)→x), `add_sub_cancel_right` ((x+y)−y→x), and `sub_add_cancel` ((x−y)+y→x). No rules were missing; no changes required.
  - `hgoe_egraph.cpp`: Confirmed `Op::Mod` is grouped with `Op::Div` at 25-cycle cost (matching single `idiv` instruction latency); `Op::Call` already carries a 10-cycle cost with `memoryPressure = 1.0`; `Op::Load`/`Op::Store` do not exist in the egraph `Op` enum (memory ops are modelled via `NodeMeta::readsMemory`/`writesMemory` flags rather than separate opcodes). No changes required.
  - `synthesize.cpp`: Confirmed both `SynthOp::SHL` and `SynthOp::SHR` eval paths apply `& 63` to the shift amount; no residual `& 62` or `& 30` off-by-one masks remain. No changes required.
  - `opt_orchestrator.cpp`: Confirmed `checkStmt` handles all statement types that carry sub-expressions — `BLOCK`, `VAR_DECL`, `MOVE_DECL`, `RETURN_STMT`, `EXPR_STMT`, `IF_STMT`, `WHILE_STMT`, `DO_WHILE_STMT`, `FOR_STMT`, `FOR_EACH_STMT`, `SWITCH_STMT`, `CATCH_STMT`, `DEFER_STMT`, `THROW_STMT`, `PREFETCH_STMT`, `ASSUME_STMT`, and `PIPELINE_STMT`; `INVALIDATE_STMT` and `FREEZE_STMT` contain only a `varName` string (no sub-expression to analyse) and are correctly handled by `default: break`. No changes required.

- **Round-12 traversal audit** (`src/rlc_pass.cpp`, `src/sdr_pass.cpp`, `src/codegen_expr.cpp`):
  - `rlc_pass.cpp`: Audit confirmed `THROW_STMT`, `INVALIDATE_STMT`, `ASSUME_STMT`, and `PIPELINE_STMT` are all already handled in both `stmtUsesVar` and `renameInStmt` traversals — no changes needed.
  - `sdr_pass.cpp`: Pass operates entirely on LLVM IR (not AST); all values generated from `THROW_STMT`, `ASSUME_STMT`, `PIPELINE_STMT`, and `PREFETCH_STMT` are visible as LLVM instructions — no AST-level traversal gaps.
  - `codegen_expr.cpp`: Division by power-of-2 fast path is already present — unsigned operands and non-negative tracked values both emit `CreateLShr` (with `udiv.lshr`/`div.lshr` names) instead of `CreateUDiv`/`CreateSDiv`; also covers modulo by power-of-2 via `CreateAnd` mask — no changes needed.

## [4.4.0] - 2026-05-07

### Added

- **`@semantics(willreturn)` annotation** (`include/ast.h`, `src/parser.cpp`, `src/codegen.cpp`):
  - Applies LLVM `WillReturn` attribute to the generated function.
  - Asserts that every execution of the function will eventually return to the caller (no infinite loops, no `abort()`-class calls).
  - Enables dead-store elimination, load-forwarding, and LICM across call sites that LLVM's inter-procedural analysis cannot otherwise prove safe.
  - Emits a parse-time warning when combined with `@semantics(noreturn)` (contradictory; `noreturn` wins).

- **`@semantics(nosync)` annotation** (`include/ast.h`, `src/parser.cpp`, `src/codegen.cpp`):
  - Applies LLVM `NoSync` attribute.
  - Asserts that the function contains no synchronization primitives (mutexes, atomics, memory barriers, blocking I/O).
  - Enables the optimizer to reorder or speculate calls across this function's boundary without breaking happens-before guarantees.

- **`@semantics(nofree)` annotation** (`include/ast.h`, `src/parser.cpp`, `src/codegen.cpp`):
  - Applies LLVM `NoFree` attribute.
  - Asserts that the function does not free (deallocate) any memory reachable through its pointer arguments or global state.
  - Allows alias analysis to prove that pointer values remain valid across the call, enabling store elimination, LICM, and vectorization of surrounding code.

  **Usage**:
  ```omscript
  @semantics(pure, willreturn, nofree)
  fn fastHash(data: int) -> int { ... }

  @semantics(nosync, nofree)
  fn processBuffer(buf: ptr, len: int) -> void { ... }
  ```

### Fixed

- **Optimization pipeline: missing `extern` PassId declarations** (`include/opt_pass.h`):
  - `PassId::kDCE`, `kCSE`, `kAlgSimp`, `kCopyProp`, and `kRLC` were defined as `uint32_t` variables in `opt_orchestrator.cpp` but had no corresponding `extern` declaration in the public header. Any translation unit that included `opt_pass.h` and referenced these names could not link.
  - Added the five missing `extern uint32_t` declarations to the `namespace PassId` block in `opt_pass.h`.

- **Optimization pipeline: incomplete cascade-invalidation dependency graph** (`src/optimization_manager.cpp`):
  - `AnalysisDependencyGraph::createDefault()` previously wired only 10 of the 23 analysis dependency edges. The 13 missing edges — covering `rlc`, `dce`, `cse`, `alg_simp`, `copy_prop`, `width_legalization`, `width_opt`, and `hgoe_egraph` — meant that calling `ctx.validity().invalidate("dce")` would not cascade-invalidate `cse`, `alg_simp`, or `copy_prop`. In `runInvalidated()` mode, downstream passes silently operated on stale analysis.
  - Added all 13 missing dependency edges with explanatory comments.

- **Optimization pipeline: incorrect `invalidates_` declarations in pass metadata** (`src/opt_orchestrator.cpp`):
  - Five AST-rewriting passes declared incomplete invalidation sets. Width legalization and width-opt facts were never listed as stale even though they depend entirely on expression shapes.
  - `egraph`: `{}` → `{range_analysis, cse, width_legalization, width_opt}`
  - `hgoe_egraph`: `{range_analysis}` → `{range_analysis, cse, width_legalization, width_opt}`
  - `alg_simp`: `{range_analysis}` → `{range_analysis, width_legalization, width_opt}`
  - `copy_prop`: `{cse, range_analysis}` → `{cse, range_analysis, width_legalization, width_opt}`
  - `dce`: `{range_analysis}` → `{range_analysis, width_legalization, width_opt}`

- **Optimization pipeline: `CostTransform` passes ran unconditionally at O0** (`src/opt_orchestrator.cpp`):
  - DCE, CSE, AlgSimp, and the width optimiser are classified as `PassKind::CostTransform` — they exist to improve runtime performance, not to ensure correctness. They were always run regardless of the active optimization level.
  - At O0 the user expects minimal compile time and maximum AST fidelity for debugger step-level accuracy; running these passes modified the AST unpredictably.
  - Added an O-level gate to `runPassPipeline`: `CostTransform` passes are now skipped when `optLevel_ == OptimizationLevel::O0`. `Analysis` and `SemanticTransform` passes are unaffected.

- **Optimization pipeline: per-pass wrappers performed redundant manual invalidation** (`src/opt_orchestrator.cpp`):
  - `runDCE`, `runAlgSimp`, and `runCopyProp` each explicitly set downstream validity flags to `false` immediately before `PassScheduler::applyInvalidation()` performs the same operation through the metadata-driven cascade. The manual assignments were redundant and obscured the single source of truth for invalidation policy.
  - Removed the manual flag assignments; `applyInvalidation()` is now the sole invalidation path for these three passes.

- **IR quality: callee attributes not propagated to call instructions** (`src/codegen_builtins.cpp`):
  - `generateCall` (user function call path) did not copy the callee's `WillReturn`, `NoSync`, `NoFree`, or `MemoryEffects` attributes to the `CallInst`. LLVM's LICM, DSE, and call-site devirtualization passes operate on `CallInst` attributes directly — without them, passes running before inlining could not hoist pure calls out of loops or eliminate their stores.
  - Now, after creating the `CallInst`, `generateCall` copies all four attribute classes to the call instruction. Callee memory effects are propagated only when they are not `unknown()` so that the existing conservative path is not regressed.

- **IR quality: reborrow element GEP missing `inbounds` + non-wrapping flags** (`src/codegen_stmt.cpp`):
  - The partial-borrow path (`borrow arr[i]`) emitted a plain `CreateGEP` without `inbounds` and without `nuw`/`nsw` on the `idx+1` offset computation.
  - The borrow checker guarantees `0 ≤ idx < len`, which means `idx+1` cannot overflow unsigned (`nuw`) or signed (`nsw`), and the resulting pointer is within the array's malloc'd slab (`inbounds`). These flags are now set so that GVN, LICM, and alias analysis can fold/hoist through reborrow-derived pointer expressions.

- **CSE pass: incorrect CSE of volatile/atomic variables in `foreach`/`do-while`/`switch`/`catch`/`defer` bodies** (`src/cse_pass.cpp`):
  - `collectOpaqueVars` (which marks variable names as non-CSE-able) only recursed into `if`/`while`/`for` bodies. Variables declared `volatile` or `atomic` inside `for each`, `do...while`, `switch`, `catch`, or `defer` blocks were invisible to the opaque set, allowing the CSE pass to incorrectly fold their repeated reads into a single cached copy — violating the `volatile`/`atomic` semantics that every read must reach the underlying storage.
  - Refactored into a recursive `collectOpaqueVarsInStmt` dispatch that handles all compound statement forms. `collectOpaqueVars` and `collectOpaqueVarsInList` now delegate to it, so the opaque set is always complete before CSE runs.

- **Copy-propagation pass: same `collectOpaqueVarsInStmt` gap for `switch`/`catch`/`defer`** (`src/copy_prop_pass.cpp`):
  - The equivalent `collectOpaqueVarsInStmt` helper in the copy-propagation pass also lacked `SWITCH_STMT`, `CATCH_STMT`, and `DEFER_STMT` cases. This could allow copies of volatile/atomic variables declared in those blocks to be forwarded across reads — the same semantic violation as the CSE bug above.
  - Added the three missing cases with the same pattern already used by `rlc_pass.cpp` and `var_range_analysis.cpp`.

- **Variable-range analysis: `collectWritten` missed `catch`/`defer` bodies** (`src/var_range_analysis.cpp`):
  - `collectWritten` (used to conservatively widen loop-iteration variable ranges when the loop body may reassign them) handled `switch` cases but not `catch` or `defer` bodies. A variable reassigned inside a `catch` or `defer` block nested in a loop would not be widened, potentially keeping a stale narrowed range. This could cause incorrect range-guided optimizations (e.g., mis-folding comparisons) on variables that are actually overwritten on the exceptional path.
  - Added `CATCH_STMT` and `DEFER_STMT` cases to `collectWritten`.

- **E-graph optimizer and HGOE pass: `catch`/`defer` bodies not traversed** (`src/egraph_optimizer.cpp`, `src/hgoe_egraph.cpp`):
  - `optimizeStatementImpl` in `egraph_optimizer.cpp` and `visitStmt` in `hgoe_egraph.cpp` did not recurse into `CATCH_STMT` or `DEFER_STMT` bodies. Expressions inside catch-handler and defer blocks were silently skipped, leaving valid e-graph and HGOE rewrites (e.g., strength reductions, algebraic simplifications) on the table.
  - `hgoe_egraph.cpp::visitStmt` also missed `DO_WHILE_STMT` and `SWITCH_STMT`.
  - Added the missing cases to both files. `opt_orchestrator.cpp`'s `checkStmt` preflight validator received the same additions (`CATCH_STMT`, `DEFER_STMT`, `THROW_STMT`) for completeness.

### Documentation

- **`LANGUAGE_REFERENCE.md`**:
  - §6.6 `@semantics` table: `willreturn`, `nosync`, `nofree` rows with LLVM attribute mapping and optimizer-effect descriptions.
  - §25.2.3 Analysis dependency graph: corrected from the old 7-edge stub to all 22 edges now in `createDefault()`.
  - §25.3 Per-O-level pass list: added `PassKind` O-level gating note; documented `CostTransform` passes are skipped at O0 and listed in their correct pipeline position at O1/O2.
  - §25.6 LLVM IR quality guarantees (new section): documents every unconditional per-function attribute, O2+ additions, call-site attribute propagation, and load/store metadata emitted by the code generator. Corrected: `!range` on call sites is metadata on `CallInst`, not a function return attribute; the inaccurate "LLVM 19+" qualifier was removed.
  - §33: bumped version to `4.4.0`.
- **`README.md`**: updated "Current version" badge to `4.4.0`.
- **`include/version.h`**: bumped `OMSCRIPT_VERSION_MINOR` to `4` and `OMSCRIPT_VERSION_PATCH` to `0`.
- **`src/codegen_stmt.cpp`**: reborrow GEP comments updated to explain borrow-checker preconditions enabling `inbounds`/`nuw`/`nsw`.

## [4.4.1] - 2026-05-08

### Fixed

- **Copy-propagation: `collectWrittenDeep` misses `switch`/`catch`/`defer`** (`src/copy_prop_pass.cpp`):
  - `collectWrittenDeep` — used by `killAndRecurseBody` to conservatively kill all variables written inside a compound body before recursing into it — did not recurse into `SWITCH_STMT` case bodies, `CATCH_STMT` bodies, or `DEFER_STMT` bodies. This meant that when a loop body contained a `switch`, `catch`, or `defer` statement, variables assigned inside those sub-blocks were not killed before the loop was entered. A stale copy (e.g., `y → x` from before the loop) could remain alive even though `y` is unconditionally assigned a new value inside the loop body's switch arm, allowing the pass to incorrectly forward the pre-loop value of `x` for reads of `y` after the loop.
  - Added `SWITCH_STMT`, `CATCH_STMT`, and `DEFER_STMT` cases to `collectWrittenDeep`.

- **Copy-propagation: `propagateInBlock` does not process `switch`/`catch`/`defer` bodies** (`src/copy_prop_pass.cpp`):
  - `propagateInBlock` had no cases for `SWITCH_STMT`, `CATCH_STMT`, or `DEFER_STMT`. Both effects were wrong: (1) copy-propagation opportunities inside those bodies were silently missed; (2) after the block, variables written inside the block had not been killed from the copy map, so the pass could still forward stale copies to code appearing after the `switch`/`catch`/`defer` in the same enclosing block.
  - Added handling for all three statement types: propagate into the condition (for `switch`), kill all writes across all cases/body, then recurse with the updated map.

- **Variable-range analysis: `scanStmt` misses `for each`/`switch`/`catch`/`defer`** (`src/var_range_analysis.cpp`):
  - `scanStmt` updated the range environment only for `if`, `for`, `while`, `do-while`, and bare blocks. Statements of type `FOR_EACH_STMT`, `SWITCH_STMT`, `CATCH_STMT`, and `DEFER_STMT` hit `default: break`, so: (a) variable declarations inside those bodies were never added to the range environment; (b) variables reassigned inside those bodies were never invalidated, leaving falsely-narrowed ranges for the enclosing scope.
  - Added all four missing cases, each with the same "collect writes, erase from env, then recurse" pattern used by `for`/`while`/`do-while`.

- **Uniqueness analysis: `catch`/`defer` bodies not traversed** (`src/uniqueness_analysis.cpp`):
  - Phase 1 (`collectStringArrayVars`) and Phase 2 (`markSharedVars`) both stopped at `SWITCH_STMT` (which was already handled) without covering `CATCH_STMT` or `DEFER_STMT`. String/array variables declared or aliased exclusively inside catch or defer handlers were invisible to the analysis, making the compiler falsely assume they are uniquely owned. This could suppress necessary `strdup`/copy-on-write operations for those variables.
  - Added `CATCH_STMT` and `DEFER_STMT` traversal to both phases.

- **Borrow checker: `catch` handler bodies not checked** (`src/borrow_checker.cpp`):
  - The borrow checker's statement dispatcher had no `CATCH_STMT` case. Catch handler bodies were silently skipped, meaning: borrows made inside a catch block were never validated; use-after-move errors in catch handlers were not reported; and borrows that should be released before the handler exits were not tracked.
  - Added a `CATCH_STMT` case that pushes a borrow scope, processes all statements in the handler body, then pops the scope — the same pattern used for loop bodies and block statements.

- **`stmtCallsAny`: `switch`/`catch`/`defer` bodies not searched for concurrency primitives** (`src/codegen.cpp`):
  - `stmtCallsAny` (used by `usesConcurrencyPrimitive` to decide whether a function body contains any call to `thread_create`, `mutex_lock`, etc.) did not recurse into `SWITCH_STMT` case bodies, `CATCH_STMT` bodies, or `DEFER_STMT` bodies. A function that only called concurrency primitives inside a switch arm or a deferred cleanup block would be incorrectly classified as "not concurrent", potentially allowing the effect-inference pass to attach `nosync` or downgrade memory-ordering assumptions.
  - Added `SWITCH_STMT`, `CATCH_STMT`, and `DEFER_STMT` cases to `stmtCallsAny`. Also added the three corresponding `using omscript::` declarations to the anonymous namespace so the type names resolve without qualification.

- **Copy-propagation: `propagateInExpr` does not recurse into field-access and field-assign expressions** (`src/copy_prop_pass.cpp`):
  - `propagateInExpr` handled `BINARY_EXPR`, `UNARY_EXPR`, `TERNARY_EXPR`, `CALL_EXPR`, `ASSIGN_EXPR`, `INDEX_EXPR`, `INDEX_ASSIGN_EXPR`, `POSTFIX_EXPR`, and `PREFIX_EXPR`, but had no cases for `FIELD_ACCESS_EXPR` or `FIELD_ASSIGN_EXPR`. Sub-expressions inside a struct field access (`obj.field`) or assignment (`obj.field = rhs`) were silently skipped, so copies could not be propagated into either the object expression or the right-hand-side value of a field assignment.
  - Added both cases: `FIELD_ACCESS_EXPR` recurses into the `object` sub-expression; `FIELD_ASSIGN_EXPR` recurses into both `object` and `value`.

- **Algebraic simplification: `simplifyExpr` does not recurse into index/field sub-expressions** (`src/alg_simp_pass.cpp`):
  - The `simplifyExpr` bottom-up recurser had no cases for `INDEX_EXPR`, `INDEX_ASSIGN_EXPR`, `FIELD_ACCESS_EXPR`, `FIELD_ASSIGN_EXPR`, or `ASSIGN_EXPR`. Algebraic simplification rules (constant folding, identity elimination, strength reduction) were silently skipped for sub-expressions inside array indexing (`arr[i+0]`), element writes, struct field reads and writes, and direct assignment RHS values.
  - Added the five missing cases. Each recurses into all child sub-expressions so that bottom-up folding works through nested containers.

- **HGOE pass: `visitExpr` does not recurse into `INDEX_ASSIGN_EXPR`/`FIELD_ACCESS_EXPR`/`FIELD_ASSIGN_EXPR`** (`src/hgoe_egraph.cpp`):
  - `visitExpr` handled `INDEX_EXPR` but not `INDEX_ASSIGN_EXPR`, `FIELD_ACCESS_EXPR`, or `FIELD_ASSIGN_EXPR`. Sub-expressions inside element writes and struct field expressions were not visited by the HGOE strength-reduction pass, leaving valid rewrites (e.g., strength-reductions of index arithmetic or field-value computations) undiscovered.
  - Added all three missing cases.

- **Multiple passes: `MOVE_DECL` statements not treated as variable writes or definitions** (`src/copy_prop_pass.cpp`, `src/var_range_analysis.cpp`, `src/alg_simp_pass.cpp`, `src/uniqueness_analysis.cpp`):
  - `MOVE_DECL` (the `var y = move x` statement) declares a new variable `y` and consumes `x`. Every analysis and transformation pass that handled `VAR_DECL` failed to also handle `MOVE_DECL`:
    - **Copy propagation**: neither `collectWrittenInStmt`, `collectWrittenDeep`, nor `propagateInBlock` knew about `MOVE_DECL`. The destination name was not added to "writes", so `killAndRecurseBody` did not kill it; and after a `move`, any prior copy-map entry for the source was not invalidated, allowing the pass to continue forwarding a stale value for a variable that was just moved-from.
    - **Variable-range analysis**: `collectWritten` did not include the destination name, so range invalidation for loop bodies containing `move` declarations was incomplete. `scanStmt` did not compute the new variable's range or invalidate the moved-from variable's range.
    - **Algebraic simplification**: `simplifyStmt` did not recurse into the `MoveDecl` initializer, leaving constant-fold and identity-elimination opportunities inside `move` initializer expressions unreachable.
    - **Uniqueness analysis**: neither `collectStringArrayVars` nor `markSharedVars` handled `MOVE_DECL`. String/array variables declared via `move` were invisible to the analysis, causing under-approximation of the unique-string set and potentially unsafe `strdup`-skip decisions.
  - Added `MOVE_DECL` handling to all four passes with semantics appropriate for each.

- **Multiple passes: `PREFETCH_STMT` with embedded `VarDecl` not tracked as a variable declaration** (`src/copy_prop_pass.cpp`, `src/var_range_analysis.cpp`, `src/alg_simp_pass.cpp`, `src/uniqueness_analysis.cpp`, `src/borrow_checker.cpp`, `src/egraph_optimizer.cpp`, `src/hgoe_egraph.cpp`):
  - `prefetch [hot] var x = expr` embeds a `VarDecl` inside a `PrefetchStmt`. None of the analysis/transform passes recognised this form. As a result, the variable declared by the prefetch statement was:
    - Not added to "written" sets in `copy_prop`, `var_range` — so `killAndRecurseBody` / range invalidation did not kill it on loop back-edges.
    - Not tracked for range narrowing in `var_range`.
    - Not simplified (the `addrExpr` and `varDecl->initializer` were not folded) by `alg_simp`, `egraph_optimizer`, and `hgoe`.
    - Invisible to `uniqueness_analysis` — string/array prefetch variables had undefined uniqueness.
    - Not scope-checked by `borrow_checker` — ownership effects of the embedded declaration were ignored.
  - Added `PREFETCH_STMT` handling to all seven passes.

- **Multiple passes: `PIPELINE_STMT` stages not traversed** (`src/copy_prop_pass.cpp`, `src/var_range_analysis.cpp`, `src/alg_simp_pass.cpp`, `src/uniqueness_analysis.cpp`, `src/borrow_checker.cpp`, `src/egraph_optimizer.cpp`, `src/hgoe_egraph.cpp`):
  - `pipeline N { stage name { ... } ... }` sequences multiple named stage bodies. No analysis or transformation pass (except `rlc_pass.cpp`) traversed the stage bodies. Writes inside stages were not killed in range/copy maps, algebraic rules were not applied to stage expressions, string/array vars inside stages were invisible to uniqueness analysis, and borrow-checker did not scope-check stage bodies.
  - Added `PIPELINE_STMT` handling to all seven passes. Stage bodies are each treated as a separate nested scope, and the `count` expression is treated as a loop-count operand.

- **Multiple passes: `ASSUME_STMT` deopt body not traversed; `ASSUME_STMT` absent from `egraph_optimizer` and `hgoe_egraph`** (`src/alg_simp_pass.cpp`, `src/copy_prop_pass.cpp`, `src/egraph_optimizer.cpp`, `src/hgoe_egraph.cpp`, `src/var_range_analysis.cpp`):
  - `assume(cond) else { deoptBody }` has an optional deopt body that runs if the assumption is violated. `alg_simp_pass` and `copy_prop_pass` already visited the `condition` but skipped the `deoptBody`. Neither `egraph_optimizer` nor `hgoe_egraph` handled `ASSUME_STMT` at all. Additionally, `var_range_analysis` did not exploit `assume` conditions for range narrowing.
  - Fixed all five passes: deopt body is now recursed in `alg_simp` and `copy_prop`; `egraph_optimizer` and `hgoe_egraph` now visit both the condition and deopt body; `var_range_analysis` now calls `narrowFromCondition` on the assume condition to propagate the proven constraint into subsequent code.

- **`INVALIDATE_STMT` not erasing the variable's range in `var_range_analysis`** (`src/var_range_analysis.cpp`):
  - `invalidate x;` marks variable `x` as dead (no longer usable). The variable-range pass did not handle this statement, so stale range constraints could persist through an invalidation point, potentially allowing the pass to emit incorrect bounds for `x` after it was invalidated.
  - Added `INVALIDATE_STMT` to `scanStmt`: the variable's range entry is now erased when an invalidation is encountered.

- **Multiple passes: expression traversal gaps for `ARRAY_EXPR`, `STRUCT_LITERAL_EXPR`, `SPREAD_EXPR`, `PIPE_EXPR`, `MOVE_EXPR`, `BORROW_EXPR`, `REBORROW_EXPR`, `DICT_EXPR`, `RANGE_ANNOT_EXPR`** (`src/copy_prop_pass.cpp` `propagateInExpr`; `src/alg_simp_pass.cpp` `simplifyExpr`; `src/hgoe_egraph.cpp` `visitExpr`):
  - All three bottom-up expression traversers had `default: break` for every OmScript-specific expression type. Sub-expressions inside array literals, struct literals, spread operators, pipe operators, move/borrow/reborrow expressions, dict literals, and range annotations were silently skipped.
  - Added all nine missing cases to each traverser. Each case recurses into all reachable child sub-expressions.

- **`opt_orchestrator` preflight check: `PREFETCH_STMT`, `PIPELINE_STMT`, `ASSUME_STMT` not checked; `checkExpr` missing 10 expression kinds** (`src/opt_orchestrator.cpp`):
  - The preflight `checkStmt` and `checkExpr` lambdas in `OptimizationOrchestrator::runPreflightCheck()` perform pre-pass validity checks (including division-by-zero detection). They did not handle `PREFETCH_STMT`, `PIPELINE_STMT`, or `ASSUME_STMT` in `checkStmt`, meaning sub-expressions in those statements were never checked. `checkExpr` lacked cases for `INDEX_ASSIGN_EXPR`, `FIELD_ACCESS_EXPR`, `FIELD_ASSIGN_EXPR`, `SPREAD_EXPR`, `PIPE_EXPR`, `MOVE_EXPR`, `BORROW_EXPR`, `REBORROW_EXPR`, `RANGE_ANNOT_EXPR`, `STRUCT_LITERAL_EXPR`, `DICT_EXPR`, and `ARRAY_EXPR`.
  - Added all missing cases to both `checkStmt` and `checkExpr`.

## [4.3.2] - 2026-05-07

### Fixed

- **E-graph: `div_self` rule fires for `x = 0`** (`src/egraph.cpp`):
  - The rule `x / x → 1` previously had no guard, causing it to silently fold `0 / 0 → 1` (a division-by-zero that should trap at runtime).
  - Added a guard that requires `x` to be a provably non-zero constant.

- **E-graph: `mod_self` rule fires for `x = 0`** (`src/egraph.cpp`):
  - The rule `x % x → 0` previously had no guard, causing `0 % 0 → 0` (undefined behavior) to be silently optimized away.
  - Added a guard that requires `x` to be a provably non-zero constant.

- **E-graph: `mul_div_cancel` unguarded algebraic version shadows guarded relational version** (`src/egraph.cpp`):
  - `getAlgebraicRules()` registered `mul_div_cancel` without a non-zero divisor guard. `getRelationalRules()` registered a correctly guarded version under the same name. `getAllRules()` deduplicates by keeping only the first occurrence, so the guarded version was silently discarded, allowing `(x * 0) / 0 → x` to fire.
  - Renamed the algebraic rule to `mul_div_cancel_alg` and added a non-zero guard to it, so both rules coexist in the rule set.

- **E-graph: `range_check_unsigned` produces wrong results for negative `x`** (`src/egraph.cpp`):
  - The rule `(x >= C1) && (x <= C2) → (x - C1) <= (C2 - C1)` is only semantically valid with **unsigned** comparison. The EGraph only has signed `Op::Le`, so the rewrite was incorrect for negative `x` (signed subtraction overflow + incorrect signed compare could flip the result).
  - Disabled the rule with a detailed comment. It can be re-enabled once an unsigned comparison operator is added to the `Op` enum.

- **E-graph: `mask_shift_normalize` uses arithmetic (signed) C++ shift for `newMask`** (`src/egraph.cpp`):
  - The rule `(x & mask) >> a → (x >> a) & (mask >> a)` computed the new mask constant via C++ signed right-shift. OmScript's `>>` operator maps to `CreateLShr` (logical / unsigned-fill). For negative mask values the arithmetic shift fills high bits with ones while the logical shift fills with zeros, producing an incorrect constant.
  - Changed to cast through `unsigned long long` before shifting: `static_cast<long long>(static_cast<unsigned long long>(*m) >> static_cast<unsigned>(*a))`.

- **`rotate_left` / `rotate_right` constant fold: undefined behavior when rotation amount is 0** (`src/codegen_builtins.cpp`):
  - The constant-fold paths for both builtins computed `v >> (64 - amt)` and `v << (64 - amt)`. When `amt == 0`, `64 - amt = 64` and shifting a 64-bit value by its own width is undefined behavior in C++.
  - Added `amt == 0` identity short-circuit: when the rotation amount is zero the value is returned unchanged.

- **Preprocessor: `\<newline>` line continuation inserts a spurious space** (`src/preprocessor.cpp`):
  - The line-joining step appended `' '` when it saw `\<LF>`, breaking macro-name splicing (e.g., `MY_\<newline>MACRO` produced `MY_ MACRO` instead of `MY_MACRO`).
  - Changed to a zero-character splice (no character appended), matching C preprocessor semantics. Also handles `\<CR><LF>` Windows line endings.

- **Debug build profile incorrectly enables `optMax`** (`src/project.cpp`):
  - `BuildProfile::makeDebug()` had `p.optMax = true` despite setting `optLevel = O0` and `debugInfo = true`. Maximum optimization passes defeat debug-info fidelity and step-level correctness.
  - Changed to `p.optMax = false` in the debug profile.

- **`scanImports` fails to recognize `import` when separated from the path by a newline** (`src/build_graph.cpp`):
  - The whitespace-skip loop after the `import` keyword only consumed spaces and tabs, not `\n` or `\r`. A multi-line import statement (`import\n"file.om"`) was silently skipped by the incremental build scanner.
  - Added `\n` and `\r` to the accepted whitespace characters.

- **`functionsChanged` statistics counter is never incremented** (`src/opt_context.cpp`):
  - `EGraphSubsystem::optimizeFunction()` checked whether `stats_.expressionsSimplified` had increased after calling `egraph::optimizeFunction()`. However, `egraph::optimizeFunction()` never updates that counter (only `optimizeExpression()` does), so `functionsChanged` was always 0.
  - Changed to unconditionally increment `functionsChanged` after each `optimizeFunction()` call, with a comment explaining the limitation.

- **`version.h` inconsistency: component macros said 4.3.0 but `OMSC_VERSION` said "4.5.0"** (`include/version.h`):
  - All version constants are now consistently set to 4.3.2.

### Documentation

- **`LANGUAGE_REFERENCE.md`**:
  - §2.6: Fixed `>>` operator description. The operator always performs a **logical** (zero-fill) right shift via LLVM `CreateLShr`, regardless of whether the value is signed or unsigned. The previous description ("arithmetic for signed, logical for unsigned") was incorrect.
  - §3.3: Updated `__VERSION__` predefined macro value to `"4.3.2"`. Updated `__VECTOR_WIDTH__` and `__SIMD_*__` descriptions to clarify these are detected at **runtime** via `llvm::sys::getHostCPUFeatures()`, not at C compile time.
  - §3.4: Rewrote Line Continuation section to document **zero-character splice** semantics: `\<newline>` removes both characters with no separator, enabling macro-name splitting across lines.
  - §33: Updated version number from `4.1.1` to `4.3.2`. Updated LLVM compatibility note (LLVM 18 primary). Updated source-compatibility note to reference v4.3.

- **`README.md`**: Updated "Current version" badge to 4.3.2.



### Added

- **`@align(N)` / `@align()` — full-body alignment** (`include/ast.h`, `src/parser.cpp`, `src/codegen.cpp`):
  - `@align(N)` aligns the function entry point to exactly N bytes (power-of-two required).
  - `@align()` (no argument, sentinel `hintAlign = -1`) selects **cache-line-optimal alignment (64 bytes)** for the function entry point AND every local variable `alloca` emitted in the function body. This maximises cache locality for hot functions.
  - Default (no annotation): 16-byte entry alignment at O2+; 32-byte for `@hot` functions.

- **`@speculatable` function annotation** (`include/ast.h`, `src/parser.cpp`, `src/codegen.cpp`):
  - Adds LLVM `Speculatable` attribute to the generated function.
  - Allows the optimizer to hoist or speculate calls across branches and into loop preheaders.
  - Safe only when the function has no observable side effects, does not trap, and does not read/write memory visible to callers.
  - Strongest when combined with `@pure`: `@speculatable @pure fn ...`

- **`@repr(...)` struct layout annotation** (`include/ast.h`, `include/parser.h`, `src/parser.cpp`, `include/codegen.h`, `src/codegen.cpp`, `src/codegen_stmt.cpp`):
  - `@repr(C)` — stable, ABI-compatible C layout (fields in declaration order, natural alignment on the struct's `alloca`).
  - `@repr(packed)` — minimal memory, no padding (`isPacked = true` LLVM StructType).
  - `@repr(align(N))` — force the struct's `alloca` alignment to N bytes.
  - `@repr(auto)` — compiler optimizes layout freely (default, equivalent to no annotation).
  - `@repr(soa)` — structure-of-arrays hint (recorded for future layout passes).
  - Parsed immediately before the `struct` keyword at the top level.
  - Stored in `StructDecl::repr` (new `StructRepr` enum in `ast.h`) and `StructDecl::reprAlignN`.

- **Documentation** (`LANGUAGE_REFERENCE.md`):
  - Added `@align(N)` / `@align()` section (§6.6).
  - Added `@speculatable` section (§6.6).
  - Added `@repr(...)` section (§4.4.6 and §10.5).
  - Added all previously undocumented annotations to Additional Annotations (§6.6): `@noreturn`, `@restrict`, `@noalias`, `@nounwind`, `@const_eval`, `@minsize`, `@optnone`, `@parallel`, `@noparallel`, `@nounroll`.

## [4.3.0] - 2026-05-04

### Added

- **`include/ast_arena.h`** — new arena-allocation infrastructure for the compiler:
  - **`BumpAllocator`** — a slab-based bump-pointer arena allocator.  Memory is carved from fixed-size slabs (default 64 KiB, doubling up to 4 MiB cap).  Provides:
    - `alloc(n, align)` — O(1) raw byte allocation.
    - `make<T>(args…)` — typed placement-new with automatic destructor registration for non-trivially-destructible types.
    - `copyString(string_view)` — copy a string into the arena and return a NUL-terminated `const char*` valid for the arena's lifetime.
    - `reset()` — call all registered destructors and reset the bump pointer (retains slab memory for reuse).
    - `bytesAllocated()` / `bytesUsed()` / `slabCount()` — allocation statistics for profiling.
  - **`StringHash` / `StringEqual`** — transparent hash and equality functors with `using is_transparent = void`.  Enable `std::unordered_map<std::string_view, V>` to be queried directly with `std::string`, `std::string_view`, or `const char*` keys without constructing a temporary `std::string`.

- **`pass_utils.h`** — added two shared type-name predicate helpers, centralising logic that was previously duplicated across `parser.cpp` and `opt_context.h`:
  - `isIntWidthTypeName(std::string_view)` — returns true for `iN`/`uN` integer-width cast names (N in [1, 256]), e.g. `i8`, `u32`, `i128`.
  - `isKnownScalarTypeName(std::string_view)` — returns true for any of the built-in OmScript scalar/aggregate type keywords (`int`, `float`, `double`, `bool`, `string`, `dict`, `bigint`, `ptr`) or any integer-width type.

### Changed

- **`BuiltinEffectTable` public API** (`include/opt_context.h`) — all `static bool` predicate methods and `get()` now accept `std::string_view` instead of `const std::string&`.  Callers with `std::string` values pass without allocation (implicit conversion); callers with string literals or `string_view` values no longer construct a temporary `std::string`.

- **`BuiltinEffectTable` internal table** (`src/opt_context.cpp`) — changed from `std::unordered_map<std::string, BuiltinEffects>` (heap-allocated keys) to `std::unordered_map<std::string_view, BuiltinEffects>` (zero-allocation keys; all entries point to string literals with static storage duration).  Lookup is O(1) and allocation-free for all callers.

- **`BuiltinEffectTable::isWidthCastName`** — replaced two `nm.substr(0, 5)` calls (each allocating a temporary `std::string`) with `nm.compare(0, 5, …)` (zero-allocation prefix comparison).  The per-entry `iN`/`uN` check now delegates to the canonical `isIntWidthTypeName` helper from `pass_utils.h`, eliminating the second copy of that logic.

- **`src/parser.cpp`** — removed the local-file-scope `isIntWidthTypeName` and `isKnownTypeName` static functions (which duplicated logic from `opt_context.h` and `pass_utils.h`).  Both call sites now use the shared `isIntWidthTypeName` and `isKnownScalarTypeName` from `pass_utils.h`.

## [4.2.0] - 2026-05-04

### Added

- **`atomic` variable qualifier** — `atomic var name: type = value;` declares a variable whose every load, store, and read-modify-write operation is emitted as a sequentially-consistent (seq-cst) atomic instruction at the LLVM IR level.
  - All loads compile to `load atomic i64 … seq_cst`.
  - All stores (plain assignment and initializer) compile to `store atomic i64 … seq_cst`.
  - Increment/decrement (`++`/`--`) compile to `atomicrmw add/sub … seq_cst` — the operation is indivisible with respect to all threads.
  - Compound assignments (`+=`, `-=`, `&=`, `|=`, `^=`) detect the `x = x OP rhs` pattern and also emit a single `atomicrmw add/sub/and/or/xor … seq_cst` instruction.
  - Atomic variables receive natural ABI alignment on their `alloca` so the LLVM backend can satisfy hardware alignment requirements for lock-free atomic instructions.
  - Atomic variables are excluded from the CopyProp and CSE AST passes — their values can change between any two reads, so forwarding or hoisting their loads is unsound.
  - `!invariant.load` and `!noundef` metadata are suppressed on atomic loads.
  ```omscript
  global atomic var counter: i64 = 0;

  fn worker() {
      counter++;              // atomicrmw add … seq_cst
      return 0;
  }

  fn main() {
      var t1 = thread_create("worker");
      var t2 = thread_create("worker");
      thread_join(t1);
      thread_join(t2);
      println(counter);       // always 2 — no mutex needed
      return 0;
  }
  ```

- **`volatile` variable qualifier** — `volatile var name: type = value;` declares a variable whose every load and store is marked volatile in the LLVM IR, preventing the compiler from eliding, caching, reordering, or CSE-ing memory operations on it.
  - All loads compile to `load volatile i64 …`.
  - All stores compile to `store volatile i64 …`.
  - Increment/decrement loads and stores are both marked volatile.
  - Volatile variables are excluded from the CopyProp and CSE AST passes.
  - `!invariant.load` and `!noundef` metadata are suppressed on volatile loads.
  - Intended for memory-mapped I/O registers, signal handlers, and other variables whose value may change externally without any visible write in the program.
  ```omscript
  volatile var status: i64 = 0;

  fn poll() -> i64 {
      while (status == 0) {}  // status re-read every iteration
      return status;
  }
  ```

- **`atomic volatile var`** — both qualifiers may be combined in either order (`atomic volatile var` or `volatile atomic var`). The resulting variable has both atomic ordering (seq-cst) and volatile semantics on every access.
  ```omscript
  atomic volatile var hw_reg: i64 = 0;
  ```

  See §5.5 and §5.6 of the Language Reference for the full specification, and §20.3 for the concurrency context.

---

## [4.1.1] - 2026-04-15

### Added

- **CF-CTRE (Cross-Function Compile-Time Reasoning Engine)** — new compiler phase inserted between AST pre-analysis and LLVM IR generation.  CF-CTRE is a deterministic SSA-semantics interpreter embedded in the compiler that executes pure functions across function-call boundaries at compile time, memoises results, and preserves pipeline SIMD tile semantics.  Key capabilities:
  - **Cross-function evaluation** — `comptime { chain(encode(x)) }` descends into all transitively-called pure functions, not just builtins.
  - **Memoisation** — each unique `(function, args)` combination is evaluated at most once; subsequent identical calls reuse the cached result (O(1) lookup).
  - **Array results** — pure functions that return arrays are fully evaluated; the result is emitted as a `private unnamed_addr constant [N+1 × i64]` global (no heap allocation at runtime).
  - **Pipeline SIMD tile semantics** — loops are processed as tiles of 8 × `u64` lanes; partial final tiles are masked; one tile always executes even if `n < 8` (matching the hardware SIMD target model).
  - **Fixed-point purity analysis** — whole-program analysis detects pure functions without requiring explicit `@pure` annotations; handles mutual recursion conservatively.
  - **Specialization** — calls with all-constant arguments produce specialised results cached by argument hash.
  - **Depth + budget guards** — depth limit 128 frames, instruction budget 10 000 000 per compilation unit; both violations silently fall back to runtime.
  - **Back-propagation** — zero-arg pure function results are back-propagated into the legacy `constIntReturnFunctions_` / `constStringReturnFunctions_` tables for full compatibility with all existing fold helpers.
  - **`@const_eval` override** — forces a function eligible for CF-CTRE regardless of the purity analysis.
  - **`--verbose` stats** — prints `[cfctre] Pass complete: N functions registered, M pure, K calls memoised, A arrays allocated`.
  See §28 of the Language Reference for the full specification.

- **Integer type-cast syntax** — `u64(x)`, `u32(x)`, `u16(x)`, `u8(x)`, `i64(x)`, `i32(x)`, `i16(x)`, `i8(x)`, `bool(x)` are now recognized as function-call-style type coercions. They work both in normal compiled code and inside `comptime` blocks:
  - `u64(x)`, `i64(x)`, `int(x)`, `uint(x)` — identity (all OmScript integers are `i64`)
  - `u32(x)` — mask to lower 32 bits (`x & 0xFFFFFFFF`)
  - `i32(x)` — `trunc` + `sext` to 32 bits (preserves signed value in range)
  - `u16(x)` — mask to lower 16 bits (`x & 0xFFFF`)
  - `i16(x)` — `trunc` + `sext` to 16 bits
  - `u8(x)` — mask to lower 8 bits (`x & 0xFF`)
  - `i8(x)` — `trunc` + `sext` to 8 bits
  - `bool(x)` — normalize to 0 or 1 (`x != 0 ? 1 : 0`)
  All variants fold at compile time in `comptime` blocks via `evalConstBuiltin`.

- **`comptime {}` — array-returning user functions** — `comptime` blocks can now call user-defined functions that return arrays. The entire function body is evaluated at compile time by the constant evaluator; the result is emitted as a `private unnamed_addr constant [N+1 × i64]` global with OmScript's `[length, elem0, …]` array layout. No runtime allocation, no function call:
  ```omscript
  fn str_to_u64_fast(s:string) -> u64[] {
      var n:int = len(s);
      var blocks:int = (n + 7) >> 3;
      var out:u64[] = array_fill(blocks, 0);
      for (i:int in 0...blocks) {
          var base:int = i << 3;
          var x:u64 = 0;
          if (base + 0 < n) { x |= u64(s[base + 0]) << 0;  }
          // ... (full 8 byte lanes)
          out[i] = x;
      }
      return out;
  }
  // Fully evaluated at compile time — zero runtime overhead:
  var M:u64[] = comptime { str_to_u64_fast("hello"); };
  ```

- **`comptime {}` — implicit return** — The last bare expression statement in a `comptime` block is now the implicit return value, so `comptime { expr; }` works without an explicit `return` keyword. An explicit `return` still works. This matches block-expression semantics in other modern languages.

- **`emitComptimeArray` helper** — new internal codegen helper that allocates `[N+1 × i64] private unnamed_addr constant` globals with OmScript's array layout and returns the base pointer as `i64`. Used by both the COMPTIME_EXPR emitter and the call-site constant folder for array-returning pure functions.

- **Array results in call-site constant folding** — pure user functions that return arrays are now fully folded at their call sites (not just inside `comptime` blocks). When the compiler detects that all arguments to a pure function are compile-time constants and the function body can be fully evaluated, array results are emitted as global constants. The callee is registered in `arrayReturningFunctions_` so downstream analysis correctly tracks the variable as array-typed.

---

## [4.1.0] - 2026-04-14

### Changed (compiler optimization improvements)

- **Struct field load/store IR quality improvements** — `generateFieldAccess` and `generateFieldAssign` now emit higher-quality IR:
  - All struct field loads now use `CreateAlignedLoad(8)` (previously bare `CreateLoad`). All OmScript structs are `malloc`'d or stack-allocated as `[N x i64]` alloca, so every field pointer is naturally 8-byte aligned; making this explicit in the IR lets LLVM's backend pick optimal load instructions and enables the vectorizer to merge adjacent field loads.
  - At O1+, all struct field loads now carry `!noundef` metadata, matching the invariant on variable loads and array element loads. OmScript struct fields are always initialized from struct literals before use (the ownership system prevents accessing uninitialized fields), so `!noundef` is a guaranteed property that enables LLVM to speculate, hoist, and CSE field reads.
  - Struct literal initialization stores now use `CreateAlignedStore(8)` for consistency with the aligned loads.

- **`generateIncDec` IR quality improvements** — the `++`/`--` operators on integer variables now emit higher-quality IR that allows LLVM to perform stronger downstream optimizations:
  - The load emitted inside `x++`/`x--` is now a proper aligned load (`CreateAlignedLoad`, 8-byte alignment) matching what `generateIdentifier` emits for regular variable reads.
  - At O1+, the load carries `!noundef` metadata, matching the OmScript invariant that all variables are initialized before use. This enables LLVM to speculate, CSE, and hoist the load more aggressively.
  - When the variable is tracked as non-negative (in `nonNegValues_`), the load now carries `!range [0, INT64_MAX)` metadata (with tight upper bound if available), making non-negativity visible to every IR-level pass (LVI, CVP, InstCombine, SCEV) — not just through `llvm.assume` intrinsics.
  - `x++` on a non-negative variable now sets the `nuw` (no unsigned wrap) flag on the increment instruction in addition to the existing `nsw` flag. When `x ∈ [0, INT64_MAX]`, `x + 1 ∈ [1, INT64_MAX]` which is strictly less than UINT64_MAX, so unsigned wrap is impossible. The `nuw` flag enables LLVM's SCEV to compute tighter unsigned ranges and helps the loop unroller propagate non-negativity to unrolled copies.
  - After `x++` on a non-negative variable, both the alloca and the result value are inserted into `nonNegValues_`, so downstream operations (e.g. `y = x + 1` after `x++`) can also benefit from `nuw+nsw`. After `x--`, the alloca is conservatively removed from `nonNegValues_` (the result may be −1 if `x` was 0).

### Changed (production-readiness / code quality)

- **Version bumped to 4.1.0** — reflects the full set of IR quality, constant-folding, and language improvements added since 4.0.0.
- **Preprocessor errors use `DiagnosticError`** — all `throw std::runtime_error(...)` in `preprocessor.cpp` have been replaced with `throw DiagnosticError(Diagnostic{...})`, the unified error type used throughout the rest of the compiler. Error messages now include a structured `SourceLocation` (filename + line number) so diagnostic renderers can emit consistent, machine-readable output.
- **E-graph internal invariant violations use `llvm::report_fatal_error`** — two raw `assert()` calls in `egraph.cpp` have been replaced with `llvm::report_fatal_error(...)`. This ensures that out-of-range ClassId accesses produce a clear, attributed fatal error via LLVM's error-handling infrastructure rather than a silent `SIGABRT` in release builds (where `NDEBUG` disables `assert`).
- **Unreachable path in `parser.cpp` uses `llvm_unreachable`** — the `throw std::logic_error("unreachable")` after `error()` in `Parser::consume()` has been replaced with `llvm_unreachable(...)`. This uses LLVM's standard mechanism for annotating provably unreachable code: it becomes a no-op in release builds (letting the compiler see the unreachable path) and prints a diagnostic + aborts in debug builds.
- **Removed unused `<cassert>` include** from `egraph.cpp`.


- **Constant folding extended to newer builtins** — the following builtins are now folded at compile time when their arguments are literal constants, eliminating the function call and all associated runtime overhead entirely:
  - `str_to_int("42")` → integer `42`
  - `str_pad_left("hi", 5, " ")` → interned string `"   hi"`
  - `str_pad_right("hi", 5, " ")` → interned string `"hi   "`
  - `sum([1, 2, 3])` → integer `6`
  - `sum(array_fill(5, 3))` → integer `15` (= 5×3), avoids allocation entirely
  - `sum(range(1, 6))` → integer `15` (arithmetic series formula), avoids allocation
  - `array_product([2, 3, 4])` → integer `24`
  - `array_last([10, 20, 30])` → integer `30`
  - `array_min([5, 2, 8, 1])` → integer `1`
  - `array_max([5, 2, 8, 1])` → integer `8`
  - `array_contains([1, 2, 3], 2)` → integer `1`
  - `array_find([10, 20, 30], 20)` → integer `1`
  - `index_of([10, 20, 30], 30)` → integer `2`
  - `len(range(3, 8))` → integer `5`
  - `len(range_step(0, 10, 2))` → integer `5`
  - `len(str_chars(s))` → `strlen(s)` (avoids allocating the char array)
  - `len(array_concat([1,2], [3,4,5]))` → integer `5` via evalConstBuiltin chain
  - `len(array_slice([1,2,3,4,5], 1, 4))` → integer `3` via evalConstBuiltin chain
  Each has a matching IR-level early-out (before generating loop/branch IR) and is also handled by `evalConstBuiltin` for cross-function constant propagation via `tryConstEvalFull`.

- **Purity analysis coverage** — `str_to_int`, `str_pad_left`, `str_pad_right`, `sum`, `array_product`, `array_last`, `array_min`, `array_max`, `array_contains`, `index_of`, `array_find`, `array_fill`, `array_concat`, `array_slice`, `array_copy`, `str_chars`, `str_join`, `range`, `range_step`, `log10`, `exp`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `cbrt`, `hypot`, `fma`, `copysign`, `min_float`, and `max_float` are now registered in `kPureBuiltins`, enabling `autoDetectConstEvalFunctions` to classify user functions that call these builtins as pure and eligible for cross-function constant propagation.

- **`stdlibFunctions` completeness** — `array_insert`, `array_last`, `array_product`, `str_pad_left`, `str_pad_right`, `fma`, `copysign`, `min_float`, and `max_float` were missing from the canonical stdlib function set used to validate `@optmax` function bodies. They are now registered, preventing false "cannot invoke non-OPTMAX function" errors.

- **`!noundef` metadata on `INDEX_OF` and `ARRAY_CONTAINS` loops** — element loads in these two search loops were missing `!noundef` metadata at O1+. Added to be consistent with all other array element loads in the codebase, enabling LLVM's undefined-value elimination passes.

- **`evalConstBuiltin` enriched** — `array_min`, `array_max`, `array_contains`, `index_of`, `array_find`, `array_fill`, `array_concat`, `array_slice`, `typeof`, `str_chars`, `startswith`/`endswith` aliases, `log10`, and `sum` (via constant array) are now evaluated at compile time by the abstract constant evaluator used for inter-procedural constant propagation (`tryConstEvalFull`). This enables chains like `array_find(array_concat([1,2],[3,4]), 3)` → `2` at compile time.


### Added

- **`comptime {}` blocks** — expressions evaluated entirely at compile time and stored to a variable. Any `comptime {}` initializer participates in the constant-folding chain just like a `const` declaration:
  ```omscript
  var a = comptime { return 6 * 7; };        // a = 42, folded
  var b = comptime { var n = 5; return n * n; }; // b = 25
  ```
- **`parallel` keyword on loops** — prepend `parallel` to `for`, `while`, or `foreach` to assert that iterations are independent and allow auto-parallelization via `llvm.loop.parallel_accesses` metadata:
  ```omscript
  parallel for (i in 0...n) { sum = sum + arr[i]; }
  ```
- **`@loop(independent=true)`** — emits `llvm.access.group` on all memory operations in the loop body and attaches `llvm.loop.parallel_accesses` metadata, telling LLVM there are no loop-carried memory dependencies. More precise than `parallel` for scalar loops.
- **`@loop(fuse=true)`** — adjacent `for` loops over the same range (same `start` and `end`) are merged into a single loop body at the AST level, reducing loop overhead and improving data locality.
- **Escape analysis for small arrays** — array literals of ≤ 16 integer constants that are proven not to escape their declaring scope are stack-allocated (`alloca`) instead of heap-allocated (`malloc`), eliminating the allocation entirely.
- **Freeze correctness** — `freeze x;` now emits an actual LLVM `freeze` instruction on the variable's current value and stores it back, guaranteeing non-poison semantics. Freeze state also propagates to borrow aliases: freezing a source variable freezes all its borrows, and vice versa.
- **`reborrow` keyword** — creates a new borrow reference from an existing borrow, or borrows a sub-element (partial borrow):
  ```omscript
  borrow ref = &arr;
  reborrow sub = &arr[0];     // partial borrow of first element
  reborrow mut sub = &arr[0]; // partial mutable borrow
  ```
- **`@allocator(size=N)` annotation** — marks a user-defined function as an allocator. Applies LLVM `allocsize(N)` attribute, `noalias` on the return value, and `willreturn`/`nounwind`, enabling LLVM to reason about heap-allocated regions created by user code:
  ```omscript
  @allocator(size=0)
  fn my_alloc(nbytes) { return malloc(nbytes); }
  ```
- **OptStats tracking** — the compiler now counts constant folds, stack-allocated arrays (escape analysis hits), frozen aliases, and loop fusions performed. When run with `-v` (verbose), a summary is printed after compilation.
- **Constant folding extended**:
  - `sqrt(N)`, `floor(N)`, `ceil(N)`, `round(N)`, `exp2(N)` with integer literal arguments are folded at compile time.
  - `exp2`, `log`, `lcm`, `is_power_of_2`, `popcount`, `clz`, `ctz`, `bswap`, `bitreverse`, `rotate_left`, `rotate_right`, `saturating_add`, `saturating_sub`, all `str_*` builtins — added to `evalConstBuiltin` and `kPureBuiltins` for cross-function propagation.
  - `comptime {}` sub-expressions participate in `tryFoldExprToConst` and `tryConstEvalFull`, enabling deeper compile-time evaluation chains.
  - `tryConstEvalFull` now checks `constIntFolds_` and `constStringFolds_` for global constants, so functions referencing file-level `const` variables can be folded cross-function.
- **Purity analysis extended** — `autoDetectConstEvalFunctions` now recognises `COMPTIME_EXPR` (always pure), `PREFIX_EXPR` (`!`/`-`/`+` are pure), `POSTFIX_EXPR` (mutations, not pure), `BREAK_STMT`, `CONTINUE_STMT`, `SWITCH_STMT` as pure statement forms. The `kPureBuiltins` set covers all string and math builtins.

### Changed
- Version bumped to **4.0.0** (major: new ownership extensions, `comptime`, `parallel`, `reborrow`).

## [3.9.0] - 2026-04-11

### Added
- **Method-call syntax: `obj.method(args)` desugars to `method(obj, args)`** (syntactic improvement).
  Any dot-followed-by-call expression is transparently rewritten in the parser so that the receiver becomes the first argument of the named function. This enables an idiomatic, object-oriented style without introducing a new runtime model:
  - `arr.push(42)` → `push(arr, 42)` — array methods
  - `str.len()` → `len(str)` — string/any builtin that takes a single value
  - `str.str_upper()` → `str_upper(str)` — prefixed builtins
  - `point.translate(dx, dy)` → `translate(point, dx, dy)` — user-defined struct methods
  - Chains (`c = c.increment(1)`) work naturally since each call returns the result.
  - Zero runtime overhead — generates identical code to the direct function-call form.
- **New example/test program** `examples/method_call_test.om` covering struct methods, array methods, string methods, and sequential chaining (expected exit 129).
- **Integration test** for `method_call_test.om` added to `run_tests.sh`.

### Changed (codegen improvement)
- **Per-field TBAA for struct field accesses.** Each `(struct_type, field_index)` pair now gets its own unique TBAA type node that is a child of the shared `struct field` root type. Previously all struct field loads and stores shared a single `tbaaStructField_` TBAA access tag, so LLVM had to assume that reading one field might alias writing any other. With per-field nodes:
  - Stores to `p.x` and loads from `p.y` are proven non-aliasing → LLVM can reorder/CSE them.
  - Generic (unknown-field) accesses that retain the old `tbaaStructField_` tag still correctly alias all per-field accesses because the shared type is their common ancestor.
  - Per-field nodes are created lazily and cached in `tbaaStructFieldCache_`.

## [3.8.0] - 2026-04-11

### Added
- **5 new built-in functions** for array and string manipulation:
  - `array_product(arr)` — multiplies all elements together; returns `1` for an empty array (the multiplicative identity). Mirrors `sum()`. Uses a vectorization-friendly loop with `mustprogress` metadata.
  - `array_last(arr)` — returns the last element of an array; aborts with a runtime error on an empty array. Branch weights favour the non-empty path (1000:1).
  - `array_insert(arr, idx, val)` — inserts `val` at index `idx`, shifting subsequent elements right. Returns a **new array** (the original is unchanged). `idx` must be in `[0, length]`; inserting at `length` is equivalent to appending. Bounds-checked with a 1000:1 hot/cold branch weight.
  - `str_pad_left(str, width, fill)` — left-pads `str` with the first character of `fill` until the string is at least `width` characters long. Returns `str` unchanged if it already meets or exceeds `width`.
  - `str_pad_right(str, width, fill)` — right-pads `str` with the first character of `fill`. Returns `str` unchanged if it already meets or exceeds `width`.
- **New example/test program** `examples/array_string_builtins_test.om` covering all five new builtins with 5 independent sub-tests.
- **Integration test** for `array_string_builtins_test.om` (expected exit 178) added to `run_tests.sh`.

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
