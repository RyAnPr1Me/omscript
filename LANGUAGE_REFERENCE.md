# OmScript Language Reference

> **Version:** 4.1.1
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
    - 5.1 Type Annotations
    - 5.2 Scalar Types
    - 5.3 Array Types
    - 5.4 Dictionary Type
    - 5.5 SIMD Vector Types
    - 5.6 Reference Types
    - 5.7 Integer Type-Cast Syntax
6. [Variables and Constants](#6-variables-and-constants)
7. [Functions](#7-functions)
    - 7.1 Basic Syntax
    - 7.2 Type-Annotated Parameters and Return Type
    - 7.3 Expression-Body Functions
    - 7.4 Default Parameter Values
    - 7.5 Generic (Type-Parameterized) Functions
    - 7.6 Function Annotations
    - 7.7 Parameter Annotation: @prefetch
    - 7.8 Tail Calls
    - 7.9 Allocator Annotation (@allocator)
    - 7.10 Method Call Syntax
8. [Control Flow](#8-control-flow)
9. [Loops](#9-loops)
10. [Expressions and Operators](#10-expressions-and-operators)
    - 10.1 Arithmetic
    - 10.2 Comparison (including chained comparisons)
    - 10.3 Logical
    - 10.4 Bitwise
    - 10.5 Null Coalescing (`??`)
    - 10.5.1 Elvis Operator (`?:`)
    - 10.5.2 `in` Operator
    - 10.6 Ternary
    - 10.7 Pipe Forward
    - 10.8 Range
    - 10.9 Spread
    - 10.10 Increment / Decrement
    - 10.11 Address / Reference
    - 10.12 Operator Precedence
    - 10.13 Type-Namespace Method Dispatch
    - 10.14 Integer Type-Cast Dispatch
11. [Arrays](#11-arrays)
12. [Strings](#12-strings)
13. [Dictionaries / Maps](#13-dictionaries--maps)
    - 13.0 Dict Literals
    - 13.1 Map Built-ins
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
    - 18.1 `@optmax(...)` Function Annotation
19. [Built-in Functions](#19-built-in-functions)
20. [Concurrency](#20-concurrency)
21. [File I/O](#21-file-io)
22. [Lambda Expressions](#22-lambda-expressions)
23. [Import System](#23-import-system)
    - 23.1 Import Aliases
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
    - 25.12 Compile-Time Array Evaluation
26. [Pipeline — Software-Prefetched Sequential Processing](#26-pipeline--software-prefetched-sequential-processing)
    - 26.1 Syntax
    - 26.2 The `__pipeline_i` iterator
    - 26.3 One-shot form
    - 26.4 Compiler guarantees
    - 26.5 When to use `pipeline` vs `times` / `for`
27. [Integer Type-Cast Reference](#27-integer-type-cast-reference)
    - 27.1 Overview Table
    - 27.2 Identity Casts: u64, i64, int, uint
    - 27.3 u32(x) — Unsigned 32-Bit Mask
    - 27.4 i32(x) — Signed 32-Bit Truncate
    - 27.5 u16(x) — Unsigned 16-Bit Mask
    - 27.6 i16(x) — Signed 16-Bit Truncate
    - 27.7 u8(x) — Unsigned 8-Bit Mask
    - 27.8 i8(x) — Signed 8-Bit Truncate
    - 27.9 bool(x) — Boolean Normalization
    - 27.10 Compile-Time Folding: Complete Rules
    - 27.11 Interaction with the Type System
    - 27.12 Complete Examples
28. [CF-CTRE — Cross-Function Compile-Time Reasoning Engine](#28-cf-ctre--cross-function-compile-time-reasoning-engine)
    - 28.1 Purpose and Position in Pipeline
    - 28.2 Core Object Model
    - 28.3 Function Eligibility Rules
    - 28.4 Execution Model
    - 28.5 Instruction Semantics
    - 28.6 Cross-Function Call Rules
    - 28.7 Pipeline Semantics and SIMD Tile Execution
    - 28.8 Specialization Engine
    - 28.9 Output and Integration Contract
    - 28.10 Performance Characteristics
    - 28.11 Programmer-Visible Effects
    - 28.12 Worked Examples

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
  → CF-CTRE           (cross-function compile-time reasoning engine, O1+)
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
| **CF-CTRE** | O1+ | Interprocedural compile-time interpreter; evaluates pure functions across call boundaries, memoises results, provides pipeline SIMD tile semantics |
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

Unsigned 64-bit values whose bit pattern exceeds `INT64_MAX` are accepted and stored as signed `i64` (two's complement):
```
0x9e3779b185ebca87   // negative as signed i64; bit pattern preserved
0xffffffffffffffff   // == -1 as signed i64
0x8000000000000000   // == INT64_MIN (-9223372036854775808)
```

**Float literals:**
```
3.14
2.0
1_000.5
1e2        // 100.0 (scientific notation)
1.5E-3     // 0.0015
2E3        // 2000.0
1.5e+2     // 150.0
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

**Bytes literals:** A hex byte-array literal creates an array of integer byte values at parse time:
```
var data = 0x"DEADBEEF";    // [0xDE, 0xAD, 0xBE, 0xEF]  (4 integers)
var b2   = 0x"01 02 03";    // [1, 2, 3] — spaces between byte pairs are allowed
var b3   = 0x"ff00ff";      // [255, 0, 255]  — lowercase hex also works
```
The literal is desugared to an array literal at parse time; `len(data) == 4`.

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

> **Important:** All integer types in OmScript (`int`, `i64`, `u64`, `i32`, `u32`, etc.) share the **same underlying LLVM representation: `i64`**. The width annotations affect how the compiler treats arithmetic results when you apply an explicit type-cast builtin — they do not change storage width. This is covered in detail in §5.7 and §27.

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

### 5.7 Integer Type-Cast Syntax

OmScript provides **function-call-style type coercions** that truncate or sign-extend integer values to a target bit width. These look like function calls but are recognized specially by the compiler and generate zero-overhead bitwise operations in LLVM IR.

| Syntax | Runtime Behavior | LLVM IR Emitted | Identity? |
|---|---|---|---|
| `u64(x)` | No-op — identity | none (value passes through) | ✓ |
| `i64(x)` | No-op — identity | none | ✓ |
| `int(x)` | No-op — identity | none | ✓ |
| `uint(x)` | No-op — identity | none | ✓ |
| `u32(x)` | Mask to lower 32 bits | `and i64 %x, 4294967295` | — |
| `i32(x)` | Truncate to 32 bits, sign-extend back to 64 | `trunc i64 → i32`, `sext i32 → i64` | — |
| `u16(x)` | Mask to lower 16 bits | `and i64 %x, 65535` | — |
| `i16(x)` | Truncate to 16 bits, sign-extend back to 64 | `trunc i64 → i16`, `sext i16 → i64` | — |
| `u8(x)` | Mask to lower 8 bits | `and i64 %x, 255` | — |
| `i8(x)` | Truncate to 8 bits, sign-extend back to 64 | `trunc i64 → i8`, `sext i8 → i64` | — |
| `bool(x)` | Normalize to 0 or 1 | `icmp ne i64 %x, 0`, `zext i1 → i64` | — |

**Compile-time folding:** All nine forms are recognized by `evalConstBuiltin` inside `comptime {}` blocks and wherever constant folding applies. For example, `u8(300)` folds to `44` at compile time (300 & 0xFF = 44).

**Key use case — string byte extraction:** When working with string bytes inside OPTMAX or comptime functions, you frequently need to treat a character code as an unsigned byte before packing it into a wider integer. The `u64(s[i])` pattern is the idiomatic way to do this:

```omscript
// Pack 8 bytes of a string into a single u64, little-endian
fn pack8(s:string, base:int) -> u64 {
    var x:u64 = 0;
    x |= u64(s[base + 0]) << 0;
    x |= u64(s[base + 1]) << 8;
    x |= u64(s[base + 2]) << 16;
    x |= u64(s[base + 3]) << 24;
    x |= u64(s[base + 4]) << 32;
    x |= u64(s[base + 5]) << 40;
    x |= u64(s[base + 6]) << 48;
    x |= u64(s[base + 7]) << 56;
    return x;
}
```

Without `u64(s[i])`, the character code would be sign-extended from `i8` to `i64`, potentially setting upper bits. The `u64()` identity cast documents the intent clearly and prevents future sign-extension bugs.

**Signed wrapping example:**
```omscript
var x:int = 300;
var a = u8(x);    // 44   (300 & 0xFF)
var b = i8(x);    // 44   (300 trunc to i8 = 0x2C = 44 — positive, so same)
var c = u8(200);  // 200
var d = i8(200);  // -56  (200 trunc to i8 = 0xC8 = -56 in two's complement)
var e = bool(0);  // 0
var f = bool(99); // 1
```

See §27 for the complete type-cast reference with all edge cases documented.

---

## 6. Variables and Constants

### 6.1 Variable Declaration

```
var x = 42                // mutable, no annotation
var x:int = 42            // mutable, annotated
var arr = [1, 2, 3]       // array
var d = map_new()         // dictionary
var a = 1, b = 2, c = 3   // multiple variables in one declaration
const x = 10, y = 20      // also works with const
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

A `comptime {}` block is executed **entirely at compile time** by the constant evaluator. The result — which may be a constant integer, string value, or **array** — is then substituted at the call site as a literal constant or global constant. No runtime code is emitted for the block itself.

#### 6.3.1 Basic Usage

```omscript
var a = comptime { return 6 * 7; };           // a = 42
var b = comptime { var n = 5; return n * n; }; // b = 25
const c = comptime { return min(10, 20); };    // c = 10
```

#### 6.3.2 Implicit Return (v4.1.1+)

The **last bare expression statement** in a `comptime` block is automatically the return value — no `return` keyword is required. Both forms are equivalent:

```omscript
// With explicit return (always worked):
var x = comptime { return 6 * 7; };

// With implicit return (new in v4.1.1):
var x = comptime { 6 * 7; };

// More complex example — implicit return of last expression:
var y = comptime {
    var n = 5;
    n * n;           // ← implicit return value; no 'return' needed
};
// y = 25
```

An explicit `return` in the middle of a `comptime` block still works and exits the block early. The implicit-return rule only applies to the last statement when no explicit `return` is present.

#### 6.3.3 Builtin Functions in comptime

Many built-in functions are recognized as **pure** and evaluated at compile time when their arguments are constant. The full list of comptime-foldable builtins:

**Math builtins (comptime-foldable):**
`abs`, `min`, `max`, `pow`, `sqrt`, `floor`, `ceil`, `round`, `exp2`, `log2`, `sign`, `clamp`, `lcm`, `gcd`, `is_even`, `is_odd`, `is_power_of_2`, `saturating_add`, `saturating_sub`

**Bit manipulation (comptime-foldable):**
`popcount`, `clz`, `ctz`, `bswap`, `bitreverse`, `rotate_left`, `rotate_right`

**String builtins (comptime-foldable):**
`len` / `str_len`, `str_eq`, `str_concat`, `str_upper`, `str_lower`, `str_contains`, `str_index_of`, `str_replace`, `str_trim`, `str_starts_with`, `str_ends_with`, `str_repeat`, `str_reverse`, `str_count`, `str_pad_left`, `str_pad_right`, `str_to_int`, `to_char`, `is_alpha`, `is_digit`

**Array builtins (comptime-foldable):**
`len` (on `array_fill`, `range`, `range_step`, `array_concat`, `str_chars`), `sum`, `array_product`, `array_last`, `array_min`, `array_max`, `array_contains`, `array_find`, `index_of`

**Integer type-cast builtins (comptime-foldable, v4.1.1+):**
`u64`, `i64`, `int`, `uint`, `u32`, `i32`, `u16`, `i16`, `u8`, `i8`, `bool`

```omscript
const HASH_BITS = comptime { popcount(0xDEADBEEF); };  // 24
const KEY_LEN   = comptime { str_len("hello");        };  // 5
const MASK      = comptime { u32(0xFFFFFFFF);         };  // 4294967295
const FLAG      = comptime { bool(42);                };  // 1
```

#### 6.3.4 Array-Returning User Functions (v4.1.1+)

`comptime` blocks can now call **user-defined functions that return arrays**. The entire function body is evaluated at compile time by the constant evaluator. The result is emitted as a `private unnamed_addr constant [N+1 x i64]` global in LLVM IR — using OmScript's standard `[length, elem0, elem1, ...]` array layout — and the variable at the call site becomes a pointer to that global. **No runtime allocation occurs, no function is called at runtime.**

This is the most powerful feature of `comptime` blocks and enables zero-cost compile-time table generation:

```omscript
// Build a lookup table at compile time
fn make_sin_table(n:int) -> float[] {
    var out:float[] = array_fill(n, 0);
    for (i:int in 0...n) {
        // Approximate sin with integer math for comptime compatibility
        out[i] = i;  // placeholder — real use would be pure integer math
    }
    return out;
}

var SIN_TABLE:float[] = comptime { make_sin_table(256); };
// SIN_TABLE is a compile-time global constant — zero runtime overhead
```

**The `str_to_u64_fast` pattern** — the canonical example of comptime array generation. This function converts a string into an array of 64-bit words (little-endian, 8 bytes per word), using `u64(s[i])` to zero-extend each byte before bitwise ORing it into position:

```omscript
fn str_to_u64_fast(s:string) -> u64[] {
    var n:int = len(s);
    var blocks:int = (n + 7) >> 3;
    var out:u64[] = array_fill(blocks, 0);
    for (i:int in 0...blocks) {
        var base:int = i << 3;
        var x:u64 = 0;
        if (base + 0 < n) { x |= u64(s[base + 0]) << 0;  }
        if (base + 1 < n) { x |= u64(s[base + 1]) << 8;  }
        if (base + 2 < n) { x |= u64(s[base + 2]) << 16; }
        if (base + 3 < n) { x |= u64(s[base + 3]) << 24; }
        if (base + 4 < n) { x |= u64(s[base + 4]) << 32; }
        if (base + 5 < n) { x |= u64(s[base + 5]) << 40; }
        if (base + 6 < n) { x |= u64(s[base + 6]) << 48; }
        if (base + 7 < n) { x |= u64(s[base + 7]) << 56; }
        out[i] = x;
    }
    return out;
}

// The entire function body is evaluated at compile time.
// The string "hello" has 5 bytes → 1 block.
// Emitted as: @M = private unnamed_addr constant [2 x i64] [i64 1, i64 0x6F6C6C6568]
var M:u64[] = comptime { str_to_u64_fast("hello"); };
```

The generated global has layout `[N+1 x i64]` where the first element is the array length (`N`) and the remaining `N` elements are the array values. This is OmScript's standard array representation (same as heap arrays, but in read-only static memory).

**Registration in `arrayReturningFunctions_`:** When the compiler encounters a user function called inside a `comptime` block that returns an array, it registers the function in the internal `arrayReturningFunctions_` set. This ensures that downstream type-inference and alias analysis know the variable has array type, even though no runtime `malloc` was performed.

#### 6.3.5 comptime Evaluation Rules

A `comptime` block is **guaranteed to evaluate at compile time**. If the body references any non-constant value, the compiler emits a **hard error** — it never silently falls back to runtime evaluation.

**Allowed inside comptime:**
- `var` declarations with constant initializers
- Arithmetic expressions with constant operands
- Any comptime-foldable builtin function (see §6.3.3)
- String operations on string literals
- Array operations on constant arrays
- User-defined pure functions whose bodies can themselves be fully evaluated
- Integer type-cast builtins: `u8(x)`, `u16(x)`, etc.
- `if`/`else` with constant conditions
- `for (i in 0...N)` loops with constant bounds
- `array_fill`, `range`, `array_concat` with constant arguments

**Not allowed inside comptime:**
- Reading non-constant variables from outer scope
- I/O operations (`print`, `input`, etc.)
- File operations (`file_read`, etc.)
- Threading operations
- Random number generation (`random()`)
- Any function with observable side effects

**Example — comptime block with control flow:**
```omscript
const LOOKUP:int[] = comptime {
    var table:int[] = array_fill(16, 0);
    for (i:int in 0...16) {
        table[i] = i * i;   // squares: 0, 1, 4, 9, 16, ...
    }
    table;   // implicit return
};
// LOOKUP == [0, 1, 4, 9, 16, 25, 36, 49, 64, 81, 100, 121, 144, 169, 196, 225]
```

#### 6.3.6 comptime vs const

| Feature | `const x = expr` | `var x = comptime { ... }` |
|---|---|---|
| Guaranteed compile-time | Only if `expr` is a literal or fold-able | Always (error if not possible) |
| Can contain loops | No | Yes |
| Can call pure functions | No | Yes |
| Can return arrays | No | Yes (v4.1.1+) |
| Reassignable | No | Yes (technically) |
| Propagates downstream | Yes | Yes (treated as const for folding) |

A `var` initialized from a `comptime` block participates in downstream constant folding just like a `const` — even though it is technically reassignable, the compiler knows its initial value is a compile-time constant and will fold arithmetic on it.

#### 6.3.7 Downstream Constant Propagation

The result of a `comptime` block participates in the full constant-folding chain. Subsequent operations on a `comptime`-initialized variable may also be folded:

```omscript
var BLOCK_SIZE:int = comptime { 1 << 6; };    // 64
var HALF:int = BLOCK_SIZE / 2;                // also folded: 32
var MASK:int = BLOCK_SIZE - 1;                // also folded: 63
var LOG2:int = log2(BLOCK_SIZE);              // also folded: 6
```

All four variables become compile-time constants — no division, subtraction, or `log2` call occurs at runtime.



### 6.4 Register Variables

```
register var x:int = 0    // hint to force variable into CPU register
```

Instructs the compiler to promote the variable to an SSA register via LLVM's `mem2reg` pass. Only works for types that fit in a register (integers, floats, bools).

### 6.5 Assignment

```
x = 10
arr[i] = value
obj.field = value
```

### 6.6 Compound Assignment

```
x += 1     x -= 1     x *= 2     x /= 2     x %= 3
x &= mask  x |= flag  x ^= bits  x <<= 2    x >>= 2
x **= 2    x ??= default_val
x &&= rhs  // x = x && rhs  (logical AND assignment)
x ||= rhs  // x = x || rhs  (logical OR assignment)
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

Self-recursive calls in tail position are automatically converted to `musttail` jumps — a **guaranteed** tail call elimination (TCO), not merely a hint. The `musttail` calling convention requires the callee's signature to exactly match the caller's. When this condition is met, LLVM is obligated to emit a jump (not a call), ensuring **O(1) stack usage** regardless of recursion depth.

Applies only to **self-recursive** calls (a function calling itself) where all parameter types match the current function's signature.

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

### 7.10 Method Call Syntax

Any call of the form `receiver.method(args...)` is desugared at parse time to `method(receiver, args...)`. This works for all user-defined functions and all built-in functions, making it easy to write object-oriented style code:

```
var arr = [3, 1, 2];
arr.sort();                // sort(arr)
var n = arr.len();         // len(arr)
var m = arr.array_max();   // array_max(arr)

var s = "Hello World";
var u = s.str_upper();     // str_upper(s) → "HELLO WORLD"
var l = s.len();           // len(s) → 11

struct Vec2 { x, y }
fn dot(u, v) { return u.x * v.x + u.y * v.y; }
var a = Vec2 { x: 3, y: 4 };
var b = Vec2 { x: 1, y: 2 };
var d = a.dot(b);          // dot(a, b) → 11
```

The desugaring is purely syntactic — there is no runtime dispatch or vtable lookup.

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

### 9.2 do...while / do...until

```
do {
    // ...
} while (condition);
```

`do...until` is also supported — stops when the condition becomes **true** (desugars to `do { ... } while (!condition)`):

```
do {
    i += 1;
} until (i >= limit);   // equivalent to: do { i += 1; } while (!(i >= limit))
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

**Chained comparisons:** A chain of `<`, `<=`, `>`, or `>=` operators is automatically desugared to a conjunction. Middle operands that are identifiers or literals are evaluated only once:

```
1 < x < 10           // (1 < x) && (x < 10)
1 <= a <= b <= 100   // (1 <= a) && (a <= b) && (b <= 100)
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

### 10.5.1 Elvis Operator

The `?:` operator is a shorthand null-coalescing/ternary. If the left operand is truthy, it returns the left operand itself; otherwise it returns the right operand:

```
x ?: default_val   // if x is truthy, yields x; else yields default_val
```

For identifiers and literals this desugars to `x ? x : default_val`. For complex expressions it desugars to `x ?? default_val`.

### 10.5.2 `in` Operator

The `in` operator tests whether a value exists in an array (desugars to `array_contains(arr, val)`). It has comparison-level precedence:

```
if (x in arr) { ... }         // 1 if arr contains x
val = (42 in collection);     // expression context
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
10. `==`, `!=`, `<`, `<=`, `>`, `>=`, `in` — comparisons also support chaining (`a < b < c`)
11. `&&`
12. `||`
13. `??`, `?:`
14. `? :` (ternary)
15. Assignment (`=`, `+=`, `-=`, `*=`, `/=`, `%=`, `**=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `??=`, `&&=`, `||=`)
16. `|>` (pipe forward)

### 10.13 Type-Namespace Method Dispatch

The `::` operator supports calling built-in operations using a type name as a namespace. This is purely syntactic sugar — it desugars to the corresponding function call or operator at parse time.

**Integer namespace** (`int`, `i64`, `i32`, `i16`, `i8`, `u64`, `u32`, `u16`, `u8`, `uint`):
```
int::abs(x)         // abs(x)
int::min(a, b)      // min(a, b)
int::max(a, b)      // max(a, b)
int::sign(x)        // sign(x)
int::clamp(x,lo,hi) // clamp(x, lo, hi)
int::mod(a, b)      // a % b
int::is_even(x)     // is_even(x)
int::is_odd(x)      // is_odd(x)
int::to_float(x)    // to_float(x)
int::bitand(a, b)   // a & b
int::bitor(a, b)    // a | b
int::bitxor(a, b)   // a ^ b
int::shl(a, n)      // a << n
int::shr(a, n)      // a >> n
int::pow(a, b)      // pow(a, b)
int::gcd(a, b)      // gcd(a, b)
int::lcm(a, b)      // lcm(a, b)
```

**Float namespace** (`float`, `f64`, `f32`, `double`):
```
float::sqrt(x)      // sqrt(x)
float::floor(x)     // floor(x)
float::ceil(x)      // ceil(x)
float::round(x)     // round(x)
float::abs(x)       // abs(x)
float::sin(x)       // sin(x)
float::cos(x)       // cos(x)
float::to_int(x)    // to_int(x)
float::min(a, b)    // min_float(a, b)
float::max(a, b)    // max_float(a, b)
```

**String namespace** (`string`, `str`):
```
string::len(s)              // len(s)
string::contains(s, sub)    // str_contains(s, sub)
string::replace(s, old, new) // str_replace(s, old, new)
string::str_upper(s)        // str_upper(s)
string::str_lower(s)        // str_lower(s)
string::trim(s)             // str_trim(s)
string::split(s, delim)     // str_split(s, delim)
string::to_int(s)           // str_to_int(s)
string::to_float(s)         // str_to_float(s)
```

**Array namespace** (`array`, `arr`):
```
array::len(a)           // len(a)
array::push(a, v)       // push(a, v)
array::pop(a)           // pop(a)
array::sort(a)          // sort(a)
array::reverse(a)       // reverse(a)
array::sum(a)           // sum(a)
array::min(a)           // array_min(a)
array::max(a)           // array_max(a)
```

**Bool namespace** (`bool`):
```
bool::and(a, b)   // a && b
bool::or(a, b)    // a || b
bool::not(x)      // !x
bool::xor(a, b)   // a ^ b
```

### 10.14 Integer Type-Cast Dispatch

The integer type-cast syntax (`u8(x)`, `i32(x)`, etc.) looks like a function call but is handled specially by the compiler via a dedicated dispatch path in `generateCall` / `generateBuiltin`. This section documents the precise semantics of each cast, the LLVM IR emitted, and when compile-time folding applies.

**How the dispatch works:**
1. The lexer recognizes `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `int`, `uint`, `bool` as keyword-like identifiers.
2. When the parser sees one of these identifiers followed by `(`, it marks the CALL_EXPR with `isCastBuiltin = true`.
3. During codegen, `generateCall` checks for `isCastBuiltin` and routes to the type-cast handler rather than the normal user-function lookup.
4. The type-cast handler emits the appropriate LLVM bitwise/trunc/sext sequence.
5. Inside a `comptime` block or during `evalConstBuiltin`, the handler folds the result immediately to a constant integer.

**Detailed semantics per cast:**

```omscript
// Identity casts — value is passed through unchanged
var a = u64(x);    // x as-is; documents "treat as unsigned 64-bit"
var b = i64(x);    // x as-is
var c = int(x);    // x as-is
var d = uint(x);   // x as-is

// Zero-extension (masking) casts — clear upper bits
var e = u32(x);    // x & 0x00000000FFFFFFFF
var f = u16(x);    // x & 0x000000000000FFFF
var g = u8(x);     // x & 0x00000000000000FF

// Sign-extension (truncate + sext) casts — preserve signed value
var h = i32(x);    // ((x << 32) >> 32) — i.e., trunc to i32 then sext
var i_ = i16(x);   // trunc to i16 then sext to i64
var j = i8(x);     // trunc to i8 then sext to i64

// Boolean normalization
var k = bool(x);   // (x != 0) ? 1 : 0
```

**Concrete examples with literal folding:**
```omscript
// All of these fold at compile time:
const A = u8(300);       // 44   (300 & 0xFF)
const B = i8(200);       // -56  (200 → 0xC8 trunc→i8 = -56 sext→i64)
const C = u16(70000);    // 4464 (70000 & 0xFFFF)
const D = i16(40000);    // -25536 (40000 → 0x9C40 trunc→i16 = -25536)
const E = u32(-1);       // 4294967295
const F = i32(0x1_0000_0001); // 1  (low 32 bits = 1, positive → sext = 1)
const G = bool(0);       // 0
const H = bool(-99);     // 1
const I = bool(1);       // 1
```

**Inside comptime blocks:**
```omscript
// The type-cast builtins work inside comptime and fold the entire expression:
var BYTE_MASK:u64 = comptime { u64(0xFF); };      // 255
var WORD_MASK:u64 = comptime { u64(0xFFFF); };    // 65535
var NORMALIZED:int = comptime { bool(42); };       // 1

// Used in a loop inside comptime:
fn build_byte_table(n:int) -> u8[] {
    var t:u8[] = array_fill(n, 0);
    for (i:int in 0...n) {
        t[i] = u8(i * 17);   // comptime-folds: u8(0), u8(17), u8(34), ...
    }
    return t;
}
var BYTE_TABLE:u8[] = comptime { build_byte_table(16); };
```

**Interaction with bitwise operations:**
```omscript
// Classic byte-packing idiom using u64() for zero-extension:
fn pack_bytes(a:int, b:int, c:int, d:int) -> u64 {
    return u64(a) | (u64(b) << 8) | (u64(c) << 16) | (u64(d) << 24);
}

// Without u64(), the shift might produce unexpected results if the value
// has high bits set (e.g., from a previous operation):
//   bad: a | (b << 8)  — if a = 0x1FF, upper bit bleeds into byte 2
//   good: u8(a) | (u8(b) << 8)  — guarantees each byte is exactly 8 bits
```

---

## 11. Arrays

### 11.1 Array Literals

```
var arr = [1, 2, 3]
var empty = []
var typed:int[] = [10, 20, 30]
```

### 11.2 Indexing and Slicing

```
arr[0]          // read (bounds-checked at O0/O1)
arr[i] = val    // write
s[i]            // string subscript: returns the integer character code at index i
s[i] = code     // string byte assignment: writes a single byte (integer) at index i
                //   only valid on heap-allocated strings (str_concat, str_upper, etc.)
                //   literal strings are read-only
```

**Slice syntax** — extracts a sub-array using `start:end` notation (end is exclusive):
```
arr[1:4]    // sub-array [arr[1], arr[2], arr[3]] — desugars to array_slice(arr, 1, 4)
arr[:4]     // from index 0 to 3 — desugars to array_slice(arr, 0, 4)
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

### 13.0 Dict Literals

Dict literals create a map inline without calling `map_new()`:

```
var d = {"key1": 100, "key2": 200}
var e = {}                              // empty dict
var counts = {"a": 1, "b": 2, "c": 3}
```

Dict literals are expressions and can be used anywhere a dict value is expected. They are equivalent to calling `map_new()` followed by a series of `map_set()` calls.

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
    range(0, 100) percent: int,  // value range hint
    move payload: int        // field carries move semantics
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
    RED,
    GREEN = 10,
    BLUE
}

var c = Color::RED   // scope resolution (preferred)
var g = Color::GREEN // 10
```

Enum members without explicit values are auto-numbered from 0 (or from the previous explicit value + 1).

**Access forms:**
- `EnumName::MEMBER` — scope-resolution syntax (preferred)
- `EnumName_MEMBER` — underscore-separated flat name (also valid; generated internally)

> **Note:** `Color.Red` (dot-access syntax) is **not** valid — dot notation applies to struct field access on variable instances, not to enum type names. Use `Color::RED` or `Color_RED`.

Enum values are integer constants and can be used in any arithmetic or switch expression:

```
enum Status { OK = 0, ERROR = 1, PENDING = 2 }

var s = Status::ERROR;
if (s == Status::ERROR) { println("error"); }

switch (s) {
    case 0: { println("ok"); break; }
    case 1: { println("error"); break; }
    default: { println("pending"); }
}
```

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

### 18.1 `@optmax(...)` Function Annotation

As an alternative to `OPTMAX=: ... OPTMAX!:` blocks, the `@optmax(...)` annotation applies OPTMAX semantics to a single function with fine-grained control over individual optimization knobs:

```
@optmax(
    safety = off,              // off | relaxed | on  (bounds check behavior)
    fast_math = true,          // enable LLVM fast-math flags
    report = false,            // print per-function opt report to stdout
    loop = {
        unroll = 4,            // loop unroll factor
        vectorize = true,      // force vectorization
        tile = 32,             // loop tiling block size
        parallel = true        // assert iteration independence
    },
    memory = {
        prefetch = true,       // auto-insert prefetch hints
        noalias = true,        // mark all params noalias
        stack = true           // prefer stack allocation
    },
    assume = ["n > 0", "n < 1000"],   // assertions injected at function entry
    specialize = ["hot_path"]          // specialization hints
)
fn dot(a:float[], b:float[], n:int) -> float {
    var sum:float = 0.0;
    for (i:int in 0...n) { sum += a[i] * b[i]; }
    return sum;
}
```

Bare `@optmax` (without parentheses) applies OPTMAX semantics with default settings:

```
@optmax
fn fast_fn(arr:int[], n:int) -> int { ... }
```

---

## 19. Built-in Functions

### 19.1 I/O

| Function | Description |
|---|---|
| `print(val)` | Print value followed by a newline |
| `println(val)` | Print value followed by a newline (identical behavior to `print`) |
| `write(val)` | Print value **without** a trailing newline |
| `print_char(c)` | Print a single character by ASCII/Unicode code |
| `input()` | Read a whitespace-delimited word from stdin |
| `input_line()` | Read a full line from stdin |
| `exit(code)` / `exit_program(code)` | Exit with given code |

> **Note:** Both `print()` and `println()` always append a newline. Use `write()` when you need output without a trailing newline.

### 19.2 Math

| Function | Description | Compile-Time Folds? |
|---|---|---|
| `abs(x)` | Absolute value | ✓ with literal arg |
| `pow(x, y)` | x to the power y (integer exponentiation) | ✓ with literal args |
| `sqrt(x)` | Integer square root | ✓ with literal arg |
| `cbrt(x)` | Cube root (float result) | — |
| `exp(x)` | e^x (float result) | — |
| `exp2(x)` | 2^x | ✓ with literal arg |
| `log(x)` | Natural logarithm (float result) | — |
| `log2(x)` | Base-2 logarithm | ✓ with literal arg |
| `log10(x)` | Base-10 logarithm (float result) | — |
| `floor(x)` | Floor | ✓ with literal arg |
| `ceil(x)` | Ceiling | ✓ with literal arg |
| `round(x)` | Round to nearest integer | ✓ with literal arg |
| `sin(x)` | Sine (float result) | — |
| `cos(x)` | Cosine (float result) | — |
| `tan(x)` | Tangent (float result) | — |
| `asin(x)` | Arc sine (float result) | — |
| `acos(x)` | Arc cosine (float result) | — |
| `atan(x)` | Arc tangent (float result) | — |
| `atan2(y, x)` | Two-argument arc tangent (float result) | — |
| `hypot(x, y)` | sqrt(x² + y²) (float result) | — |
| `min(a, b)` | Minimum of two values | ✓ with literal args |
| `max(a, b)` | Maximum of two values | ✓ with literal args |
| `min_float(a, b)` | Floating-point minimum (NaN-aware) | — |
| `max_float(a, b)` | Floating-point maximum (NaN-aware) | — |
| `sign(x)` | Sign: -1, 0, or 1 | ✓ with literal arg |
| `clamp(x, lo, hi)` | Clamp x to [lo, hi] | ✓ with literal args |
| `gcd(a, b)` | Greatest common divisor | ✓ with literal args |
| `lcm(a, b)` | Least common multiple | ✓ with literal args |
| `is_even(x)` | 1 if even, 0 otherwise | ✓ with literal arg |
| `is_odd(x)` | 1 if odd, 0 otherwise | ✓ with literal arg |
| `is_power_of_2(x)` | 1 if x is a power of 2, 0 otherwise | ✓ with literal arg |
| `fma(a, b, c)` | Fused multiply-add: a*b+c | — |
| `copysign(x, y)` | Magnitude of x with sign of y | — |
| `random()` | Random float in [0.0, 1.0) | — (side-effectful) |

### 19.3 Arithmetic with Explicit Overflow/Precision Mode

| Function | Description | Compile-Time Folds? |
|---|---|---|
| `fast_add(a, b)` | Addition with `nsw` (no signed wrap) flag | — |
| `fast_sub(a, b)` | Subtraction with `nsw` flag | — |
| `fast_mul(a, b)` | Multiplication with `nsw` flag | — |
| `fast_div(a, b)` | Division (exact, no remainder) | — |
| `precise_add(a, b)` | Addition without unsafe flags | — |
| `precise_sub(a, b)` | Subtraction without unsafe flags | — |
| `precise_mul(a, b)` | Multiplication without unsafe flags | — |
| `precise_div(a, b)` | Division without unsafe flags | — |
| `saturating_add(a, b)` | Saturating addition (clamps at INT64_MAX) | ✓ with literal args |
| `saturating_sub(a, b)` | Saturating subtraction (clamps at INT64_MIN) | ✓ with literal args |

The `fast_*` variants emit the `nsw` (no signed wrap) flag on the LLVM instruction, which tells LLVM that signed overflow is undefined behavior — enabling stronger optimizations like loop-carried IV rewriting. Use these only when you have proven no overflow can occur.

The `precise_*` variants explicitly omit any wrap/exact flags, giving conservative semantics that match C's two's-complement integer arithmetic.

The `saturating_*` variants map to LLVM's `llvm.sadd.sat.i64` / `llvm.ssub.sat.i64` intrinsics and fold at compile time with literal arguments:
```omscript
const MAX_PLUS_1 = saturating_add(9223372036854775807, 1);  // 9223372036854775807 (no overflow)
const MIN_MINUS_1 = saturating_sub(-9223372036854775808, 1); // -9223372036854775808 (no underflow)
```

### 19.4 Bit Manipulation

| Function | Description | Compile-Time Folds? |
|---|---|---|
| `popcount(x)` | Count set bits (maps to POPCNT instruction on x86) | ✓ with literal arg |
| `clz(x)` | Count leading zeros (maps to LZCNT/BSR) | ✓ with literal arg |
| `ctz(x)` | Count trailing zeros (maps to TZCNT/BSF) | ✓ with literal arg |
| `bitreverse(x)` | Reverse all bit positions | ✓ with literal arg |
| `bswap(x)` | Byte-swap / endianness swap | ✓ with literal arg |
| `rotate_left(x, n)` | Rotate bits left by n positions | ✓ with literal args |
| `rotate_right(x, n)` | Rotate bits right by n positions | ✓ with literal args |

```omscript
// All compile-time folds:
const BITS  = popcount(0xFF00FF00);  // 16
const LEAD  = clz(0x0001000000000000); // 15
const TRAIL = ctz(0x0010);           // 4
const REV   = bitreverse(0x0F0F0F0F); // 0xF0F0F0F000000000 (as u64)
const SWAP  = bswap(0x0102030405060708); // 0x0807060504030201
const ROT   = rotate_left(1, 3);     // 8
```

### 19.5 Type Utilities

| Function | Description |
|---|---|
| `typeof(x)` | Returns an integer type tag: 1 = integer, 2 = float, 3 = string |
| `len(x)` | Length of array or string |
| `to_int(x)` | Convert to integer |
| `to_float(x)` | Convert to float |
| `to_string(x)` | Convert number to string |
| `assert(cond)` | Runtime assertion (aborts on failure) |

### 19.5.1 Integer Type-Cast Functions

These are **function-call-style type coercions**, not ordinary functions. They are dispatched by the compiler to dedicated IR-generation code and fold at compile time inside `comptime` blocks. See §5.7 and §10.14 for the full rationale; §27 for the complete reference.

| Syntax | Behavior | Compile-Time Folds? |
|---|---|---|
| `u64(x)` | Identity — no-op | ✓ |
| `i64(x)` | Identity — no-op | ✓ |
| `int(x)` | Identity — no-op | ✓ |
| `uint(x)` | Identity — no-op | ✓ |
| `u32(x)` | `x & 0xFFFFFFFF` — mask to lower 32 bits | ✓ |
| `i32(x)` | Truncate to 32 bits + sign-extend to 64 | ✓ |
| `u16(x)` | `x & 0xFFFF` — mask to lower 16 bits | ✓ |
| `i16(x)` | Truncate to 16 bits + sign-extend to 64 | ✓ |
| `u8(x)` | `x & 0xFF` — mask to lower 8 bits | ✓ |
| `i8(x)` | Truncate to 8 bits + sign-extend to 64 | ✓ |
| `bool(x)` | `(x != 0) ? 1 : 0` | ✓ |

```omscript
var a = u8(300);     // 44
var b = i8(200);     // -56
var c = bool(0);     // 0
var d = bool(42);    // 1
var e = u32(-1);     // 4294967295
var f = i32(-1);     // -1 (sign-preserved)
```



### 19.6 Time / System

| Function | Description |
|---|---|
| `time()` | Current Unix timestamp in seconds (integer) |
| `sleep(ms)` | Sleep for `ms` milliseconds |

### 19.6.1 Shell / Process

| Function | Description |
|---|---|
| `command(cmd)` | Run shell command `cmd` via `popen(3)` and return its stdout as a string. Returns `""` on failure. Also available as `std::command(cmd)`. |
| `shell(cmd)` | Alias for `command(cmd)`. |

```omscript
var output = command("echo hello");      // "hello\n"
var files  = shell("ls /tmp");           // directory listing
var lines  = str_split(output, "\n");    // split into lines
```

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

### 23.1 Import Aliases

An imported module can be given a namespace alias using `as`:

```
import "utils" as utils;        // functions accessible as utils::funcname()
import "math/fast" as fast;     // fast::dot(), fast::norm(), etc.
```

With an alias, the imported symbols are accessed via `::` scope resolution. Without an alias, all symbols are imported into the current scope directly.

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

**Verbose output (`-V` / `--verbose`):** In addition to standard compiler progress, prints an **opt-report** summary after compilation showing how many of each optimization class were applied:

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

When the compiler is run with `-V` / `--verbose`, it prints an **opt-report** summary after compilation listing all optimization counters:

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

### 25.12 Compile-Time Array Evaluation

OmScript's `comptime` system (§6.3) can evaluate user-defined functions that return arrays entirely at compile time, emitting the results as read-only global constants. This section documents the exact mechanism, the generated IR, and all the patterns that enable it.

#### 25.12.1 Global Layout: `[N+1 x i64]`

All OmScript arrays — both heap-allocated at runtime and compile-time constants — use the same memory layout:

```
[length, elem0, elem1, elem2, ...]
```

The first `i64` word is the **length** of the array. The remaining `N` words are the elements. This means:
- `arr[0]` is stored at `base + 8` (offset 1 × 8 bytes)
- `arr[i]` is stored at `base + 8*(i+1)`
- `len(arr)` reads `*(base + 0)`

For a compile-time array of 3 elements `[10, 20, 30]`, the global is:
```llvm
@arr = private unnamed_addr constant [4 x i64] [i64 3, i64 10, i64 20, i64 30]
```

For an empty array `[]`:
```llvm
@arr = private unnamed_addr constant [1 x i64] [i64 0]
```

#### 25.12.2 The `emitComptimeArray` Helper

`emitComptimeArray` is the internal codegen function responsible for creating `[N+1 x i64] private unnamed_addr constant` globals. It is called from two places:
1. **COMPTIME_EXPR emitter** — when a `comptime { }` block's evaluated result is an array.
2. **Call-site constant folder** — when a pure user function is called with all-constant arguments and returns an array.

The function:
1. Allocates an `llvm::ArrayType` of size `N+1` (i64 elements).
2. Populates element 0 with the length `N` as a constant `i64`.
3. Populates elements 1..N with the computed array values as constant `i64`s.
4. Creates a `GlobalVariable` with `private` linkage, `unnamed_addr`, and `constant` storage class.
5. Returns a `bitcast` of the global pointer to `i64*`, matching OmScript's array-pointer type.

The generated variable is also stored in the compiler's internal constant table so that subsequent `len()` calls on it fold immediately to the literal length without reading from the global.

#### 25.12.3 Constant Folding Chains for Arrays

Many array builtins participate in a **folding chain** — when their argument is itself a constant array, the result is also a constant:

| Builtin call | Folds when | Result |
|---|---|---|
| `len(array_fill(N, v))` | `N` is a literal integer | Literal `N` |
| `len(range(a, b))` | `a`, `b` are literals | Literal `b - a` |
| `len(range_step(a, b, s))` | all literal | Literal `(b-a)/s` |
| `len(str_chars(s))` | `s` is a string literal | Literal `strlen(s)` |
| `len(array_concat(a, b))` | both `a`, `b` are constant arrays | Literal `len(a)+len(b)` |
| `sum([1,2,3])` | all elements are literals | Literal sum |
| `sum(array_fill(n, v))` | `n`, `v` are literals | Literal `n*v` |
| `sum(range(a, b))` | `a`, `b` are literals | Literal `(a+b-1)*(b-a)/2` |
| `array_min([...])` | all elements are literals | Literal minimum |
| `array_max([...])` | all elements are literals | Literal maximum |
| `array_product([...])` | all elements are literals | Literal product |
| `array_last([...])` | all elements are literals | Literal last element |
| `array_contains([...], v)` | all elements and `v` are literals | Literal 0 or 1 |
| `array_find([...], v)` | all elements and `v` are literals | Literal index or -1 |
| `index_of([...], v)` | all elements and `v` are literals | Literal index or -1 |

These chains compose: `sum(range(1, 101))` folds to `5050` without allocating an array or computing a loop.

#### 25.12.4 The `str_to_u64_fast` Pattern in Detail

The canonical use case for comptime array evaluation is encoding a string as an array of 64-bit words at compile time for use in fast hashing, pattern matching, or SIMD comparisons.

**How the compiler evaluates `str_to_u64_fast("hello")`:**

1. The `comptime` evaluator sees a call to a user function with a string-literal argument.
2. It looks up `str_to_u64_fast` in the function table and finds it is a pure function (no side effects).
3. It registers `str_to_u64_fast` in `arrayReturningFunctions_` since its return type is `u64[]`.
4. It executes the function body step by step:
   - `len("hello")` → 5 (constant-folds immediately)
   - `(5 + 7) >> 3` → 1 (one 8-byte block covers 5 bytes)
   - `array_fill(1, 0)` → constant array `[0]`
   - Loop `i in 0...1`: `base = 0`, then processes bytes at indices 0–4
   - `u64(s[0])` → `u64('h')` → `u64(104)` → 104
   - `104 << 0` → 104; `u64(s[1]) << 8` → `101 << 8` = 25856; etc.
   - Final `x` = `0x6F6C6C6568` (little-endian encoding of "hello\0\0\0")
5. `emitComptimeArray` is called with `[1, 0x6F6C6C6568]`.
6. The global `@M = private unnamed_addr constant [2 x i64] [i64 1, i64 478560413544]` is emitted.
7. The variable `M` is bound to this global; `len(M)` folds to `1`.

**Full function listing with annotation:**
```omscript
fn str_to_u64_fast(s:string) -> u64[] {
    var n:int = len(s);                    // comptime: 5
    var blocks:int = (n + 7) >> 3;        // comptime: 1
    var out:u64[] = array_fill(blocks, 0); // comptime: [0]
    for (i:int in 0...blocks) {            // runs once: i=0
        var base:int = i << 3;             // comptime: 0
        var x:u64 = 0;
        // Each condition is checked at compile time:
        if (base + 0 < n) { x |= u64(s[base + 0]) << 0;  }  // 'h'=104
        if (base + 1 < n) { x |= u64(s[base + 1]) << 8;  }  // 'e'=101
        if (base + 2 < n) { x |= u64(s[base + 2]) << 16; }  // 'l'=108
        if (base + 3 < n) { x |= u64(s[base + 3]) << 24; }  // 'l'=108
        if (base + 4 < n) { x |= u64(s[base + 4]) << 32; }  // 'o'=111
        if (base + 5 < n) { /* 5 < 5: skip */ }
        if (base + 6 < n) { /* skip */ }
        if (base + 7 < n) { /* skip */ }
        out[i] = x;    // out[0] = 0x6F6C6C6568 = 478560413544
    }
    return out;
}

var M:u64[] = comptime { str_to_u64_fast("hello"); };
// Emitted IR: @M = private unnamed_addr constant [2 x i64] [i64 1, i64 478560413544]
// len(M) == 1, M[0] == 478560413544 == 0x6F6C6C6568

// Usage: fast substring check at runtime
fn contains_hello(s:string) -> bool {
    if (len(s) < 5) { return false; }
    var word = u64(s[0]) | (u64(s[1]) << 8) | (u64(s[2]) << 16) |
               (u64(s[3]) << 24) | (u64(s[4]) << 32);
    return (word & 0xFFFFFFFFFF) == M[0];
}
```

#### 25.12.5 Requirements for Comptime Array Functions

For a user function to be evaluatable inside `comptime`:
1. **All arguments must be compile-time constants** (literals, `const` variables, or results of other comptime expressions).
2. **The function must be pure** — it must not call any I/O, system, or threading builtins.
3. **All control flow must be statically deterministic** — `if` conditions must evaluate to known values; loop bounds must be known constants.
4. **No pointer aliasing to external state** — the function must not read from global mutable variables.
5. **Recursion is supported** — as long as it terminates in a bounded number of steps (the evaluator has a depth limit of 10,000 steps).

Functions that call other user functions are supported as long as those callee functions also satisfy these requirements.

#### 25.12.6 `arrayReturningFunctions_` Registration

When the compiler encounters a function being called in a comptime context and determines that function returns an array, it registers the function name in the `arrayReturningFunctions_` set (a `std::unordered_set<std::string>` inside `CodeGenerator`). This registration serves several purposes:

1. **Type inference** — subsequent uses of the variable at call sites are typed as array, not integer. This affects which LLVM getelementptr offsets are generated for element access.
2. **len() folding** — if the comptime result is a known-size array, `len()` calls on the variable can fold to the constant length immediately.
3. **Downstream comptime propagation** — if the array variable is passed to another pure function in another comptime block, the evaluator knows to treat it as a constant array, not a runtime pointer.

---



## 26. Pipeline — Software-Prefetched Sequential Processing

A `pipeline` block expresses a sequence of named *stages* that execute
repeatedly.  Unlike a general-purpose `for` loop, it hides iteration details
behind a count expression (or elides them entirely for one-shot use) and
automatically applies software-prefetch and loop-interleaving hints so the
hardware can overlap stage execution with memory traffic.

### 26.1 Syntax

```
// Count form — execute stages N times
pipeline <expr> {
    stage <name> { <body> }
    ...
}

// One-shot form — execute stages once, no loop overhead
pipeline {
    stage <name> { <body> }
    ...
}
```

- `<expr>` is any expression evaluated **once** before the first iteration.
  It may be a literal, a variable, or an arbitrary sub-expression.
- At least one `stage` must be present.
- Stage names are labels only; they impose no variable scoping.

**Examples:**

```omscript
// ① Accumulate __pipeline_i five times (result = 0+1+2+3+4 = 10)
var sum = 0;
pipeline 5 {
    stage add {
        sum = sum + __pipeline_i;
    }
}

// ② Three-stage streaming transform over an array of length n
pipeline n {
    stage load    { x = a[__pipeline_i]; }
    stage compute { y = transform(x);    }
    stage store   { out[__pipeline_i] = y; }
}

// ③ Dynamic count from a function call
pipeline len(data) {
    stage process { consume(data[__pipeline_i]); }
}
```

### 26.2 The `__pipeline_i` iterator

The compiler injects a hidden signed-integer variable named `__pipeline_i`
that counts from `0` (inclusive) to `<count>` (exclusive).  It is fully
accessible inside all stage bodies:

```omscript
pipeline n {
    stage body {
        var elem = arr[__pipeline_i];   // index the array with the iterator
        print(__pipeline_i);            // print iteration number
    }
}
```

`__pipeline_i` behaves like a normal read-only variable inside the stages:
you may pass it to functions, use it in expressions, and index arrays with it.
Assigning to `__pipeline_i` is legal but has no effect on the loop counter
(the compiler updates it separately at the end of each iteration).

### 26.3 One-shot form

When no count is provided the stages execute exactly once as a straight-line
block — no loop is emitted at all.  This is useful for structured
Load→Compute→Store bursts where the caller manages iteration:

```omscript
// Caller loops; pipeline just names the phases clearly
for (i in 0...n) {
    pipeline {
        stage load    { x = raw[i]; }
        stage compute { y = process(x); }
        stage store   { out[i] = y; }
    }
}
```

### 26.4 Compiler guarantees

| Property | Details |
|---|---|
| **Execution order** | Stages always execute in declaration order, sequentially.  No concurrency. |
| **Count evaluation** | `<expr>` is evaluated once before any stage runs. |
| **Zero count** | A count ≤ 0 means the body never executes (same as `times 0`). |
| **Auto-prefetch** | At `-O1` or above, the compiler identifies every `arr[__pipeline_i]` access and emits `llvm.prefetch` for `arr[__pipeline_i + D]` where `D = max(8, 2 × number_of_stages)`. |
| **Loop metadata** | The loop back-edge carries `llvm.loop.mustprogress`, `llvm.loop.vectorize.enable`, `llvm.loop.interleave.count(nstages)`, and `llvm.loop.pipeline.initiationinterval=1`. |
| **Iterator type** | `__pipeline_i` is `i64` (the default integer type). |

### 26.5 When to use `pipeline` vs `times` / `for`

| Construct | Use when |
|---|---|
| `times N { ... }` | Body is a single logical unit; no index needed. |
| `for (i in 0...N) { ... }` | You need explicit index control, custom step, or `break`/`continue`. |
| `pipeline N { stage load {...} stage compute {...} stage store {...} }` | Body decomposes naturally into distinct phases (load/compute/store) *and* you want the compiler to automatically insert prefetches and interleave hints. |
| `pipeline { stage ... }` | One-shot structured burst; caller handles the outer loop. |

> **Tip:** `pipeline` is most beneficial when stage bodies contain array reads
> separated from where the read values are used.  The compiler can then hide
> memory latency by prefetching the next iteration's data while the current
> iteration's compute stages run.

---

## 27. Integer Type-Cast Reference

This appendix provides a complete, unambiguous reference for all 9 integer type-cast functions introduced in OmScript 4.1.1. These are function-call-style coercions handled specially by the compiler — they are not user-callable functions in the normal sense, and they produce no runtime overhead for identity casts. See §5.7 and §10.14 for the conceptual introduction.

### 27.1 Overview Table

| Cast | Width | Behavior | LLVM IR | Identity? | Comptime Folds? |
|---|---|---|---|---|---|
| `u64(x)` | 64-bit unsigned | Pass-through | (none) | ✓ | ✓ |
| `i64(x)` | 64-bit signed | Pass-through | (none) | ✓ | ✓ |
| `int(x)` | 64-bit signed | Pass-through | (none) | ✓ | ✓ |
| `uint(x)` | 64-bit unsigned | Pass-through | (none) | ✓ | ✓ |
| `u32(x)` | 32-bit unsigned | Zero-extend (mask) | `and i64 %x, 4294967295` | — | ✓ |
| `i32(x)` | 32-bit signed | Truncate + sign-extend | `trunc`, `sext` | — | ✓ |
| `u16(x)` | 16-bit unsigned | Zero-extend (mask) | `and i64 %x, 65535` | — | ✓ |
| `i16(x)` | 16-bit signed | Truncate + sign-extend | `trunc`, `sext` | — | ✓ |
| `u8(x)` | 8-bit unsigned | Zero-extend (mask) | `and i64 %x, 255` | — | ✓ |
| `i8(x)` | 8-bit signed | Truncate + sign-extend | `trunc`, `sext` | — | ✓ |
| `bool(x)` | 1-bit | Normalize 0/1 | `icmp ne`, `zext` | — | ✓ |

### 27.2 Identity Casts: `u64`, `i64`, `int`, `uint`

These four casts are complete no-ops at runtime. The value is passed through unchanged. They exist to:
- **Document intent** in code that mixes signed and unsigned interpretations
- **Suppress type-mismatch warnings** from future type-stricter analysis tools
- **Enable zero-cost `u64(s[i])` patterns** where the programmer needs to assert "treat this byte as unsigned before shifting"

```omscript
var x:int = 42;
var a = u64(x);    // same as x; type annotation documents "unsigned 64-bit"
var b = i64(x);    // same as x
var c = int(x);    // same as x
var d = uint(x);   // same as x

// Comptime:
const K:u64 = comptime { u64(100); };    // 100
const M:int = comptime { int(0xFF);  };  // 255
```

**LLVM IR generated:** none (the value is used directly).

### 27.3 `u32(x)` — Unsigned 32-Bit Mask

Masks the input to the lower 32 bits, zero-extending the upper 32 bits.

**Formula:** `x & 0xFFFFFFFF`

**LLVM IR:** `%result = and i64 %x, 4294967295`

```omscript
u32(0)                    // 0
u32(4294967295)           // 4294967295 (0xFFFFFFFF)
u32(4294967296)           // 0           (2^32 wraps to 0)
u32(4294967297)           // 1
u32(-1)                   // 4294967295  (0xFFFFFFFFFFFFFFFF & 0xFFFFFFFF)
u32(-2147483648)          // 2147483648  (0x80000000)
u32(0x1_2345_6789)        // 0x23456789 (591751049)
```

**Comptime:**
```omscript
const A = u32(0x1_FFFF_FFFF);  // 4294967295
const B = u32(-1);             // 4294967295
const C = u32(1_000_000_000_000); // 3567587328 (1e12 & 0xFFFFFFFF)
```

### 27.4 `i32(x)` — Signed 32-Bit Truncate

Truncates to 32 bits and sign-extends the result back to 64 bits. This preserves the two's-complement signed value if it fits in `[-2^31, 2^31-1]`.

**Formula:** `sext(trunc(x, i32), i64)`

**LLVM IR:** `%t = trunc i64 %x to i32; %result = sext i32 %t to i64`

```omscript
i32(0)                  // 0
i32(2147483647)         // 2147483647   (INT32_MAX — unchanged)
i32(2147483648)         // -2147483648  (INT32_MAX+1 wraps to INT32_MIN)
i32(-2147483648)        // -2147483648  (INT32_MIN — unchanged)
i32(-2147483649)        // 2147483647   (INT32_MIN-1 wraps to INT32_MAX)
i32(-1)                 // -1
i32(0xFFFFFFFF)         // -1           (0xFFFFFFFF = 4294967295 trunc to i32 = -1)
i32(0x80000000)         // -2147483648
```

**Use case:** Ensuring that arithmetic produces the same result as 32-bit C `int` operations. For example, when implementing a hash function specified in terms of 32-bit integer arithmetic:
```omscript
fn fnv1a_32(data:u8[]) -> u32 {
    var h:u32 = u32(2166136261);
    for (b in data) {
        h = i32(h ^ u8(b));       // XOR byte, treat result as signed 32-bit
        h = i32(h * 16777619);    // FNV prime multiplication, 32-bit wrapped
    }
    return u32(h);
}
```

### 27.5 `u16(x)` — Unsigned 16-Bit Mask

Masks the input to the lower 16 bits.

**Formula:** `x & 0xFFFF`

**LLVM IR:** `%result = and i64 %x, 65535`

```omscript
u16(0)          // 0
u16(65535)      // 65535 (0xFFFF)
u16(65536)      // 0
u16(-1)         // 65535
u16(0xABCD)     // 43981
u16(0x1ABCD)    // 43981 (upper bits discarded)
```

### 27.6 `i16(x)` — Signed 16-Bit Truncate

Truncates to 16 bits and sign-extends back to 64 bits.

**Formula:** `sext(trunc(x, i16), i64)`

```omscript
i16(32767)     // 32767   (INT16_MAX)
i16(32768)     // -32768  (INT16_MAX+1 → INT16_MIN)
i16(-32768)    // -32768
i16(-1)        // -1
i16(0xFFFF)    // -1
i16(0x8000)    // -32768
```

### 27.7 `u8(x)` — Unsigned 8-Bit Mask

Masks the input to the lower 8 bits. **This is the most frequently used cast**, especially for treating string bytes as unsigned values before packing them into wider integers.

**Formula:** `x & 0xFF`

**LLVM IR:** `%result = and i64 %x, 255`

```omscript
u8(0)       // 0
u8(127)     // 127
u8(128)     // 128
u8(255)     // 255
u8(256)     // 0
u8(-1)      // 255   (0xFF)
u8(-128)    // 128   (0x80)
u8('A')     // 65
u8(300)     // 44    (300 & 0xFF = 0x2C)
```

**Classic usage — zero-extend string bytes for bitwise operations:**
```omscript
// Reading bytes from a string for hashing or pattern matching:
fn hash_string(s:string) -> u64 {
    var h:u64 = 14695981039346656037;  // FNV offset basis
    var n = len(s);
    for (i:int in 0...n) {
        h ^= u64(s[i]);          // u64() zero-extends the byte
        h *= 1099511628211;      // FNV prime
    }
    return h;
}

// Without u64(s[i]), the byte would be sign-extended for values ≥ 128,
// which would corrupt the upper 56 bits of h during XOR.
```

**Comptime folding:**
```omscript
const BYTE_300 = u8(300);    // 44
const BYTE_NEG = u8(-1);     // 255
const BYTE_FF  = u8(0xFF00); // 0

// In a loop inside comptime — generates a lookup table:
fn byte_parity_table() -> int[] {
    var t:int[] = array_fill(256, 0);
    for (i:int in 0...256) {
        var x = i;
        x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
        t[i] = u8(x) & 1;
    }
    return t;
}
const PARITY:int[] = comptime { byte_parity_table(); };
```

### 27.8 `i8(x)` — Signed 8-Bit Truncate

Truncates to 8 bits and sign-extends back to 64 bits.

**Formula:** `sext(trunc(x, i8), i64)`

```omscript
i8(127)     // 127   (INT8_MAX)
i8(128)     // -128  (INT8_MAX+1 → INT8_MIN)
i8(255)     // -1
i8(-128)    // -128
i8(-1)      // -1
i8(0)       // 0
i8(200)     // -56   (200 = 0xC8; 0xC8 as signed i8 = -56)
```

**Use case:** Implementing algorithms that process signed bytes (e.g., audio samples, temperature deltas):
```omscript
fn apply_signed_delta(data:int[], deltas:int[], n:int) -> int[] {
    var out:int[] = array_fill(n, 0);
    for (i:int in 0...n) {
        out[i] = data[i] + i8(deltas[i]);  // treat delta as signed 8-bit
    }
    return out;
}
```

### 27.9 `bool(x)` — Boolean Normalization

Normalizes any integer to exactly 0 or 1. Equivalent to `(x != 0) ? 1 : 0`.

**Formula:** `(x != 0) ? 1 : 0`

**LLVM IR:** `%cmp = icmp ne i64 %x, 0; %result = zext i1 %cmp to i64`

```omscript
bool(0)          // 0
bool(1)          // 1
bool(-1)         // 1
bool(42)         // 1
bool(0x80000000) // 1
bool(0)          // 0

// Useful for converting comparison results to explicit 0/1 integers:
var flag = bool(x > threshold);    // 0 or 1, not an LLVM i1
var count_nonzero = bool(a) + bool(b) + bool(c);  // sum of booleans
```

**Comptime:**
```omscript
const FLAGS:int[] = comptime {
    var t:int[] = array_fill(8, 0);
    for (i:int in 0...8) {
        t[i] = bool(i % 3);   // 0,1,1,0,1,1,0,1
    }
    t;
};
```

### 27.10 Compile-Time Folding: Complete Rules

The following rules govern when a type-cast expression is folded at compile time:

1. **Literal argument:** Any call like `u8(255)`, `i32(-1)`, `bool(0)` with a literal integer argument is **always folded** by the constant evaluator, regardless of context.

2. **Inside `comptime {}` blocks:** All 9 casts are recognized by `evalConstBuiltin` and fold immediately when the argument is a known constant (which inside `comptime`, all variables are).

3. **After `const` declaration:** When the argument is a `const` variable, downstream uses may also fold:
   ```omscript
   const N = 300;
   const B = u8(N);    // folds to 44
   ```

4. **After `comptime` declaration:** Variables initialized from `comptime` blocks are treated as constants for downstream folding purposes.

5. **In `OPTMAX` blocks:** The OPTMAX evaluator applies the same constant folding rules as `comptime`.

6. **Chaining:** Type-cast calls can be chained, and each step folds:
   ```omscript
   const X = i32(u8(1000) + 200);  // u8(1000)=232, +200=432, i32(432)=432
   ```

### 27.11 Interaction with the Type System

Since all OmScript integers share the same LLVM type (`i64`), the type-cast functions do not change the storage type of a variable. They are purely **value-transforming operations** that create a new `i64` value with different bits.

This means:
- `var x:u32 = u32(y)` — the annotation `:u32` is informational only; the underlying value is still `i64`, but the `u32()` cast ensures the upper 32 bits are zero.
- `var x:i8 = i8(y)` — the annotation `:i8` is informational; the actual value is `i64` with bits 8–63 set to the sign of bit 7.
- Arithmetic on a `u8`-cast value is still 64-bit arithmetic — you must re-apply `u8()` after arithmetic to clamp back to 8 bits if needed.

```omscript
var a:u8 = u8(200);    // a = 200
var b:u8 = u8(200);    // b = 200
var c = a + b;         // c = 400 (64-bit addition — NOT 144!)
var d:u8 = u8(a + b);  // d = 144 (400 & 0xFF = 0x90 = 144)
```

This is intentional — it matches C's behavior where arithmetic on narrow integers is promoted to `int`.

### 27.12 Complete Examples

**Example 1: Building a Bloom Filter Bitmask at Compile Time**
```omscript
fn build_bloom_mask(keys:string[]) -> u64[] {
    var mask:u64[] = array_fill(4, 0);  // 256-bit bloom filter (4 × 64-bit words)
    for (k in keys) {
        var h:u64 = 14695981039346656037;
        for (i:int in 0...len(k)) {
            h ^= u64(k[i]);
            h *= 1099511628211;
        }
        var bucket = u64(h >> 58);           // bits 63-58 select word (0-3)
        var bit    = u64(1) << u64(h & 63); // bit within word
        mask[u8(bucket) >> 6] |= bit;       // u8() ensures 0-3 index
    }
    return mask;
}

const BLOOM:u64[] = comptime { build_bloom_mask(["foo", "bar", "baz"]); };
```

**Example 2: CRC-8 Table Generation at Compile Time**
```omscript
fn make_crc8_table() -> u8[] {
    var t:u8[] = array_fill(256, 0);
    for (i:int in 0...256) {
        var crc:int = i;
        for (j:int in 0...8) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x8C;
            } else {
                crc >>= 1;
            }
        }
        t[i] = u8(crc);   // clamp to 8-bit result
    }
    return t;
}

const CRC8_TABLE:u8[] = comptime { make_crc8_table(); };
// 256-entry lookup table, fully computed at compile time
// Emitted as: @CRC8_TABLE = private unnamed_addr constant [257 x i64] [i64 256, ...]
```

**Example 3: Using All Type Casts in a Single Expression**
```omscript
fn pack_rgb(r:int, g:int, b:int) -> u32 {
    // Clamp each channel to 8 bits and pack into a 32-bit RGB value
    return u32(u8(r)) | (u32(u8(g)) << 8) | (u32(u8(b)) << 16);
}

// At compile time:
const RED   = comptime { pack_rgb(255, 0, 0); };   // 0x0000FF
const GREEN = comptime { pack_rgb(0, 255, 0); };   // 0x00FF00
const BLUE  = comptime { pack_rgb(0, 0, 255); };   // 0xFF0000
const WHITE = comptime { pack_rgb(255, 255, 255); }; // 0xFFFFFF
```

---

*End of OmScript Language Reference — Version 4.1.1*

---

## 28. CF-CTRE — Cross-Function Compile-Time Reasoning Engine

> **Available since:** v4.1.1  
> **Trigger:** O1+ (automatic), `@pure`, `@const_eval`  
> **Source:** `include/cfctre.h`, `src/cfctre.cpp`

CF-CTRE is OmScript's **deterministic SSA-semantics compile-time interpreter**.  It sits between the AST pre-analysis passes and LLVM IR generation and executes pure functions **across function-call boundaries** at compile time.  Results are memoised, pipeline structure is preserved, and SIMD lane semantics are respected.

---

### 28.1 Purpose and Position in Pipeline

#### 28.1.1 Goals

CF-CTRE solves a fundamental limitation of classical constant folding: it is stopped by function calls.  Classical folding can evaluate `abs(min(3, 7))` but not:

```omscript
fn encode(x:int) -> int { return x * 6364136223846793005 + 1442695040888963407; }
fn chain(x:int)  -> int { return encode(encode(encode(x))); }

const K = comptime { chain(42); }   // ← CF-CTRE evaluates all three calls
```

Classical folding would refuse this because `encode` is a user function.  CF-CTRE descends into `encode`, executes it three times (with memoisation so the second and third calls reuse the first result), and substitutes `K` with the fully computed constant — **zero runtime code emitted**.

#### 28.1.2 Placement

```
Frontend → SSA Builder → CF-CTRE → OPTMAX → LLVM IR → Backend
```

More precisely, within the compiler's `generateProgram()` function:

```
preAnalyzeStringTypes()
preAnalyzeArrayTypes()
analyzeConstantReturnValues()      ← zero-arg pure function results
autoDetectConstEvalFunctions()     ← purity detection
inferFunctionEffects()             ← side-effect classification
runCFCTRE()                        ← ← ← CF-CTRE PHASE ← ← ←
generateFunction() × N             ← LLVM IR emission
runOptimizationPasses()
```

CF-CTRE runs **after** all AST analysis so it has the maximum available constant information, and **before** LLVM IR emission so its results are visible to the code generator.

---

### 28.2 Core Object Model

CF-CTRE defines five core value/memory types:

#### 28.2.1 `CTValue` — Compile-Time Value

A tagged union representing any scalar or compound value that CF-CTRE can compute:

| Kind | Underlying C++ type | Description |
|---|---|---|
| `CONCRETE_U64` | `uint64_t` | Unsigned 64-bit integer |
| `CONCRETE_I64` | `int64_t` | Signed 64-bit integer (default OmScript integer) |
| `CONCRETE_F64` | `double` | IEEE-754 double-precision float |
| `CONCRETE_BOOL` | `bool` | Boolean (true / false) |
| `CONCRETE_STRING` | `std::string` | Compile-time string value |
| `CONCRETE_ARRAY` | `CTArrayHandle` (uint64) | Opaque handle into CTHeap |
| `UNINITIALIZED` | — | Sentinel (not yet computed) |

Key constructors:

```cpp
CTValue::fromI64(int64_t)        // integer (most common)
CTValue::fromU64(uint64_t)       // unsigned integer
CTValue::fromF64(double)         // float
CTValue::fromBool(bool)          // boolean
CTValue::fromString(std::string) // string
CTValue::fromArray(CTArrayHandle)// array handle
CTValue::uninit()                // UNINITIALIZED sentinel
```

Key accessors:

```cpp
bool isInt()     → true for U64, I64, or BOOL
bool isString()  → true for STRING
bool isArray()   → true for ARRAY
int64_t asI64()  → coerces U64/BOOL/F64 to signed int64
std::string asStr()
CTArrayHandle asArr()
```

`memoHash()` computes a stable 64-bit hash of the value for use as memoisation keys.  For arrays, it hashes element-by-element through CTHeap.

#### 28.2.2 `CTHeap` — Deterministic Compile-Time Memory

CF-CTRE's memory model uses a **handle-based heap** — no raw pointers exist at compile time.

```
struct CTHeap {
    map<CTArrayHandle, CTArray> arrays_
    uint64_t next_handle_
}

struct CTArray {
    uint64_t len
    vector<CTValue> data
}
```

Rules:
- `alloc(n)` → allocates a new array of `n` UNINITIALIZED elements and returns its handle.
- `store(handle, index, value)` → mutates `heap[handle][index]` in place.
- `load(handle, index)` → returns the current value at that index.
- `length(handle)` → returns `n` (the length stored at the header).
- Handles are monotonically increasing 64-bit integers (deterministic — no ASLR, no raw pointers).
- Arrays are mutable from within CF-CTRE execution (unlike LLVM global constants).
- The heap is per-`CTEngine` instance; it is destroyed when the compilation unit finishes.

#### 28.2.3 `CTFrame` — Function Execution Context

Each active function invocation owns a `CTFrame`:

```
struct CTFrame {
    FunctionDecl* fn          // AST function being executed
    map<string, CTValue> locals  // variable bindings for this call
    CTHeap* heap              // pointer to shared CTHeap
    CTValue returnValue       // set when a return is executed
    bool didReturn            // signals early return
    bool didBreak             // signals break from loop
    bool doContinue           // signals continue in loop
    CTValue lastBareExpr      // implicit return value (last expression stmt)
}
```

Locals shadow outer scopes correctly: entering a `{` block pushes a copy of the current locals; exiting the block restores the outer copy.  This matches OmScript's block-scoping rules exactly.

#### 28.2.4 `CTGraph` — Interprocedural Call Graph

CF-CTRE builds a lightweight call graph from the AST:

```
struct CTCallEdge { string caller; string callee; }
struct CTGraph    { set<string> nodes; vector<CTCallEdge> edges; }
```

The call graph is used for:
- Topological ordering during pre-evaluation (leaf functions first).
- Detecting recursive cycles (prevents infinite expansion).
- Specialization: clustering call sites by constant-argument shape.

#### 28.2.5 `CTEngine` — Main Engine

`CTEngine` is the single public class.  One instance exists per `CodeGenerator`, allocated in `runCFCTRE()`:

```cpp
class CTEngine {
public:
    void registerGlobalConst(const string& name, CTValue v);
    void registerEnumConst(const string& name, int64_t v);
    void runPass(Program* program);        // whole-program analysis
    bool isPure(const string& fnName) const;
    optional<CTValue> executeFunction(const string& name, vector<CTValue> args);
    optional<CTValue> evalComptimeBlock(BlockStmt* body);
    CTHeap& heap();
    vector<CTValue> extractArray(CTArrayHandle h) const;
    struct Stats { ... };
    const Stats& stats() const;
};
```

---

### 28.3 Function Eligibility Rules

A function is eligible for CF-CTRE execution if **all** of the following hold:

#### 28.3.1 Required Conditions

| Condition | How checked |
|---|---|
| Annotated `@pure` **or** auto-detected as pure | Fixed-point purity analysis (see §28.3.2) |
| All call-site arguments are CT-known | Each arg reduces to a `CTValue` (not `UNINITIALIZED`) |
| No external I/O | No `print`, `println`, `input_line`, file operations in body |
| No non-deterministic operations | No `rand()`, `time()`, `srand()` |
| No unsafe pointer escape | No raw pointer arithmetic, no FFI calls |
| Recursion depth ≤ 128 | `kMaxDepth` limit |
| Total instruction budget not exceeded | `kMaxInstructions` = 10 000 000 per compilation unit |

#### 28.3.2 `@const_eval` Override

Annotating a function `@const_eval` **forces** eligibility regardless of the purity analysis result.  Use this when you know a function is safe to execute at compile time but the static analysis would otherwise conservatively reject it.

```omscript
@const_eval
fn my_hash(s:string) -> int {
    var h:int = 0xcbf29ce484222325;
    for (i:int in 0...len(s)) {
        h ^= s[i];
        h *= 0x100000001b3;
    }
    return h;
}

const HASH_OF_HELLO = comptime { my_hash("hello"); }
```

#### 28.3.3 Purity Analysis — Fixed-Point Algorithm

CF-CTRE uses a whole-program fixed-point analysis to detect pure functions without explicit annotations:

```
Phase 1: Seed — mark all @pure and @const_eval functions as pure.
Phase 2: Iterate until stable:
    For each unmarked function F:
        If body contains only pure operations → mark F pure
Phase 3: Pure operations in a function body:
    - Arithmetic / logical / bitwise / shift / comparison expressions
    - Calls to other pure functions (already in the pure set)
    - Array read / write (no external I/O)
    - String operations (pure builtins only)
    - if / else / switch / for / foreach / while / do-while
    - Variable declarations and assignments
    - Type casts
    NOT pure:
    - print / println / input_line
    - file open / read / write / close / append
    - rand() / srand() / time()
    - try / catch (potential runtime error with exit side-effect)
    - Calls to unknown or impure functions
```

Mutual recursion is handled conservatively: if A calls B and B calls A and neither is marked pure by the end of the fixed-point loop, both remain impure.

---

### 28.4 Execution Model

#### 28.4.1 Entry Points

CF-CTRE is triggered from two places in the code generator:

1. **`comptime {}` blocks** — when the code generator encounters a `ComptimeExpr` node, it calls `ctEngine_->evalComptimeBlock(body)` before falling back to the legacy `tryConstEvalFull`.

2. **Call-site constant folding** — when all arguments to a call are compile-time constants and the callee is `isPure()`, the code generator calls `ctEngine_->executeFunction(callee, ctArgs)` before falling back to `tryConstEvalFull`.

Both paths produce a `CTValue` that is then converted to LLVM IR constants.

#### 28.4.2 `evalComptimeBlock` Algorithm

```
evalComptimeBlock(block):
    frame = new CTFrame(nullptr)   // top-level: no enclosing function
    frame.heap = &engine.heap_
    for each stmt in block.statements:
        evalStmt(frame, stmt)
        if frame.didReturn:
            return frame.returnValue
    return frame.lastBareExpr     // implicit return
```

The **implicit return** rule: the last *bare expression statement* (an `ExprStmt` whose expression is not a call with side effects) becomes the block's return value even without an explicit `return`.  This matches OmScript block-expression semantics.

#### 28.4.3 `executeFunction` Algorithm

```
executeFunction(fnName, args):
    1. Build memo key: hash(fnName, hash(args[0]), hash(args[1]), ...)
    2. If key in memo_cache_: return memo_cache_[key]
    3. If depth_ >= kMaxDepth: return nullopt   (depth guard)
    4. Look up FunctionDecl* for fnName
    5. frame = new CTFrame(fn)
       frame.heap = &engine.heap_
    6. Bind args to parameter names in frame.locals
    7. depth_++
    8. for each stmt in fn.body.statements:
           evalStmt(frame, stmt)
           if frame.didReturn: break
    9. depth_--
    10. result = frame.returnValue (or lastBareExpr if no explicit return)
    11. memo_cache_[key] = result
    12. return result
```

#### 28.4.4 Depth and Fuel Guards

| Guard | Value | Behaviour on violation |
|---|---|---|
| `kMaxDepth` | 128 | Returns `nullopt` (falls back to runtime) |
| `kMaxInstructions` | 10 000 000 | Returns `nullopt` (falls back to runtime) |
| Heap size | unlimited (system RAM) | No hard limit; controlled by `kMaxInstructions` |

When CF-CTRE returns `nullopt`, the code generator silently falls back to the legacy constant evaluator or emits a normal runtime call.  CF-CTRE never causes a compile error by failing to evaluate.

---

### 28.5 Instruction Semantics

CF-CTRE evaluates OmScript AST nodes rather than a separate SSA IR (it is an *AST-level interpreter with SSA-like semantics*).

#### 28.5.1 Arithmetic Operations

All arithmetic wraps at 64 bits (two's-complement):

| OmScript op | CF-CTRE behaviour |
|---|---|
| `a + b` | `int64_t(a) + int64_t(b)` with 64-bit wraparound |
| `a - b` | `int64_t(a) - int64_t(b)` with 64-bit wraparound |
| `a * b` | `int64_t(a) * int64_t(b)` with 64-bit wraparound |
| `a / b` | Signed integer division; `b == 0` → returns `nullopt` |
| `a % b` | Signed remainder; `b == 0` → returns `nullopt` |
| `-a` | Unary negate |
| `a ^ b` | Bitwise XOR |
| `a & b` | Bitwise AND |
| `a \| b` | Bitwise OR |
| `~a` | Bitwise NOT |
| `a << b` | Left shift (64-bit) |
| `a >> b` | Arithmetic right shift (64-bit) |

**Float operations** (`CONCRETE_F64`): `+`, `-`, `*`, `/` with IEEE-754 `double` semantics.  Integer and float values are automatically coerced when mixed (int → double).

**String concatenation** (`+` on two strings): immediate string concatenation.

#### 28.5.2 Comparison and Logical Operations

| OmScript op | CF-CTRE result |
|---|---|
| `a == b` | `CTValue::fromBool(a == b)` |
| `a != b` | `CTValue::fromBool(a != b)` |
| `a < b` | `CTValue::fromBool(a < b)` (signed) |
| `a <= b` | `CTValue::fromBool(a <= b)` |
| `a > b` | `CTValue::fromBool(a > b)` |
| `a >= b` | `CTValue::fromBool(a >= b)` |
| `a && b` | Short-circuit AND (b not evaluated if a is false) |
| `a \|\| b` | Short-circuit OR (b not evaluated if a is true) |
| `!a` | Logical NOT |

#### 28.5.3 Memory Operations

**Array allocation:**
```omscript
var arr:int[] = array_fill(n, value)
```
CF-CTRE:
1. Evaluates `n` → must be a non-negative `CTValue` integer.
2. `handle = heap_.alloc(n)` — allocates `n` slots initialized to `value`.
3. `frame.locals["arr"] = CTValue::fromArray(handle)`.

**Array element load:**
```omscript
x = arr[i]
```
CF-CTRE:
1. Evaluates `arr` → must be `CONCRETE_ARRAY`.
2. Evaluates `i` → must be `CONCRETE_I64` or `CONCRETE_U64`.
3. Bounds-checks: if `i < 0 || i >= heap_.length(handle)` → returns `nullopt`.
4. Returns `heap_.load(handle, i)`.

**Array element store:**
```omscript
arr[i] = x
```
CF-CTRE:
1. Resolves `arr` to its `CTArrayHandle` from `frame.locals`.
2. Evaluates `i` and `x`.
3. Bounds-checks.
4. `heap_.store(handle, i, x)`.

**Array length:**
```omscript
len(arr)
```
Returns `CTValue::fromI64(heap_.length(handle))`.

**String indexing:**
```omscript
s[i]
```
Returns the character code (integer) at position `i`.  Bounds-checked.

#### 28.5.4 Control Flow

**`if` / `else`:**
```
eval condition → must be CTValue (bool or int ≠ 0)
if truthy: execute then-branch
else:      execute else-branch (if present)
```

**`for` range loop:**
```omscript
for (i:int in start...end) { body }
```
CF-CTRE:
1. Evaluates `start` and `end`.
2. Iterates `i = start, start+1, ..., end-1` (exclusive upper bound; `0...3` → 0,1,2).
3. Uses inclusive upper bound for `...=` (0...=3 → 0,1,2,3).
4. Even if `end <= start` (zero iterations): enters the loop construct but executes zero body iterations.  The loop variable `i` is bound for each iteration.
5. `break` sets `frame.didBreak = true` and stops iteration.
6. `continue` sets `frame.doContinue = true`, advances to next iteration.

**`foreach` / `for` collection:**
```omscript
foreach (v in arr) { body }
```
Iterates each element of a CT-known array.

**`while` / `do-while` / `until`:**
Condition re-evaluated each iteration.  Body executes while condition is truthy.  Protected by the instruction budget (`kMaxInstructions`).

**`switch`:**
```
eval discriminant
for each case:
    if case.isDefault: remember as fallback
    else: eval case values; if any matches discriminant, execute body, break
if no match found and default exists: execute default body
```

**`return`:**
Sets `frame.returnValue = value` and `frame.didReturn = true`.  Any enclosing loop terminates.

**`break` / `continue`:**
Set the corresponding signal flag on the frame.  Loops check these flags after each body execution.

---

### 28.6 Cross-Function Call Rules

#### 28.6.1 Call Resolution

When CF-CTRE encounters a function call expression:

```
call f(a, b):
    ctArgs = [eval(a), eval(b)]
    if any ctArg is UNINITIALIZED → cannot evaluate → return nullopt
    if isPure(f) and all ctArgs are CT-known:
        return executeFunction(f, ctArgs)     ← INLINE EXECUTION
    else:
        return nullopt                         ← runtime call
```

#### 28.6.2 Inline Execution

CF-CTRE does **not** perform textual inlining (AST substitution).  Instead it:
1. Allocates a fresh `CTFrame` for `f`.
2. Binds `ctArgs` to `f`'s parameter names in the new frame.
3. Executes `f`'s body statement-by-statement.
4. Returns the computed `CTValue`.

This means the original AST of `f` is unchanged; only the *result* is folded.

#### 28.6.3 Memoisation

Before executing any function call, CF-CTRE checks the memo cache:

```
key = (fnName, hash(arg0), hash(arg1), ...)
if key in memo_cache_: return memo_cache_[key]
```

After successful execution, the result is stored:
```
memo_cache_[key] = result
```

For array results, the CTHeap snapshot is included in the memo value — the entire heap state produced by the function is preserved.  Subsequent calls with the same arguments reuse the pre-computed array handle.

**Memo key construction** is deterministic and argument-order-sensitive:
```
key = fnName XOR rotl(hash(arg0), 17) XOR rotl(hash(arg1), 31) XOR ...
```

#### 28.6.4 Recursion

Recursive functions are supported up to `kMaxDepth = 128` frames.  The depth counter is incremented on entry and decremented on exit.  A function that would exceed the limit returns `nullopt` instead (the call site falls through to a runtime call).

Mutually recursive functions that are provably pure (e.g. each only calls the other with strictly decreasing arguments — a Fibonacci pair) will be correctly evaluated as long as the recursion terminates within the depth limit.

---

### 28.7 Pipeline Semantics and SIMD Tile Execution

CF-CTRE has special handling for OmScript's `pipeline` statement (see §26) to preserve software-pipeline structure and honour the SIMD vector model.

#### 28.7.1 SIMD Vector Model

```
kSIMDLaneWidth = 8   // u64 lanes per tile
```

CF-CTRE treats each loop body as operating on a **vector tile of 8 elements**.  For a range `0...n`:

- Number of full tiles: `n / 8`
- Remainder tile width: `n % 8` (may be 0..7)
- If `n < 8`: exactly **one tile** is executed with `n` active lanes and `8 - n` masked (zero-padded) lanes.

This matches the hardware SIMD execution model that the OmScript pipeline statement targets (256-bit AVX2 vectors of 8 × i32, or 4 × i64).

#### 28.7.2 Pipeline Stage Execution

For a `pipeline N { stage A { ... } stage B { ... } }` construct:

```
for each iteration i in 0...N:
    execute stage A (with __pipeline_i = i)
    execute stage B (with __pipeline_i = i)
```

Stage state (local variables) persists **across stage boundaries** within a single iteration.  This models the software-pipeline's register-passing semantics: a value computed in stage A is visible in stage B for the same iteration.

#### 28.7.3 Tile Execution API

Internally, CF-CTRE exposes:

```
execute_tile(base, width, mask):
    for lane in 0...width:
        if mask[lane]:
            execute body with iterator = base + lane
        else:
            execute body with iterator = 0 (zero-padded)
```

Even a partial final tile always executes one tile.

#### 28.7.4 Invariant

CF-CTRE **never drops pipeline structure** unless explicitly permitted.  If a pipeline body contains an impure operation that prevents full CT evaluation, CF-CTRE returns `nullopt` for that pipeline and the code generator emits it as a normal runtime loop.

---

### 28.8 Specialization Engine

When a function is called with **all-constant literal arguments**, CF-CTRE can record a *specialization*:

```
f("hello")  →  specialized_f__hello result cached
```

Rules:
1. Specialization keys are (function name, stable arg hash).
2. Results are memoised — repeated calls to `f("hello")` anywhere in the program share the same cached result.
3. Specialization is transparent to the programmer; it is purely a compiler optimization.
4. The original function is unmodified in the IR.

The specialization cache is the same as the memo cache; no separate data structure is needed.

---

### 28.9 Output and Integration Contract

#### 28.9.1 What CF-CTRE Produces

After `runPass()`:

| Output | Description |
|---|---|
| `isPure(fn)` → `bool` | Fast O(1) per-function purity query |
| `executeFunction(fn, args)` → `optional<CTValue>` | On-demand memoised CT evaluation |
| `evalComptimeBlock(block)` → `optional<CTValue>` | Block-level CT evaluation |
| Back-propagated constants | New entries in `constIntReturnFunctions_` / `constStringReturnFunctions_` |

#### 28.9.2 Back-Propagation into Legacy Fold Tables

After `runPass()`, `runCFCTRE()` iterates all zero-arg pure functions and queries CF-CTRE for their pre-evaluated results.  Any integer or string results are inserted into the legacy `constIntReturnFunctions_` / `constStringReturnFunctions_` maps, making them visible to `tryFoldExprToConst` and `tryConstEvalFull` without those functions needing to know about CF-CTRE.

#### 28.9.3 Integration Contract

| CF-CTRE **will** do | CF-CTRE **will not** do |
|---|---|
| Evaluate pure functions deterministically | Emit machine code |
| Return constant `CTValue` or `nullopt` | Run OS processes |
| Preserve pipeline metadata | Perform nondeterministic operations |
| Memoise results across call sites | Drop pipeline structure without fallback |
| Back-propagate to legacy tables | Modify the AST (read-only) |
| Build a call graph | Produce diagnostic errors on evaluation failure |

When CF-CTRE cannot evaluate something, it always returns `nullopt` and the compiler continues normally.

---

### 28.10 Performance Characteristics

| Property | Value |
|---|---|
| Time complexity | O(number of CT-executed instructions) |
| Memo cache | Hash map; O(1) amortized lookup and insert |
| Depth limit | 128 frames |
| Instruction budget | 10 000 000 instructions per compilation unit |
| Heap overhead | ~64 bytes per allocated array element (CTValue) |
| Call graph build | O(nodes + edges) |
| Purity fixed-point | Converges in O(functions × depth of call graph) iterations |

For typical programs (hundreds of functions, most with ≤ 10 calls each), the CF-CTRE phase completes in microseconds.  For programs with deeply computed constant tables (e.g. `comptime { build_lut(256); }`) it may take milliseconds but still never emits runtime code for the evaluated portion.

Verbose output (enabled by `-v` or `--verbose`) shows CF-CTRE statistics at the end of the phase:

```
[cfctre] Pass complete: 47 functions registered, 12 pure,
         3 calls memoised, 2 arrays allocated
```

---

### 28.11 Programmer-Visible Effects

#### 28.11.1 When CF-CTRE Fires

CF-CTRE evaluation is triggered when:

1. **`comptime { expr }` block** — always tried first.  If CF-CTRE can evaluate it, the result replaces the block; otherwise `tryConstEvalFull` is tried; if both fail, a compile error is issued.

2. **Call to a pure function with all-constant arguments** — tried silently at any call site in the program.  If CF-CTRE evaluates it, the call is replaced with the constant.  If not, normal code is generated.

3. **Zero-argument pure functions** — pre-evaluated during `runPass()` and stored as constants.  Any reference to a zero-arg pure function result is already folded before code generation begins.

#### 28.11.2 `-O0` Behaviour

At `-O0`, CF-CTRE is **disabled** (the entire phase is skipped).  All `comptime {}` blocks still fall through to `tryConstEvalFull` (which handles simple arithmetic and string folding).  This preserves predictable non-optimized code generation.

#### 28.11.3 Error Handling

CF-CTRE never aborts compilation.  All evaluation failures silently produce `nullopt`.  The only way CF-CTRE causes a compile error is indirectly: if a `comptime {}` block fails **both** CF-CTRE and `tryConstEvalFull`, the code generator emits the error `"comptime block could not be evaluated at compile time"` — not CF-CTRE itself.

#### 28.11.4 Diagnostic Output

When `--verbose` / `-v` is passed:

```
[cfctre] Pass complete: 47 functions registered, 12 pure,
         3 calls memoised, 2 arrays allocated
```

Fields:
- `functions registered` — functions seen during `runPass()`.
- `pure` — functions confirmed pure by the analysis.
- `calls memoised` — call sites whose result was cached (includes pre-evaluation of zero-arg functions).
- `arrays allocated` — CT heap arrays created (may be freed after use).

---

### 28.12 Worked Examples

#### 28.12.1 Simple Cross-Function Evaluation

```omscript
@pure
fn square(x:int) -> int { return x * x; }

@pure
fn sum_of_squares(a:int, b:int) -> int {
    return square(a) + square(b);
}

// CF-CTRE evaluates sum_of_squares(3, 4) by:
//   1. executeFunction("square", [3]) → 9  (memoised)
//   2. executeFunction("square", [4]) → 16 (new)
//   3. 9 + 16 → 25
const RESULT = comptime { sum_of_squares(3, 4); }
// RESULT = 25, zero runtime code
```

#### 28.12.2 Array Build at Compile Time

```omscript
@pure
fn make_powers_of_two(n:int) -> int[] {
    var out:int[] = array_fill(n, 0);
    for (i:int in 0...n) {
        out[i] = 1 << i;
    }
    return out;
}

// CF-CTRE fully evaluates make_powers_of_two(8):
//   Allocates CTHeap array of 8 elements
//   Stores 1, 2, 4, 8, 16, 32, 64, 128
//   Returns handle → converted to LLVM global constant [9 × i64] { 8, 1, 2, 4, 8, 16, 32, 64, 128 }
var POW2:int[] = comptime { make_powers_of_two(8); }
// POW2 is a compile-time global constant — no heap allocation at runtime
```

#### 28.12.3 Memoisation with Repeated Calls

```omscript
@pure
fn fib(n:int) -> int {
    if (n <= 1) { return n; }
    return fib(n-1) + fib(n-2);
}

// Without memoisation: 2^30 recursive calls for fib(30)
// With CF-CTRE memoisation: each (fib, n) computed once and cached
const F30 = comptime { fib(30); }   // evaluates in O(n) memoised calls
const F29 = comptime { fib(29); }   // cached — instant
```

#### 28.12.4 Pipeline Constant Pre-Computation

```omscript
@pure
fn build_sbox(n:int) -> int[] {
    var s:int[] = array_fill(n, 0);
    for (i:int in 0...n) {
        // AES-like S-box step (simplified for illustration)
        s[i] = (i * 0x1F + 0x63) & 0xFF;
    }
    return s;
}

// S-box computed entirely at compile time — embedded as a global constant
const SBOX:int[] = comptime { build_sbox(256); }

// Pipeline using the compile-time S-box
pipeline 8 {
    stage substitute {
        // All references to SBOX are reads from a .rodata global — no heap
        var byte:int = input[__pipeline_i];
        output[__pipeline_i] = SBOX[byte];
    }
}
```

#### 28.12.5 Specialization

```omscript
@pure
fn hash_string(s:string, seed:int) -> int {
    var h:int = seed;
    for (i:int in 0...len(s)) {
        h ^= s[i];
        h *= 0x100000001b3;
    }
    return h;
}

// All three calls are evaluated at compile time by CF-CTRE.
// The specialization cache contains:
//   ("hash_string", hash("hello"), hash(0xcbf29ce484222325)) → <result1>
//   ("hash_string", hash("world"), hash(0xcbf29ce484222325)) → <result2>
//   ("hash_string", hash("hello"), hash(42))                 → <result3>
const H1 = comptime { hash_string("hello", 0xcbf29ce484222325); }
const H2 = comptime { hash_string("world", 0xcbf29ce484222325); }
const H3 = comptime { hash_string("hello", 42); }
```

---

