# OmScript Language Reference

> **Version:** 3.7.0
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
17. [Memory Semantics](#17-memory-semantics)
18. [OPTMAX Blocks](#18-optmax-blocks)
19. [Built-in Functions](#19-built-in-functions)
20. [Concurrency](#20-concurrency)
21. [File I/O](#21-file-io)
22. [Lambda Expressions](#22-lambda-expressions)
23. [Import System](#23-import-system)
24. [Compiler CLI Reference](#24-compiler-cli-reference)
25. [Advanced Optimization Features](#25-advanced-optimization-features)

---

## 1. Overview

OmScript is a statically-compiled, dynamically-typed language with optional type annotations. It compiles through LLVM to native machine code. Key features:

- **Dynamic typing with optional annotations** — variables are untyped by default; annotations enable advanced optimizations and SIMD types
- **LLVM backend** — ahead-of-time compilation to native binaries
- **Three-stage optimizer** — e-graph equality saturation, superoptimizer, and hardware graph optimization engine (HGOE)
- **C-compatible performance** — designed to match C performance with high-level syntax

---

## 2. Compilation Pipeline

```
Source (.om)
  → Preprocessor     (macro expansion, conditional compilation)
  → Lexer            (tokenization)
  → Parser           (AST construction)
  → Code Generator   (LLVM IR generation)
  → E-Graph          (equality saturation, algebraic identities)
  → Superoptimizer   (idiom recognition, branch-to-select)
  → HGOE             (hardware-aware scheduling, FMA fusion)
  → LLVM             (standard optimization passes)
  → Native Binary
```

---

## 3. Lexical Structure

### 3.1 Comments

```
// Single-line comment
/* Block comment */
```

Block comments nest correctly and must be terminated.

### 3.2 Keywords

```
fn        return    if        else      elif      unless
while     do        for       foreach   forever   loop
repeat    until     times     with      var       const
register  break     continue  in        true      false
null      switch    case      default   try       catch
throw     enum      struct    import    move      invalidate
borrow    prefetch  likely    unlikely  when      guard
defer     swap
```

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

### 6.2 Constants

```
const PI = 3.14159
const MAX:int = 100
```

Constants must be initialized at declaration and cannot be reassigned. The compiler performs constant folding at compile time.

### 6.3 Register Variables

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
| `@pure` | Mark as pure (no side effects) |
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

Prefetched parameters must be explicitly `invalidate`d before the function returns.

### 7.8 Tail Calls

Self-recursive calls in tail position are automatically converted to jumps (guaranteed tail call elimination).

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
    case 2:
    case 3: { ... }    // fallthrough: matches 2 or 3
    default: { ... }
}
```

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

Defers execution of a statement to the end of the enclosing block:

```
fn open_file() {
    defer println("done");
    // ... work ...
    // "done" prints here, at block exit
}
```

### 8.8 with

Scoped variable bindings that are lexically scoped to a block:

```
with (var f = open_file()) {
    // f is available here
}
```

Desugars to a block with variable declarations.

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
| `char_at(s, i)` | Character at index `i` (as string) |
| `str_eq(s1, s2)` | String equality (returns 1/0) |
| `str_concat(s1, s2)` | Concatenate two strings |
| `str_substr(s, start, len)` | Substring of length `len` starting at `start` |
| `str_upper(s)` | Uppercase |
| `str_lower(s)` | Lowercase |
| `str_find(s, sub)` | Find substring (returns index or -1) |
| `str_contains(s, sub)` | Contains substring (1/0) |
| `str_index_of(s, sub)` | First index of substring (-1 if not found) |
| `str_replace(s, old, new)` | Replace first occurrence |
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
| `map_get(m, key)` | Get value for key |
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

## 17. Memory Semantics

OmScript provides explicit move/borrow/invalidate semantics for performance-critical code.

### 17.1 move

```
var a = [1, 2, 3]
var b = move a     // b owns the array; a is invalidated
```

After `move`, accessing the source variable is undefined behavior.

### 17.2 borrow

```
fn process(data) {
    var view = borrow data
    // view shares data's storage; data is not moved
}
```

### 17.3 invalidate

```
invalidate a;      // explicitly marks variable 'a' as invalid
```

Required before returning from a function that received a `@prefetch` parameter — the parameter must be invalidated before return.

### 17.4 prefetch

The `prefetch` statement issues software prefetch instructions and optionally declares a variable:

```
prefetch arr;              // prefetch 'arr' into L1 cache
prefetch+128 arr;          // prefetch 'arr' and 128 bytes ahead
prefetch data:int = compute();    // declare 'data' and prefetch it
```

Prefetched variables must be `invalidate`d before the function returns.

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
| `typeof(x)` | Returns string name of type |
| `len(x)` | Length of array or string |
| `to_int(x)` | Convert to integer |
| `to_float(x)` | Convert to float |
| `to_string(x)` | Convert number to string |
| `assert(cond)` | Runtime assertion (aborts on failure) |

### 19.6 Time / System

| Function | Description |
|---|---|
| `time()` | Current time in milliseconds |
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
| `thread_create(fn, arg)` | Create a new thread running `fn(arg)` |
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
| `-fegraph` | on | E-graph equality saturation |
| `-fsuperopt` | on | Superoptimizer pass |
| `-fhgoe` | on | Hardware Graph Optimization Engine |

### 24.6 Superoptimizer Level

```
-fsuperopt=0   Disabled
-fsuperopt=1   Basic idiom recognition
-fsuperopt=2   Default (idiom + algebraic simplification)
-fsuperopt=3   Full (+ enumerative synthesis)
```

### 24.7 Debugging and Info

```
-g / --debug   Emit debug information
-V / --verbose Verbose compiler output
-static        Link statically
-s / --strip   Strip debug symbols from output
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

Enabled by default at O1+; disable with `-fno-egraph`.

### 25.2 Superoptimizer

A post-LLVM optimization pass that applies:
- **Idiom recognition**: popcount, bswap, rotate, min/max, abs, etc.
- **Algebraic simplification**: algebraic identities on LLVM IR instructions
- **Branch-to-select**: converts simple diamond CFGs to `select` instructions
- **Enumerative synthesis** (level 3 only): enumerates short instruction sequences

Enabled by default at O1+; configure with `-fsuperopt=<level>`.

### 25.3 Hardware Graph Optimization Engine (HGOE)

Activated when `-march` or `-mtune` is provided (including `native`). Builds a structural model of the target CPU microarchitecture and:
- Maps operations to hardware execution units
- Inserts FMA (fused multiply-add) instructions where profitable
- Applies hardware-specific strength reductions
- Uses hardware-aware cost models for scheduling decisions

### 25.4 OPTMAX Blocks

See [Section 18](#18-optmax-blocks).

### 25.5 Profile Guidance (PGO)

The build system includes `benchmark_pgo.sh` for profile-guided optimization runs. PGO is coordinated externally via the build scripts — there is no language-level PGO syntax.
