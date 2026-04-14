# OmScript Language Reference

> **Version:** 4.0.0
> **Compiler:** `omsc` — OmScript Compiler
> **Backend:** LLVM 18+ · Ahead-of-Time Compilation
> **License:** See repository root

---

## Table of Contents

1. [Overview](#1-overview)
2. [Compilation Pipeline](#2-compilation-pipeline)
3. [Lexical Structure](#3-lexical-structure)
4. [Preprocessor](#4-preprocessor)
5. [Types and Values](#5-types-and-values)
6. [Variables and Constants](#6-variables-and-constants)
7. [Functions](#7-functions)
8. [Control Flow](#8-control-flow)
9. [Loops](#9-loops)
10. [Expressions and Operators](#10-expressions-and-operators)
11. [Arrays](#11-arrays)
12. [Strings](#12-strings)
13. [Dictionaries / Maps](#13-dictionaries--maps)
14. [Structs](#14-structs)
15. [Enums](#15-enums)
16. [Error Handling](#16-error-handling)
17. [Memory Semantics and Ownership System](#17-memory-semantics-and-ownership-system)
    - 17.0 Ownership States
    - 17.1 move
    - 17.2 borrow / borrow mut
    - 17.3 invalidate
    - 17.4 freeze
    - 17.5 reborrow + partial borrow
    - 17.6 prefetch
    - 17.7 No-Aliasing Guarantee
18. [OPTMAX Blocks](#18-optmax-blocks)
19. [Built-in Functions](#19-built-in-functions)
20. [Concurrency](#20-concurrency)
21. [File I/O](#21-file-io)
22. [Lambda Expressions](#22-lambda-expressions)
23. [Import System](#23-import-system)
24. [Compiler CLI Reference](#24-compiler-cli-reference)
25. [Advanced Optimization Features](#25-advanced-optimization-features)
    - 25.1 E-Graph Equality Saturation
    - 25.2 Superoptimizer (+ Branch-to-Select)
    - 25.3 Hardware Graph Optimization Engine (HGOE)
    - 25.4 OPTMAX Blocks
    - 25.5 Profile Guidance (PGO)
    - 25.6 Escape Analysis (Stack Allocation)
    - 25.7 Bounds Check Hoisting
    - 25.8 Allocation Elimination
    - 25.9 SROA (Struct/Array → Scalars)
    - 25.10 Reduction Recognition
    - 25.11 OptStats

---

## 1. Overview

OmScript is a statically-compiled, dynamically-typed language with optional type annotations. It compiles through LLVM to native machine code. Key features:

- **Dynamic typing with optional annotations** — variables are untyped by default; annotations enable advanced optimizations and SIMD types
- **LLVM backend** — ahead-of-time compilation to native binaries
- **Four-stage optimizer** — AST pre-passes (comptime evaluation, loop fusion, escape analysis), e-graph equality saturation, superoptimizer, and hardware graph optimization engine (HGOE)
- **C-compatible performance** — designed to match C performance with high-level syntax
- **Compile-time ownership system** — `move`, `borrow`, `borrow mut`, `freeze`, `reborrow`, and `invalidate` keywords for static lifetime tracking and aggressive alias optimizations
- **`comptime {}` blocks** — arbitrary expressions evaluated entirely at compile time and substituted as constants
- **`parallel` loops** — assert iteration independence for auto-parallelization
- **Loop annotations** — `@loop(independent=true)`, `@loop(fuse=true)`, `@loop(unroll=N)`, `@loop(vectorize=true/false)`, `@loop(tile=N)`

---

## 2. Compilation Pipeline

```
Source (.om)
  → Preprocessor      (macro expansion, conditional compilation)
  → Lexer             (tokenization)
  → Parser            (AST construction)
  → AST Pre-passes    (comptime eval, loop fusion, constant propagation)
  → Code Generator    (LLVM IR generation, escape analysis, freeze/reborrow)
  → E-Graph           (equality saturation, algebraic identities, O2+)
  → Superoptimizer    (idiom recognition, branch-to-select, O2+)
  → HGOE              (hardware-aware scheduling, FMA fusion, -march/-mtune)
  → LLVM              (standard optimization passes: SROA, mem2reg, GVN, …)
  → Native Binary
```

**AST pre-passes** run before LLVM IR generation and include:

| Pass | Trigger | Effect |
|---|---|---|
| `comptime {}` evaluation | Always | Evaluates constant blocks, substitutes result as literal |
| Cross-function const propagation | O1+ | Inlines pure zero-arg function results as constants |
| Loop fusion | `@loop(fuse=true)` | Merges adjacent same-range loops into one |
| Escape analysis | O1+ | Stack-allocates non-escaping small arrays |
| Constant folding | Always | Folds arithmetic, builtins, string ops with literal args |

---

## 3. Lexical Structure

### 3.1 Comments

```
// Single-line comment
/* Block comment */
```

Block comments do not nest. The first `*/` encountered closes the comment, regardless of any nested `/*` inside it. An unterminated block comment is a compile error.

### 3.2 Keywords

```
fn        return    if        else      elif      unless
while     do        for       foreach   forever   loop
repeat    until     times     with      var       const
register  break     continue  in        true      false
null      switch    case      default   try       catch
throw     enum      struct    import    move      invalidate
borrow    freeze    reborrow  prefetch  likely    unlikely
when      guard     defer     swap      parallel  comptime
```

**v4.0.0 additions:** `freeze`, `reborrow`, `parallel`, `comptime`

### 3.3 Literals

**Integer literals:**
```
42          // decimal
0xFF        // hexadecimal (0x or 0X prefix)
0o77        // octal (0o or 0O prefix)
0b1010      // binary (0b or 0B prefix)
1_000_000   // underscores as separators
```

**Float literals:**
```
3.14
2.0
1_000.5
```

**String literals:**
```
"hello"
"line\nnewline"    // \n, \t, \r, \\, \", \0, \xHH supported
```

**String interpolation:** `$"..."` with `{expr}` placeholders — desugars into a concatenation chain at parse time:
```
var name = "world"
var s = $"hello {name}!"    // equivalent to "hello " + name + "!"
```

**Multi-line string literals:** triple-quoted `"""..."""` — spans multiple lines; all characters between the delimiters are included verbatim:
```
var text = """
line one
line two
"""
```

**Boolean literals:** `true`, `false`

**Null literal:** `null`

### 3.4 Identifiers

Identifiers begin with a letter or underscore and may contain letters, digits, and underscores.

---

## 4. Preprocessor

The preprocessor runs before lexing. All directives begin with `#`.

### 4.1 Macro Definition

```
#define NAME value
#define NAME(param1, param2) body_with_params
#undef NAME
```

**Predefined macros:**

| Macro | Value |
|---|---|
| `__VERSION__` | Compiler version string (e.g., `"3.7.0"`) |
| `__OS__` | `"linux"`, `"macos"`, or `"windows"` |
| `__ARCH__` | `"x86_64"`, `"aarch64"`, `"arm"`, or `"unknown"` |
| `__FILE__` | Current filename |
| `__LINE__` | Current line number |
| `__COUNTER__` | Auto-incrementing integer (unique per use) |

### 4.2 Conditional Compilation

```
#ifdef NAME
#ifndef NAME
#if expr
#elif expr
#else
#endif
```

The `#if`/`#elif` expressions support integer arithmetic, comparisons (`==`, `!=`, `<`, `>`, `<=`, `>=`), and `defined(NAME)`.

### 4.3 Diagnostics

```
#error message           // Compile-time error
#warning message         // Compile-time warning (continues)
#info message            // Compile-time info message (continues)
#assert expr             // Abort with default message if expr == 0
#assert expr "message"   // Abort with custom message if expr == 0
#require "version"       // Error if compiler version is older than required
```

### 4.4 Counter Macros

```
#counter MY_COUNTER      // Creates MY_COUNTER as an auto-incrementing macro
// First use of MY_COUNTER expands to "0", then "1", etc.
```

### 4.5 Line Continuation

A backslash `\` immediately before a newline joins the line with the next:

```
#define LONG_MACRO \
    some_value
```

---

## 5. Types and Values

OmScript is dynamically typed. Type annotations are optional except inside `OPTMAX` blocks (where they are required). Annotated variables enable stronger code generation.

### 5.1 Type Annotations

Type annotations are written with `:` after a name:

```
var x:int = 42
var y:float = 3.14
var s:string = "hello"
```

### 5.2 Scalar Types

| Annotation | Description | LLVM Type |
|---|---|---|
| `int` | 64-bit signed integer (default integer) | `i64` |
| `i64` | 64-bit signed integer | `i64` |
| `u64` | 64-bit unsigned integer | `i64` |
| `i32` | 32-bit signed integer | `i32` |
| `u32` | 32-bit unsigned integer | `i32` |
| `i16` | 16-bit signed integer | `i16` |
| `u16` | 16-bit unsigned integer | `i16` |
| `i8` | 8-bit signed integer | `i8` |
| `u8` | 8-bit unsigned integer | `i8` |
| `float` | 64-bit IEEE 754 double | `f64` |
| `double` | 64-bit IEEE 754 double | `f64` |
| `bool` | Boolean (1-bit) | `i1` |
| `string` | String (heap-allocated, NUL-terminated) | `ptr` |

### 5.3 Array Types

```
var arr:int[]     // array of int
var mat:float[][] // 2D array of float
```

### 5.4 Dictionary Type

```
var d:dict
var d:dict[string, int]   // generic annotation (informational)
```

### 5.5 SIMD Vector Types

Used with type annotations for explicit SIMD programming:

| Annotation | Description |
|---|---|
| `f32x4` | 4 × f32 vector (128-bit SSE) |
| `f32x8` | 8 × f32 vector (256-bit AVX) |
| `f64x2` | 2 × f64 vector (128-bit SSE) |
| `f64x4` | 4 × f64 vector (256-bit AVX) |
| `i32x4` | 4 × i32 vector (128-bit SSE) |
| `i32x8` | 8 × i32 vector (256-bit AVX) |
| `i64x2` | 2 × i64 vector (128-bit SSE) |
| `i64x4` | 4 × i64 vector (256-bit AVX) |

### 5.6 Reference Types

```
var x:&i32     // reference to i32 (same underlying storage)
```

Reference types share the same LLVM representation as their base type; the annotation affects alias analysis.

---

## 6. Variables and Constants

### 6.1 Variable Declaration

```
var x = 42                // mutable, no annotation
var x:int = 42            // mutable, annotated
var arr = [1, 2, 3]       // array
var d = map_new()         // dictionary
```

**Array destructuring** — assigns elements of an array to individual variables in one declaration:

```
var [a, b, c] = arr       // a = arr[0], b = arr[1], c = arr[2]
var [x, _, z] = arr       // use '_' to skip an element
const [first, second] = arr
```

Desugars to a temporary variable plus individual element reads.

### 6.2 Constants

```
const PI = 3.14159
const MAX:int = 100
```

Constants must be initialized at declaration and cannot be reassigned. The compiler performs constant folding at compile time.

### 6.3 Compile-Time Blocks (`comptime`)

A `comptime {}` block is executed **entirely at compile time** by the constant evaluator. The result must be a constant integer or string value, which is then substituted at the call site as a literal — no runtime code is emitted.

```
var a = comptime { return 6 * 7; };          // a = 42, compile-time constant
var b = comptime { var n = 5; return n * n; }; // b = 25
const c = comptime { return min(10, 20); };  // c = 10
```

**Rules:**
- The body must be a sequence of `var` declarations and a final `return <expr>` where all values are statically known.
- Builtin math functions (`abs`, `min`, `max`, `pow`, `sqrt`, `floor`, `ceil`, `round`, `exp2`, `log`, `clamp`, `sign`, `lcm`, `gcd`, `is_even`, `is_odd`, `is_power_of_2`, `popcount`, `clz`, `ctz`, `bswap`, `bitreverse`, `rotate_left`, `rotate_right`, `saturating_add`, `saturating_sub`) are recognized as pure and evaluated at compile time when arguments are constant.
- A `comptime {}` result participates in the downstream constant-folding chain the same way a `const` declaration does — subsequent uses of the variable may also be folded.
- A `var` initialized with `comptime {}` is treated like a `const` for the purpose of compile-time propagation even though it remains technically reassignable.

**When a `comptime` block cannot be folded** (because the body references non-constant values), the compiler emits a hard error at compile time. This makes `comptime` a guarantee, not a hint.

### 6.4 Register Variables

```
register var x:int = 0    // hint to force variable into CPU register
```

Instructs the compiler to promote the variable to an SSA register via LLVM's `mem2reg` pass. Only works for types that fit in a register (integers, floats, bools).

### 6.4 Assignment

```
x = 10
arr[i] = value
obj.field = value
```

### 6.5 Compound Assignment

```
x += 1     x -= 1     x *= 2     x /= 2     x %= 3
x &= mask  x |= flag  x ^= bits  x <<= 2    x >>= 2
x **= 2    x ??= default_val
```

---

## 7. Functions

### 7.1 Basic Syntax

```
fn name(param1, param2) {
    // body
}
```

### 7.2 Type-Annotated Parameters and Return Type

```
fn add(a:int, b:int) -> int {
    return a + b;
}
```

### 7.3 Expression-Body Functions

A function with a single-expression body can use `=` syntax:

```
fn square(x:int) -> int = x * x;
```

### 7.4 Default Parameter Values

Parameter defaults must be literal values:

```
fn greet(name, times = 1) {
    repeat times { println(name); }
}
```

### 7.5 Generic (Type-Parameterized) Functions

```
fn identity<T>(x:T) -> T {
    return x;
}
```

Type parameters are declared in `<...>` after the function name and are mainly informational.

### 7.6 Function Annotations

Annotations appear before `fn` and are prefixed with `@`:

```
@hot
fn compute(x:int) -> int { ... }

@inline @pure
fn add(a:int, b:int) -> int = a + b;
```

| Annotation | Effect |
|---|---|
| `@inline` | Force function inlining |
| `@noinline` | Prevent inlining |
| `@cold` | Mark as cold (infrequently called) |
| `@hot` | Mark as hot (optimize aggressively; elides bounds checks at O2+) |
| `@pure` | Mark as pure (no side effects); enables cross-function constant propagation |
| `@noreturn` | Function never returns |
| `@static` | Internal linkage |
| `@flatten` | Inline all call sites within this function |
| `@unroll` | Unroll loops within this function |
| `@nounroll` | Prevent loop unrolling |
| `@restrict` | All pointer params are non-aliasing (`noalias`) |
| `@noalias` | Alias for `@restrict` |
| `@vectorize` | Enable loop auto-vectorization |
| `@novectorize` | Disable loop auto-vectorization |
| `@parallel` | Enable loop parallelization hints |
| `@noparallel` | Disable loop parallelization hints |
| `@minsize` | Optimize for code size |
| `@optnone` | Disable all optimizations (overrides `@inline`, `@hot`) |
| `@nounwind` | Function never throws (no unwind tables) |
| `@const_eval` | Evaluate at compile time when possible |
| `@allocator(size=N)` | Function is a memory allocator; adds LLVM `allocsize(N)` + `noalias` on return (see §7.9) |

**File-level annotation:**
```
@noalias   // applied before any fn/struct/enum — sets noalias on all functions in file
```

### 7.7 Parameter Annotation: @prefetch

```
fn process(@prefetch data, size:int) {
    // 'data' will be prefetched into cache before first use
}
```

At function return the compiler automatically emits cache-eviction prefetch hints for any `@prefetch`-annotated parameters that are not being returned. No explicit `invalidate` is required for `@prefetch` parameters.

### 7.8 Tail Calls

Self-recursive calls in tail position are automatically converted to jumps (guaranteed tail call elimination).

### 7.9 Allocator Annotation (`@allocator`)

```
@allocator(size=0)
fn my_alloc(nbytes) {
    return malloc(nbytes);
}

@allocator(size=0, count=1)
fn my_calloc(nmemb, size) {
    return calloc(nmemb, size);
}
```

`@allocator(size=N)` tells the compiler that this function **returns freshly allocated memory** of a size determined by parameter index `N` (zero-indexed). The compiler adds:

- LLVM `allocsize(N)` attribute — informs alias analysis that the returned pointer covers exactly `param[N]` bytes
- `noalias` on the return value — the returned pointer never aliases any existing pointer
- `willreturn` + `nounwind` — the function always returns and never throws

Optional `count=M` specifies that the total allocation size is `param[N] * param[M]` bytes (analogous to `calloc`).

**Effect:** LLVM's `DeadArgumentElimination`, `InlinerPass`, and escape analysis can now reason about the allocation size and provenance, enabling allocation elimination when the result is proven dead.

---

## 8. Control Flow

### 8.1 if / else / elif

```
if (condition) {
    // ...
} elif (other) {
    // ...
} else {
    // ...
}
```

`elif` is a first-class keyword (not `else if`).

### 8.2 unless

Inverted `if` — executes the body when the condition is **false**:

```
unless (x == 0) {
    // runs when x != 0
}
```

Desugars to `if (!condition)`.

### 8.3 Branch Prediction Hints

```
likely if (condition) { ... }
unlikely if (condition) { ... }
```

Attaches branch-weight metadata to the conditional branch.

### 8.4 guard

Early-exit pattern — executes the body when the condition is **false**:

```
guard (x > 0) else {
    return -1;
}
```

Desugars to `if (!condition) { body }`.

### 8.5 switch

```
switch (value) {
    case 1: { ... }
    case 2, 3: { ... }    // multi-value case: matches 2 or 3
    default: { ... }
}
```

Multiple comma-separated values in one `case` arm map to the same body. OmScript has **no C-style fallthrough** — an empty case body simply exits the switch without executing the next case's body. To match multiple values, use comma syntax (`case 2, 3:`).

`break` inside any case arm exits the switch (same as a loop `break`).

### 8.6 when

Pattern-matching style switch with `=>` syntax:

```
when (value) {
    1 => { println("one"); },
    2, 3 => { println("two or three"); },
    _ => { println("other"); }
}
```

Desugars to a switch statement.

### 8.7 defer

Defers execution of a statement to the end of the enclosing block. Multiple `defer`s in the same block execute in **LIFO order** (last `defer` runs first):

```
fn open_file() {
    defer println("second");  // runs second
    defer println("first");   // runs first (LIFO)
    // ... work ...
    // deferred statements execute here, at block exit
}
```

### 8.8 with

Scoped variable bindings lexically scoped to a block. Supports `var` and `const` bindings, and multiple comma-separated bindings in one `with`:

```
with (var f = open_file()) {
    // f is available here
}

with (var a = 5, var b = 10) {
    // both a and b are in scope
}

with (const factor = 7) {
    // factor is a read-only binding
}
```

Desugars to a block with the variable declarations prepended to the body.

---

## 9. Loops

### 9.1 while

```
while (condition) {
    // ...
}
```

### 9.2 do...while

```
do {
    // ...
} while (condition);
```

### 9.3 until

Loop while condition is **false** — desugars to `while (!condition)`:

```
until (x == 0) {
    x -= 1;
}
```

### 9.4 for — Range-Based

```
for (i in 0...10) { ... }          // 0, 1, ..., 9
for (i in 0...10...2) { ... }      // 0, 2, 4, 6, 8 (step 2)
for (i in 0...10 step 2) { ... }   // same as above
```

`...` is the range operator (exclusive end). `..` is also accepted.

### 9.5 for — Downto

```
for (i in 10 downto 0) { ... }         // 10, 9, ..., 1 (step -1)
for (i in 10 downto 0 step 2) { ... }  // 10, 8, 6, 4, 2
```

### 9.6 for — Collection Iteration (for-each)

```
for (item in collection) { ... }
for (i, item in collection) { ... }   // indexed: i = index, item = element
```

When the collection is a **string**, the iterator variable holds the **integer character code** (byte value) of each character, not a single-character string:

```
for (c in "abc") {
    // c = 97, then 98, then 99  (ASCII codes)
}
```

Use `to_char(c)` to convert a character code back to a single-character string.

### 9.7 foreach

```
foreach item in collection { ... }
foreach (i, item in collection) { ... }   // indexed variant
```

Desugars to a `for` loop.

### 9.8 loop

Infinite loop (desugars to `while (true)`):

```
loop {
    // infinite
}
```

Counted loop:

```
loop 5 { ... }      // run body 5 times
loop (n) { ... }    // run body n times
```

### 9.9 repeat

Counted loop:

```
repeat 5 { ... }      // run body 5 times
repeat (n) { ... }    // run body n times
```

Post-test loop (desugars to `do...while`):

```
repeat {
    // ...
} until (condition);
```

### 9.10 forever

Infinite loop:

```
forever {
    // infinite, same as loop { }
}
```

### 9.11 times

Counted loop:

```
times 3 { ... }     // run body 3 times
times (n) { ... }   // run body n times
```

### 9.12 OPTMAX Loop Variables

Inside `OPTMAX` blocks, loop variables require type annotations:

```
for (i:int in 0...n) { ... }
```

### 9.13 break / continue

```
break;
continue;
```

### 9.14 swap

Rotates values among two or more variables atomically:

```
swap a, b;         // exchange a and b
swap a, b, c;      // a←b, b←c, c←a (rotation)
```

### 9.15 `parallel` Loops

Prepend `parallel` to any `for`, `while`, or `foreach` loop to assert that **all iterations are independent** — there are no loop-carried data dependencies. The compiler emits `llvm.loop.parallel_accesses` metadata, allowing LLVM and the auto-vectorizer/parallelizer to treat iterations as fully independent.

```
parallel for (i in 0...n) {
    result[i] = compute(data[i]);
}

parallel foreach item in arr {
    process(item);
}

parallel while (cond) {
    step();
}
```

**Semantics:** You are asserting to the compiler that the loop body does not read memory written by a prior iteration and does not write memory read by a later iteration. Violating this assertion produces **undefined behavior** — the compiler will not verify it.

**Difference from `@loop(vectorize=true)`:** `parallel` makes a stronger claim (no inter-iteration dependency of any kind), while `vectorize=true` only requests vectorization without suppressing alias checks.

### 9.16 Loop Annotations (`@loop(...)`)

The `@loop(...)` annotation is placed **between** the loop header's closing `)` and the loop body's opening `{`. It fine-tunes compiler hints for that specific loop:

```
for (i in 0...n) @loop(unroll=4) { ... }

for (i in 0...n) @loop(vectorize=true, tile=32) { ... }

for (i in 0...n) @loop(independent=true) { a[i] = b[i] + c[i]; }

while (cond) @loop(parallel=true) { step(); }

for (i in 0...n) @loop(fuse=true) { sum1 += a[i]; }
for (i in 0...n) @loop(fuse=true) { sum2 += b[i]; }    // fused with the loop above
```

Supported keys:

| Key | Type | Effect |
|---|---|---|
| `unroll=N` | integer | Unroll the loop N times (emits `llvm.loop.unroll.count`) |
| `vectorize=true/false` | bool | Enable or disable auto-vectorization for this loop |
| `tile=N` | integer | Tile/block the loop body with block size N |
| `parallel=true/false` | bool | Same as the `parallel` keyword |
| `independent=true` | bool | Emit `llvm.access.group` on all memory ops + `parallel_accesses` metadata — stronger than `parallel`, suppresses all cross-iteration alias conservatism |
| `fuse=true` | bool | Merge this loop with the immediately following `@loop(fuse=true)` loop of the same range into a single loop body |

#### `@loop(independent=true)` in depth

When `independent=true`, the compiler:
1. Creates a unique `llvm.access.group` metadata node for this loop.
2. Attaches `!llvm.access.group` to every load and store in the loop body.
3. Adds a `{!"llvm.loop.parallel_accesses", <access-group>}` entry to the loop's backedge metadata.

This tells LLVM's vectorizer and loop-carried-dependency analyzer that **no memory operation in this loop aliases any other memory operation across iterations**. It is a stronger hint than LLVM's normal alias analysis and enables vectorization of loops that would otherwise be blocked by conservative aliasing.

Only use `independent=true` when you are certain there are no cross-iteration aliases. Incorrect use is **undefined behavior**.

#### `@loop(fuse=true)` in depth

Two adjacent `for` loops annotated with `@loop(fuse=true)` that iterate over the **same range** (identical `start` and `end` expressions as integer literals or the same identifier) are merged by the AST-level loop fusion pre-pass into a single loop:

```
// Before fusion:
for (i in 0...n) @loop(fuse=true) { a[i] = b[i] * 2; }
for (i in 0...n) @loop(fuse=true) { c[i] = a[i] + 1; }

// After fusion (logically equivalent):
for (i in 0...n) {
    a[i] = b[i] * 2;
    c[i] = a[i] + 1;
}
```

**Benefits:** Eliminates loop overhead (branch, counter update) for the second loop; improves cache reuse by processing both bodies for the same `i` in the same iteration; enables vectorization across the fused body.

**Constraints:**
- Both loops must be `for (i in start...end)` range loops with equal bounds.
- The second iterator variable is mapped to the first loop's iterator inside the fused body.
- At most one pair of adjacent loops is fused per pass (not recursive/chained). Apply `@loop(fuse=true)` to all loops in a chain to fuse all of them.
- Fusion is skipped if the bounds expressions are not syntactically identical.

---

## 10. Expressions and Operators

### 10.1 Arithmetic

```
a + b    a - b    a * b    a / b    a % b
a ** b             // exponentiation (a to the power of b)
-a                 // unary negation
```

### 10.2 Comparison

```
a == b    a != b    a < b    a <= b    a > b    a >= b
```

### 10.3 Logical

```
a && b    a || b    !a
```

### 10.4 Bitwise

```
a & b     // AND
a | b     // OR
a ^ b     // XOR
~a        // NOT (bitwise complement)
a << n    // left shift
a >> n    // right shift
```

### 10.5 Null Coalescing

```
a ?? b    // if a != null/0, yields a; otherwise yields b
x ??= default_val  // compound null-coalescing assignment
```

### 10.6 Ternary

```
condition ? then_value : else_value
```

### 10.7 Pipe Forward

```
value |> function_name    // equivalent to function_name(value)
[1,2,3] |> sort |> reverse
```

### 10.8 Range

```
0...10      // exclusive range from 0 to 9
0..10       // also supported
```

### 10.9 Spread

```
...array    // spread array elements into a call or array literal
```

### 10.10 Increment / Decrement

```
x++    x--    ++x    --x
```

### 10.11 Address / Reference

```
&variable   // take address / borrow reference
```

### 10.12 Operator Precedence (high to low)

1. Postfix `++`, `--`, `[]`, `.`, function call
2. Prefix `++`, `--`, `-`, `!`, `~`, `&`
3. `**` (right-associative)
4. `*`, `/`, `%`
5. `+`, `-`
6. `<<`, `>>`
7. `&`
8. `^`
9. `|`
10. `==`, `!=`, `<`, `<=`, `>`, `>=`
11. `&&`
12. `||`
13. `??`
14. `? :` (ternary)
15. Assignment (`=`, `+=`, etc.)
16. `|>` (pipe forward)

---

## 11. Arrays

### 11.1 Array Literals

```
var arr = [1, 2, 3]
var empty = []
var typed:int[] = [10, 20, 30]
```

### 11.2 Indexing

```
arr[0]          // read (bounds-checked at O0/O1)
arr[i] = val    // write
s[i]            // string subscript: returns the integer character code at index i
```

Bounds checks are elided in `OPTMAX` functions, `@hot` functions at O2+, and when the compiler can statically prove safety.

### 11.3 Array Built-ins

| Function | Description |
|---|---|
| `len(arr)` | Number of elements |
| `push(arr, val)` | Append element |
| `pop(arr)` | Remove and return last element |
| `sort(arr)` | Sort in place |
| `reverse(arr)` | Reverse in place |
| `index_of(arr, val)` | First index of value (-1 if not found) |
| `array_contains(arr, val)` | Returns 1 if found, 0 otherwise |
| `array_fill(size, val)` | Create array of `size` copies of `val` |
| `array_concat(a, b)` | Concatenate two arrays |
| `array_slice(arr, start, end)` | Sub-array `[start, end)` |
| `array_copy(arr)` | Shallow copy |
| `array_remove(arr, i)` | Remove element at index `i` |
| `array_map(arr, fn)` | Apply function to each element, return new array |
| `array_filter(arr, fn)` | Keep elements for which `fn` returns true |
| `array_reduce(arr, fn, init)` | Reduce array to single value |
| `array_min(arr)` | Minimum value |
| `array_max(arr)` | Maximum value |
| `array_any(arr, fn)` | True if any element satisfies `fn` |
| `array_every(arr, fn)` | True if all elements satisfy `fn` |
| `array_find(arr, fn)` | First element satisfying `fn` |
| `array_count(arr, fn)` | Count elements satisfying `fn` |
| `array_product(arr)` | Product of all elements |
| `array_last(arr)` | Last element |
| `array_insert(arr, i, val)` | Insert `val` at index `i` |
| `swap(arr, i, j)` | Swap elements at indices `i` and `j` in place (bounds-checked) |
| `sum(arr)` | Sum of all elements |
| `range(start, end)` | Create array `[start, start+1, ..., end-1]` |
| `range_step(start, end, step)` | Create array with step |

---

## 12. Strings

Strings are heap-allocated and NUL-terminated. String indexing and concatenation are built-in.

### 12.1 String Built-ins

| Function | Description |
|---|---|
| `len(s)` / `str_len(s)` | Length in characters |
| `char_at(s, i)` | Character code at index `i` (returns an **integer**, the byte value; use `to_char()` to get a string) |
| `str_eq(s1, s2)` | String equality (returns 1/0) |
| `str_concat(s1, s2)` | Concatenate two strings |
| `str_substr(s, start, len)` | Substring of length `len` starting at `start` |
| `str_upper(s)` | Uppercase |
| `str_lower(s)` | Lowercase |
| `str_find(s, c)` | Find first occurrence of character code `c` (integer) in `s`; returns index or -1 |
| `str_contains(s, sub)` | Contains substring (1/0) |
| `str_index_of(s, sub)` | First index of substring `sub` (-1 if not found) |
| `str_replace(s, old, new)` | Replace all occurrences of `old` with `new` |
| `str_trim(s)` | Strip leading/trailing whitespace |
| `str_starts_with(s, prefix)` | Starts with prefix (1/0) |
| `str_ends_with(s, suffix)` | Ends with suffix (1/0) |
| `str_repeat(s, n)` | Repeat string `n` times |
| `str_reverse(s)` | Reverse string |
| `str_split(s, delim)` | Split by delimiter, returns array |
| `str_chars(s)` | Split into array of single-character strings |
| `str_join(arr, delim)` | Join array with delimiter |
| `str_count(s, sub)` | Count non-overlapping occurrences of `sub` |
| `str_pad_left(s, n, ch)` | Pad string on left to width `n` with character `ch` |
| `str_pad_right(s, n, ch)` | Pad string on right to width `n` with character `ch` |
| `to_string(x)` | Convert number to string |
| `number_to_string(x)` | Convert number to string |
| `string_to_number(s)` | Parse string as number |
| `str_to_int(s)` | Parse string as integer |
| `str_to_float(s)` | Parse string as float |
| `to_int(x)` | Convert to integer |
| `to_float(x)` | Convert to float |
| `to_char(code)` | Integer code point to single-character string |
| `char_code(c)` | Character to integer code point |
| `is_alpha(c)` | Is alphabetic (1/0) |
| `is_digit(c)` | Is decimal digit (1/0) |

---

## 13. Dictionaries / Maps

Dictionaries (maps) are hash maps mapping string keys to integer/pointer values.

### 13.1 Map Built-ins

| Function | Description |
|---|---|
| `map_new()` | Create new empty map |
| `map_set(m, key, val)` | Set key to value |
| `map_get(m, key, default)` | Get value for key; returns `default` if key is absent |
| `map_has(m, key)` | Check if key exists (1/0) |
| `map_remove(m, key)` | Remove key |
| `map_keys(m)` | Array of all keys |
| `map_values(m)` | Array of all values |
| `map_size(m)` | Number of entries |

---

## 14. Structs

### 14.1 Definition

```
struct Point {
    x: int,
    y: int
}
```

### 14.2 Field Attributes

Fields can carry optional attributes:

```
struct Buffer {
    hot data: int,           // frequently accessed field
    cold metadata: int,      // infrequently accessed
    noalias ptr: int,        // pointer does not alias other fields
    immut size: int,         // immutable after construction
    align(16) data: float,   // alignment requirement
    range(0, 100) percent: int  // value range hint
}
```

### 14.3 Struct Literals

```
var p = Point { x: 10, y: 20 }
```

### 14.4 Field Access and Assignment

```
var v = p.x
p.y = 30
p.x += 5    // compound assignment on struct fields supported
```

### 14.5 Operator Overloading

```
struct Vec2 {
    x: float,
    y: float,
    fn operator+(other: Vec2) -> Vec2 {
        return Vec2 { x: self.x + other.x, y: self.y + other.y };
    }
    fn operator==(other: Vec2) -> bool {
        return self.x == other.x && self.y == other.y;
    }
}
```

Supported operators: `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`.

Operator overload functions are automatically inlined.

---

## 15. Enums

```
enum Color {
    Red,
    Green,
    Blue,
    Custom = 42
}

var c = Color.Red
```

Enum members without explicit values are auto-numbered from 0 (or from the previous explicit value + 1).

---

## 16. Error Handling

### 16.1 try / catch / throw

```
try {
    // ...
    throw "something went wrong";
} catch (e) {
    println(e);
}
```

`throw` accepts any expression. `catch (e)` binds the thrown value to `e`.

### 16.2 assert

```
assert(x > 0);             // runtime assertion; aborts on failure
```

---

## 17. Memory Semantics and Ownership System

OmScript has a **compile-time ownership system** that statically tracks how every variable's value is used. The ownership system serves two purposes:

1. **Safety** — detecting use-after-move and use-after-invalidate at compile time.
2. **Aliasing guarantee** — the compiler proves that no two live references can point to the same memory, enabling `noalias`, `nonnull`, `dereferenceable`, and `nocapture` LLVM attributes on all pointer parameters automatically.

### 17.0 Ownership States

Every variable that participates in ownership annotations transitions through an ownership lattice:

| State | Meaning |
|---|---|
| **Owned** | Full control — may read, write, move, borrow, or invalidate |
| **Borrowed** | Shared read-only alias — source may NOT be mutated, moved, or invalidated while any borrow is live; multiple immutable borrows may coexist |
| **MutBorrowed** | Exclusive mutable alias — exactly one `borrow mut` is active; source may NOT be read, written, moved, or invalidated; no other borrow may coexist |
| **Frozen** | Value is guaranteed non-poison; source is read-only for its remaining lifetime |
| **Moved** | Ownership transferred out — any further use is a compile error |
| **Invalidated** | Explicitly killed — any further use is a compile error |

Variables that never use `move`, `borrow`, `freeze`, or `invalidate` remain in the Owned state and behave like ordinary variables.

### 17.1 move

```
var a = [1, 2, 3]
var b = move a     // b owns the array; a transitions to Moved state
```

After `move`, the source variable (`a`) is **dead** — any subsequent read or write to it is a **compile-time error**. The compiler emits `llvm.lifetime.end` on the source alloca so the register allocator and optimizer can freely reuse its storage.

`move` can also appear in expression position:

```
fn transfer(data) {
    store(move data)   // data is moved into the call; data is dead afterwards
}
```

### 17.2 borrow

`borrow` uses a **statement** syntax (not an expression), distinct from ordinary variable declarations:

```
borrow view = data;           // immutable borrow — 'view' is a read-only alias of 'data'
borrow mut ref = data;        // mutable borrow — 'ref' is an exclusive mutable alias of 'data'
borrow typed_ref:int = data;  // typed immutable borrow
```

`borrow` creates a **read-only alias**. The source variable transitions to the Borrowed state: it may still be read but cannot be mutated, moved, or invalidated while the borrow is active. The borrow ends at the end of the enclosing scope. Multiple simultaneous immutable borrows are allowed.

**Mutable borrow (`borrow mut`):**

```
var buf = [0, 0, 0];
borrow mut ref = buf;          // exclusive mutable alias
// ref[0] = 42;               // writing through a mutable borrow is valid
// buf[0] = 1;                // ERROR: 'buf' is MutBorrowed; direct mutation forbidden
```

`borrow mut` creates an **exclusive mutable alias**. The source transitions to the MutBorrowed state — no reads or writes to the source, and no new borrows, are allowed while the mutable borrow is live.

The compiler attaches `!alias.scope` and `!noalias` LLVM scoped-noalias metadata to loads through borrowed pointers, enabling alias-analysis-dependent optimizations (vectorization, LICM, load/store reordering) across borrow boundaries.

### 17.3 invalidate

```
invalidate a;      // explicitly marks variable 'a' as Invalidated
```

After `invalidate`, any use of `a` is a **compile-time error**.

Required before returning from a function in which a variable was declared with the `prefetch` statement — the compiler enforces that every prefetch-declared variable is either invalidated or returned before the function exits. This does **not** apply to `@prefetch`-annotated function parameters (those are handled automatically).

### 17.4 freeze

```
freeze x;
```

`freeze x` does two things:

1. **LLVM IR level** — emits an `llvm freeze` instruction on the current value of `x` and stores it back. This converts any possible **poison** or **undef** bits in `x` to a deterministic (but arbitrary) concrete value. After a `freeze`, `x` is guaranteed to have a well-defined value even if it was previously uninitialized or poison due to speculative execution.

2. **Ownership level** — marks `x` as Frozen. A Frozen variable is **read-only** for the rest of its lifetime — it may not be mutated, moved, or invalidated after freezing.

**Alias propagation:** When `freeze x` is called:
- If `x` is a borrow alias of `y` (i.e., `borrow x = y`), then `y` is also marked Frozen.
- If `y` is already Frozen and `borrow x = y` is created, then `x` inherits the Frozen state.

This bidirectional propagation ensures that freeze semantics are consistent across all aliases of the same underlying storage.

```
var x = compute();
freeze x;
// x is now guaranteed non-poison, read-only
// using x as a constant-like value is safe

borrow ref = x;
// ref inherits Frozen state — ref is also read-only
```

**When to use `freeze`:**
- After reads from external / FFI sources where the value might be uninitialized
- Before using a value in a context that would produce undefined behavior if the value is poison (e.g., as a shift amount, divisor, or branch condition)
- To communicate to the optimizer that a value is stable and can be freely reordered or hoisted

### 17.5 reborrow

`reborrow` creates a new borrow reference from an existing variable, using `&` to take the address:

```
var x = 100;
borrow r = x;          // borrow (no & needed)
reborrow r2 = &x;      // reborrow — & is required
reborrow mut r3 = &x;  // mutable reborrow
```

**Partial borrow — array element:**
```
var data = [10, 20, 30];
reborrow elem = &data[1];       // borrows only element at index 1
reborrow mut elem = &data[1];   // mutable borrow of element 1
```

**Partial borrow — struct field:**
```
struct Point { x, y }
var p = Point{x: 3, y: 4};
reborrow xref = &p.x;           // borrows only the 'x' field
reborrow mut yref = &p.y;       // mutable borrow of 'y' field
```

**Semantics:**
- A `reborrow` inherits its lifetime from the enclosing scope — when the scope ends, the reborrow is released and the original variable becomes accessible again.
- A mutable `reborrow mut` of a sub-element is distinct from a mutable borrow of the whole — you may have a `reborrow mut` of `p.x` and a `reborrow mut` of `p.y` simultaneously because they refer to disjoint memory.
- The reborrow is registered in the compiler's borrow map so the borrow checker tracks it correctly.
- Accessing the source variable directly while a `reborrow mut` sub-element is live is permitted for fields/elements not covered by the reborrow.

### 17.6 prefetch

The `prefetch` statement issues software prefetch instructions and optionally declares a variable:

```
prefetch arr;              // prefetch 'arr' into L1 cache
prefetch+128 arr;          // prefetch 'arr' and 128 bytes ahead
prefetch data:int = compute();    // declare 'data' and prefetch it
```

Variables declared with the `prefetch` statement must be `invalidate`d before the function returns (unless they are returned via `move`).

### 17.7 No-Aliasing Guarantee

OmScript's ownership system provides a **language-level no-aliasing guarantee**: at any point in a function, no two live pointer-typed variables can refer to the same memory region. This guarantee is enforced by the borrow checker — the Borrowed state prevents the source from being concurrently mutated, and the Moved/Invalidated states prevent any use of the old reference after transfer.

As a result of this invariant, the compiler **automatically** adds the following LLVM attributes to every pointer parameter of every user function (at all optimization levels, including O0):

| LLVM Attribute | Meaning |
|---|---|
| `noalias` | This pointer does not alias any other pointer parameter |
| `nonnull` | This pointer is never null |
| `dereferenceable(8)` | At least 8 bytes are valid at this address |
| `nocapture` | This pointer is never stored into global state or returned as a pointer |

These attributes are applied universally — not just in `@restrict` or `@optmax` functions — because they follow from the language semantics, not from programmer hints. This enables LLVM's alias analysis, CSE, LICM, vectorization, and dead-store elimination passes to apply their most aggressive optimizations throughout the entire program.

For the strongest possible alias annotations, use:

```
@restrict         // synonym: @noalias
fn compute(a:int[], b:int[], n:int) -> int { ... }
```

or apply `@noalias` at the file level to cover all functions in the file:

```
@noalias
// ... all functions below get explicit restrict on all params
```

---

## 18. OPTMAX Blocks

`OPTMAX` blocks mark regions for maximum optimization. Inside these blocks:
- All function parameters **must** have type annotations
- All `var` declarations **must** have type annotations
- All `for` loop variables **must** have type annotations
- Bounds checks on array/string accesses are **elided**
- The compiler applies the most aggressive optimization strategy

```
OPTMAX=:
fn fast_sum(arr:int[], n:int) -> int {
    var total:int = 0;
    for (i:int in 0...n) {
        total += arr[i];
    }
    return total;
}
OPTMAX!:
```

`OPTMAX=:` opens the block; `OPTMAX!:` closes it. Nesting is not allowed.

---

## 19. Built-in Functions

### 19.1 I/O

| Function | Description |
|---|---|
| `print(...)` | Print values without newline |
| `println(...)` | Print values with newline |
| `write(s)` | Write string to stdout |
| `print_char(c)` | Print a single character code |
| `input()` | Read a word from stdin |
| `input_line()` | Read a full line from stdin |
| `exit(code)` / `exit_program(code)` | Exit with given code |

### 19.2 Math

| Function | Description |
|---|---|
| `abs(x)` | Absolute value |
| `pow(x, y)` | x to the power y |
| `sqrt(x)` | Square root |
| `cbrt(x)` | Cube root |
| `exp(x)` | e^x |
| `exp2(x)` | 2^x |
| `log(x)` | Natural logarithm |
| `log2(x)` | Base-2 logarithm |
| `log10(x)` | Base-10 logarithm |
| `floor(x)` | Floor |
| `ceil(x)` | Ceiling |
| `round(x)` | Round to nearest integer |
| `sin(x)` | Sine |
| `cos(x)` | Cosine |
| `tan(x)` | Tangent |
| `asin(x)` | Arc sine |
| `acos(x)` | Arc cosine |
| `atan(x)` | Arc tangent |
| `atan2(y, x)` | Two-argument arc tangent |
| `hypot(x, y)` | sqrt(x² + y²) |
| `min(a, b)` | Minimum of two values |
| `max(a, b)` | Maximum of two values |
| `min_float(a, b)` | Floating-point minimum (NaN-aware) |
| `max_float(a, b)` | Floating-point maximum (NaN-aware) |
| `sign(x)` | Sign: -1, 0, or 1 |
| `clamp(x, lo, hi)` | Clamp x to [lo, hi] |
| `gcd(a, b)` | Greatest common divisor |
| `lcm(a, b)` | Least common multiple |
| `is_even(x)` | 1 if even, 0 otherwise |
| `is_odd(x)` | 1 if odd, 0 otherwise |
| `is_power_of_2(x)` | 1 if x is a power of 2 |
| `fma(a, b, c)` | Fused multiply-add: a*b+c |
| `copysign(x, y)` | Magnitude of x with sign of y |
| `random()` | Random float in [0.0, 1.0) |

### 19.3 Arithmetic with Explicit Overflow/Precision Mode

| Function | Description |
|---|---|
| `fast_add(a, b)` | Addition with `nsw` (no signed wrap) flag |
| `fast_sub(a, b)` | Subtraction with `nsw` flag |
| `fast_mul(a, b)` | Multiplication with `nsw` flag |
| `fast_div(a, b)` | Division (exact, no remainder) |
| `precise_add(a, b)` | Addition without unsafe flags |
| `precise_sub(a, b)` | Subtraction without unsafe flags |
| `precise_mul(a, b)` | Multiplication without unsafe flags |
| `precise_div(a, b)` | Division without unsafe flags |
| `saturating_add(a, b)` | LLVM saturating addition |
| `saturating_sub(a, b)` | LLVM saturating subtraction |

### 19.4 Bit Manipulation

| Function | Description |
|---|---|
| `popcount(x)` | Count set bits |
| `clz(x)` | Count leading zeros |
| `ctz(x)` | Count trailing zeros |
| `bitreverse(x)` | Reverse bit order |
| `bswap(x)` | Byte-swap (endianness swap) |
| `rotate_left(x, n)` | Rotate bits left by n |
| `rotate_right(x, n)` | Rotate bits right by n |

### 19.5 Type Utilities

| Function | Description |
|---|---|
| `typeof(x)` | Returns an integer type tag: 1 = integer, 2 = float, 3 = string |
| `len(x)` | Length of array or string |
| `to_int(x)` | Convert to integer |
| `to_float(x)` | Convert to float |
| `to_string(x)` | Convert number to string |
| `assert(cond)` | Runtime assertion (aborts on failure) |

### 19.6 Time / System

| Function | Description |
|---|---|
| `time()` | Current Unix timestamp in seconds (integer) |
| `sleep(ms)` | Sleep for `ms` milliseconds |

### 19.7 Optimizer Hints

| Function | Description |
|---|---|
| `assume(cond)` | Assert to optimizer that `cond` is always true (LLVM `llvm.assume`) |
| `unreachable()` | Mark code as unreachable (UB if reached) |
| `expect(val, expected)` | Branch prediction hint: `val` is likely `expected` |

---

## 20. Concurrency

OmScript provides low-level threading and mutex primitives.

### 20.1 Thread Functions

| Function | Description |
|---|---|
| `thread_create("func_name")` | Create a new thread running the named zero-argument function; returns a thread handle |
| `thread_join(t)` | Wait for thread `t` to finish |

### 20.2 Mutex Functions

| Function | Description |
|---|---|
| `mutex_new()` | Create a new mutex |
| `mutex_lock(m)` | Lock mutex `m` |
| `mutex_unlock(m)` | Unlock mutex `m` |
| `mutex_destroy(m)` | Destroy mutex `m` |

---

## 21. File I/O

| Function | Description |
|---|---|
| `file_read(path)` | Read entire file as string |
| `file_write(path, content)` | Write string to file (overwrite) |
| `file_append(path, content)` | Append string to file |
| `file_exists(path)` | Returns 1 if file exists, 0 otherwise |

---

## 22. Lambda Expressions

Lambdas create anonymous functions. They are desugared to named functions at parse time.

```
|x| x * 2                     // single parameter
|x, y| x + y                  // two parameters
|| 42                          // no parameters
|x:int| x * 2                 // annotated parameter
```

Lambdas are first-class values and can be passed to higher-order functions:

```
var doubled = array_map([1, 2, 3], |x| x * 2)
var evens = array_filter([1, 2, 3, 4], |x| x % 2 == 0)
var total = array_reduce([1, 2, 3, 4], |acc, x| acc + x, 0)
```

---

## 23. Import System

```
import "filename";
import "path/to/module";   // .om extension added automatically
```

- Circular imports are detected and silently skipped
- Paths are resolved relative to the importing file's directory
- Imports merge all functions, structs, and enums from the imported file

---

## 24. Compiler CLI Reference

### 24.1 Commands

```
omsc <file.om>                  # compile to executable (default: a.out)
omsc compile <file.om>          # same as above
omsc build <file.om>            # same as above
omsc run <file.om>              # compile and run
omsc check <file.om>            # validate syntax only
omsc emit-ir <file.om>          # print LLVM IR
omsc --emit-obj <file.om>       # emit object file only
omsc clean                      # remove build artifacts
omsc pkg <subcommand>           # package manager
```

### 24.2 Output

```
-o <file>      Output file path (default: a.out)
```

### 24.3 Optimization Levels

```
-O0            No optimization
-O1            Basic optimizations
-O2            Standard optimizations (default)
-O3            Aggressive optimizations
-Ofast         -O3 + fast-math
```

### 24.4 Target

```
-march=<cpu>   Target CPU (default: native)
-mtune=<cpu>   Tuning CPU (default: same as -march)
```

Examples: `-march=native`, `-march=znver3`, `-march=skylake`

### 24.5 Feature Flags

All `-f` flags have a `-fno-` counterpart to disable:

| Flag | Default | Description |
|---|---|---|
| `-flto` | off | Link-time optimization |
| `-fpic` | on | Position-independent code |
| `-ffast-math` | off | Fast floating-point (imprecise) |
| `-foptmax` | on | Enable OPTMAX block processing |
| `-fstack-protector` | off | Stack smashing protection |
| `-fvectorize` | on | Auto-vectorization |
| `-funroll-loops` | on | Loop unrolling |
| `-floop-optimize` | on | Loop optimization passes |
| `-fparallelize` | on | Parallelization hints |
| `-fegraph` | on | E-graph equality saturation (active at O2+) |
| `-fsuperopt` | on | Superoptimizer pass (active at O2+) |
| `-fhgoe` | on | Hardware Graph Optimization Engine (active at O2+ with -march/-mtune) |
| `-fescape-analysis` | on | Stack-allocate non-escaping small arrays (O1+) |

### 24.6 Superoptimizer Level

```
-fsuperopt-level=0   Disabled
-fsuperopt-level=1   Basic idiom recognition
-fsuperopt-level=2   Default (idiom + algebraic simplification)
-fsuperopt-level=3   Full (+ enumerative synthesis)
```

### 24.7 Debugging and Info

```
-g / --debug   Emit debug information
-V / --verbose Verbose compiler output (also prints OptStats counters)
-static        Link statically
-s / --strip   Strip debug symbols from output
```

**Verbose output (`-v` / `--verbose`):** In addition to standard compiler progress, prints an **opt-report** summary after compilation showing how many of each optimization class were applied:

```
[opt-report] Optimization statistics:
  const-folded expressions : 127
  calls inlined            :  22
  stack allocs (escape)    :   9
  loops fused              :   4
  borrows frozen           :   3
  independent loops        :   7
  allocator wrappers       :   2
```

### 24.8 Package Manager

```
omsc pkg install <package>   # Install a package
omsc pkg remove <package>    # Remove a package
omsc pkg list                # List installed packages
omsc pkg search <query>      # Search the registry
omsc pkg info <package>      # Show package details
```

---

## 25. Advanced Optimization Features

### 25.1 E-Graph Equality Saturation

The e-graph pass applies algebraic identities and constant folding to LLVM IR before the standard optimization pipeline. It uses union-find with path compression and cost-based extraction to find the lowest-cost equivalent expression.

Enabled by default at O2+; disable with `-fno-egraph`.

### 25.2 Superoptimizer

A post-LLVM optimization pass that applies:
- **Idiom recognition**: popcount, bswap, rotate, min/max, abs, sdiv/srem by power-of-2, etc.
- **Algebraic simplification**: 300+ algebraic identities on LLVM IR instructions
- **Branch-to-select**: converts simple diamond CFGs (no side-effects in either branch, single-block arms) to `select` instructions — eliminates branch misprediction penalty on modern CPUs
- **Enumerative synthesis** (level 3 only): enumerates short instruction sequences to find cheaper equivalents

Enabled by default at O2+; configure with `-fsuperopt-level=<level>`.

#### Branch-to-Select in Depth

```omscript
// Source
var y = 0;
if (x > 5) { y = x * 2; } else { y = x + 1; }

// IR before superoptimizer:
//   br i1 %cond, label %then, label %else
//   then: %a = mul i64 %x, 2 → br %merge
//   else: %b = add i64 %x, 1 → br %merge
//   merge: %y = phi [%a, %then], [%b, %else]

// IR after branch-to-select:
//   %a = mul i64 %x, 2
//   %b = add i64 %x, 1
//   %y = select i1 %cond, i64 %a, i64 %b
```

The transformation fires when both arms have no side effects and contain at most one instruction each (configurable). This produces a `CMOV` on x86 or a conditional select on ARM, avoiding branch prediction entirely.

### 25.3 Hardware Graph Optimization Engine (HGOE)

Activated at O2+ when `-march` or `-mtune` is provided (including `native`). When neither flag is set the pass is a complete no-op. Builds a structural model of the target CPU microarchitecture and:
- Maps operations to hardware execution units
- Inserts FMA (fused multiply-add) instructions where profitable
- Applies hardware-specific strength reductions (`imul` → shift+add for constant multipliers)
- Performs cycle-accurate instruction scheduling using real port models
- Sets `target-cpu` and `target-features` on every function for proper ISA selection

### 25.4 OPTMAX Blocks

See [Section 18](#18-optmax-blocks).

### 25.5 Profile Guidance (PGO)

The build system includes `benchmark_pgo.sh` for profile-guided optimization runs. PGO is coordinated externally via the build scripts — there is no language-level PGO syntax.

### 25.6 Escape Analysis (Stack Allocation)

The compiler performs **escape analysis** on array allocations at O1+ to determine whether an array can be stack-allocated instead of heap-allocated.

**An array is stack-allocated when all of the following hold:**
1. It is an array literal with ≤ 16 integer-constant elements.
2. The array is never passed to a function that stores it into a non-local location.
3. The array is never returned from its declaring function.
4. The array is never assigned to a non-local variable or struct field.
5. The array is never captured by a lambda expression.

When escape analysis succeeds, the compiler replaces the `malloc` + header init with a fixed-size `alloca`. This eliminates both the malloc call and the corresponding free, with no observable semantic difference. The stack memory is automatically reclaimed at function exit.

```omscript
fn sum_small() -> int {
    var a = [1, 2, 3, 4, 5]   // stack-allocated: 5 int literals, non-escaping
    var s = 0
    for (x in a) { s += x; }
    return s;
    // no free needed: 'a' is on the stack
}
```

The OptStats counter `Stack allocs (escape)` reports how many arrays were successfully stack-allocated.

**Disable:** `-fno-escape-analysis`

### 25.7 Bounds Check Hoisting

At O2+ inside `@hot`-annotated functions and OPTMAX blocks, redundant bounds checks within loops are **hoisted** out of the loop body. If the compiler can prove (via `llvm.assume` or range metadata) that the loop index `i` stays within `[0, len(arr))` for the entire loop duration, the per-iteration check is replaced with a single pre-loop check:

```omscript
@hot
fn sum(arr, n) {
    var s = 0;
    for (i:int in 0...n) {
        s += arr[i];   // check hoisted: verified once before the loop
    }
    return s;
}
```

The hoisted check emits an `llvm.assume(n <= len(arr))` before the loop header, enabling the vectorizer and unroller to eliminate all in-loop guards.

### 25.8 Allocation Elimination

When a heap allocation's result is provably never used (dead allocation), or when the allocation is immediately followed by a `free` / goes out of scope without any read, the compiler's dead-code elimination pass (in combination with LLVM's `DeadArgumentElimination` and `GlobalDCE`) eliminates the malloc/free pair entirely.

This is strengthened by the `@allocator(size=N)` annotation (§7.9), which marks user-defined allocation wrappers so that LLVM can reason about their return value's provenance and liveness.

```omscript
fn example() {
    var tmp = [0, 0, 0]   // allocation — but tmp is never read
    return 42;             // tmp's allocation is eliminated entirely
}
```

### 25.9 SROA (Struct/Array → Scalars)

LLVM's **Scalar Replacement of Aggregates** (SROA) pass is applied at O1+ and promotes struct fields and small fixed-size array elements to individual SSA scalar values when:
- The aggregate is declared as a local variable (alloca).
- All accesses use constant GEP indices (i.e., field accesses and constant-index array subscripts).
- The aggregate address is never taken and stored into a non-local.

OmScript assists SROA by:
- Setting **high alignment** on all struct and array allocas (`align 8`) to maximize promotion likelihood.
- Ensuring that `@loop(independent=true)` loops with fixed-size arrays use constant-index accesses where possible, enabling SROA to promote individual elements to registers.

After SROA, individual elements are accessed as register values (SSA `load`/`store` pairs become direct `phi` nodes), and mem2reg promotes them fully to SSA form, eliminating all memory traffic for the aggregate.

### 25.10 Reduction Recognition

The compiler recognizes **loop-carried reduction** patterns — accumulators updated in every iteration using an associative, commutative operator — and annotates the loop with `llvm.loop.vectorize.enable` to allow LLVM to generate horizontal-reduction SIMD instructions:

Recognized patterns:
- `sum += arr[i]` → horizontal add reduction
- `prod *= arr[i]` → horizontal multiply reduction
- `mx = max(mx, arr[i])` → horizontal max reduction
- `mn = min(mn, arr[i])` → horizontal min reduction
- `flags |= arr[i]` / `bits &= arr[i]` → bitwise OR/AND reductions

When combined with `@loop(vectorize=true)` or inside an OPTMAX block, the vectorizer emits SIMD reduction instructions (e.g., `vphaddd`, `vphaddw`, `vpmaxsd` on AVX2) that process multiple elements per clock cycle.

### 25.11 OptStats

When the compiler is run with `-v` / `--verbose`, it prints an **opt-report** summary after compilation listing all optimization counters:

| Counter | What it counts |
|---|---|
| `const-folded expressions` | Total AST-level arithmetic, string, and builtin folds |
| `calls inlined` | Call sites replaced with inlined constants via cross-function propagation |
| `stack allocs (escape)` | Arrays stack-allocated by escape analysis |
| `loops fused` | Pairs of loops merged by `@loop(fuse=true)` |
| `borrows frozen` | Variables frozen by `freeze` + their propagated borrow aliases |
| `independent loops` | Loops annotated with `@loop(independent=true)` |
| `allocator wrappers` | User functions annotated with `@allocator` |

Example output:
```
[opt-report] Optimization statistics:
  const-folded expressions : 127
  calls inlined            :  22
  stack allocs (escape)    :   9
  loops fused              :   4
  borrows frozen           :   3
  independent loops        :   7
  allocator wrappers       :   2
```

