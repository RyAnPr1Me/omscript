# OmScript Language Reference

> **Version:** 3.7.0
> **Compiler:** `omsc` — OmScript Compiler
> **Backend:** LLVM 18 · Ahead-of-Time Compilation
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
8. [Operators](#8-operators)
9. [Control Flow](#9-control-flow)
10. [Arrays](#10-arrays)
11. [Strings](#11-strings)
12. [Structs](#12-structs)
13. [Enums](#13-enums)
14. [Maps (Dictionaries)](#14-maps-dictionaries)
15. [Ownership System](#15-ownership-system)
16. [Module Imports](#16-module-imports)
17. [File I/O](#17-file-io)
18. [Concurrency](#18-concurrency)
19. [Built-in Functions](#19-built-in-functions)
20. [Optimization Directives](#20-optimization-directives)
21. [OPTMAX Directive](#21-optmax-directive)
22. [E-Graph and Superoptimizer](#22-e-graph-and-superoptimizer)
23. [Hardware Graph Optimization Engine](#23-hardware-graph-optimization-engine)
24. [CLI Reference](#24-cli-reference)
25. [Grammar Summary](#25-grammar-summary)
---

## 1. Overview

OmScript is a **statically compiled, C-like programming language** with dynamic typing and an LLVM-based AOT compiler. Key verified properties:

- **Dynamic typing** -- Variables do not require type annotations. Optional type annotations (`var x:int = 0`) are supported for documentation and are **required** inside `OPTMAX` blocks.
- **Ahead-of-Time (AOT) compilation** -- Source compiles to native machine code via LLVM. There is no runtime JIT.
- **Memory management** -- Heap memory is managed with `malloc`/`free`. There is no garbage collector and no runtime reference counting.
- **Aggressive optimization** -- Four optimization levels (O0-O3) plus an OPTMAX directive, an E-graph equality saturation pass, a superoptimizer pass, and a hardware graph optimization engine.
- **Ownership hints** -- Optional `move`, `invalidate`, and `borrow` keywords provide compile-time use-after-move/use-after-invalidate detection and improve LLVM alias analysis.
- **Comprehensive standard library** -- Math, arrays, strings, maps, file I/O, threading, and character classification, all compiled directly to LLVM IR.
- **Structs** -- Named record types with field access, mutation, field-level optimization hints, and operator overloading.
- **Enums** -- Named integer constant groups with auto-increment.
- **Module system** -- `import` statements with circular-import detection.
- **Preprocessor** -- `#define`, conditional compilation, `#error`, `#warning`, `#assert`, `#require`, `#counter`, and predefined macros.

---

## 2. Compilation Pipeline

```
Source (.om)
     |
     v
 Preprocessor      Macro expansion, conditional compilation
     |
     v
 Lexer             Tokenization
     |
     v
 Parser            AST construction
     |
     v
 E-Graph (O2+)     Equality-saturation rewrites on AST (600+ rules)
     |
     v
 Codegen           AST to LLVM IR
     |
     v
 LLVM Optimizer    Standard LLVM pass manager (level dependent)
     |
     v
 Superoptimizer    Custom IR peephole + idiom passes (O2+)
     |
     v
 HGOE              Hardware-graph instruction scheduling (-march/-mtune)
     |
     v
 LLVM Backend      Instruction selection, register allocation
     |
     v
 Native binary / object file
```

---

## 3. Lexical Structure

### 3.1 Comments

```omscript
// Single-line comment

/* Block comment
   spanning multiple lines */
```

### 3.2 Integer Literals

```omscript
42          // decimal
0xFF        // hexadecimal (case-insensitive)
0o17        // octal
0b1010      // binary
1_000_000   // underscores are ignored
```

All integer literals must fit in a 64-bit signed integer. Out-of-range values are a lex error.

### 3.3 Float Literals

```omscript
3.14
2.0
1_000.5
```

IEEE 754 double precision. A `.` followed by another `.` (range operator `..` or `...`) is not
treated as a decimal point.

### 3.4 String Literals

**Regular string** — escape sequences are processed:

```omscript
"hello\nworld"
```

Supported escapes: `\n` `\t` `\r` `\b` `\f` `\v` `\\` `\"` `\xHH`

Embedded null bytes (`\0`, `\x00`) are rejected at lex time with an error.

**Multi-line string** — no escape processing; whitespace and newlines are preserved exactly:

```
"""line one
line two"""
```

**Interpolated string** — expressions inside `{}` are evaluated at runtime and converted to strings:

```omscript
var s = $"hello {name}, value = {n + 1}";
```

Literal braces inside an interpolated string are written as `\{` and `\}`.

### 3.5 Identifiers

Start with a letter or `_`, followed by letters, digits, or `_`.

### 3.6 Keywords

```
fn       return   if       else     elif     unless
while    do       until    for      foreach  forever
loop     repeat   times    with     break    continue
var      const    switch   case     default
try      catch    throw    enum     struct   import
move     invalidate borrow  prefetch
likely   unlikely register  defer    guard    when
swap     in       true     false    null
```

The tokens `OPTMAX=:` and `OPTMAX!:` are scanned as single units (not regular keywords).

---

## 4. Preprocessor

Runs before lexing. Directives start with `#` on a line (leading whitespace is allowed).

### 4.1 Macro Definition

```omscript
#define NAME value
#define DOUBLE(x) x * 2     // function-like: no space between name and '('
#undef NAME
```

### 4.2 Conditional Compilation

```omscript
#ifdef  NAME
#ifndef NAME
#if     expr
#elif   expr
#else
#endif
```

Expressions support: integers, macro names, `defined(NAME)`, arithmetic `+ - * / %`,
comparisons `== != < <= > >=`, logical `&& || !`, and parentheses.

### 4.3 Diagnostics

```omscript
#error   "message"          // compile-time error -- aborts compilation
#warning "message"          // warning -- compilation continues
#info    "message"          // informational
#assert  expr "message"     // abort if expr evaluates to 0
```

### 4.4 Version Requirement

```omscript
#require "3.7.0"    // error if compiler version is older than 3.7.0
```

### 4.5 Counter Macro

```omscript
#counter MY_ID     // MY_ID expands to 0, 1, 2, ... on successive uses
```

### 4.6 Predefined Macros

| Macro | Value |
|-------|-------|
| `__VERSION__` | Compiler version string (e.g. `"3.7.0"`) |
| `__OS__`      | `"linux"`, `"macos"`, or `"windows"` |
| `__ARCH__`    | `"x86_64"`, `"aarch64"`, `"arm"`, or `"unknown"` |

---

## 5. Types and Values

OmScript is dynamically typed at the source level. IR representation:

| Source concept | IR representation |
|----------------|-------------------|
| Integer | `i64` |
| Float | `double` |
| Boolean | `i64` (1 = true, 0 = false) |
| `null` / `false` | `i64` value 0 |
| `true` | `i64` value 1 |
| String | `i8*` pointer to null-terminated bytes |
| Array | `i64*` pointer: `[length, elem0, elem1, ...]` |
| Struct | `i64*` pointer to heap block of fields |
| Map | `i64*` pointer to internal hash-map structure |

### 5.1 Optional Type Annotations

Advisory only -- no effect on code generation outside OPTMAX blocks.

```omscript
var x: int = 0
var s: string = "hi"
var arr: int[] = [1, 2, 3]
fn f(a: i64, b: i64) -> i64 { return a + b; }
```

Valid annotation names: `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `int`, `float`,
`double`, `bool`, `string`, `dict`, any struct name, array forms (`type[]`, `type[][]`), and
reference form (`&type`).

---

## 6. Variables and Constants

```omscript
var x = 10;
const y = 20;
var z: int = 30;

// Multi-variable in one statement
var a = 1, b = 2, c = 3;

// Array destructuring
var [first, second] = arr;
var [a, _, c] = arr;     // '_' skips an element

// Register hint: keep variable in a CPU register (mem2reg hint)
register var k = 0;
register const k = 0;
```

Variables declared without an initializer have the value 0.

**Destructuring** desugars to a hidden temp plus indexed reads:

```omscript
var [a, b] = get_pair();
// equivalent to:
//   var __tmp = get_pair();
//   var a = __tmp[0];
//   var b = __tmp[1];
```

---

## 7. Functions

### 7.1 Basic Declaration

```omscript
fn add(a, b) {
    return a + b;
}
```

All functions are top-level. Forward references work across the file and imported files.

### 7.2 Expression-Body Shorthand

```omscript
fn square(x) = x * x;
// same as: fn square(x) { return x * x; }
```

### 7.3 Type Annotations and Return Type

```omscript
fn clamp(x: i64, lo: i64, hi: i64) -> i64 {
    return max(lo, min(x, hi));
}
```

### 7.4 Generic Type Parameters

```omscript
fn identity<T>(x: T) -> T { return x; }
```

Type parameters are annotation-only -- erased at the IR level.

### 7.5 Default Parameters

```omscript
fn greet(name, greeting = "Hello") {
    println(greeting + " " + name);
    return 0;
}
```

Default values must be integer, float, or string literals. Parameters with defaults must follow
parameters without defaults.

### 7.6 Function Annotations

One or more annotations may appear directly before `fn`:

```omscript
@inline fn fast(x) { ... }
@noinline @cold fn error_handler(code) { ... }
```

| Annotation | Effect |
|------------|--------|
| `@inline` | Suggest inlining (`alwaysinline` at O2+) |
| `@noinline` | Prevent inlining |
| `@cold` | Rarely executed |
| `@hot` | Frequently executed |
| `@pure` | No side effects (`readonly` + `willreturn`) |
| `@noreturn` | Function never returns |
| `@static` | Internal linkage |
| `@flatten` | Inline all callees |
| `@unroll` | Aggressively unroll loops |
| `@nounroll` | Disable loop unrolling |
| `@restrict` | All pointer params are `noalias` |
| `@noalias` | Same as `@restrict` on functions |
| `@vectorize` | Enable SIMD vectorization for all loops |
| `@novectorize` | Disable SIMD vectorization |
| `@parallel` | Enable auto-parallelization |
| `@noparallel` | Disable auto-parallelization |
| `@minsize` | Optimize for code size |
| `@optnone` | Disable all optimizations |
| `@nounwind` | No C++ exceptions |
| `@const_eval` | Evaluate at compile time when all args are constants |

`@optnone` and `@inline` are mutually exclusive; a warning is emitted and `@inline` is ignored.

### 7.7 Parameter Annotation

```omscript
fn process(@prefetch data, count) { ... }
```

`@prefetch` emits `llvm.prefetch` at function entry for that parameter's value.

### 7.8 Method-Call Syntax

```omscript
obj.method(a, b)
// desugars to: method(obj, a, b)
```

No object system. Purely syntactic sugar with zero runtime overhead.

### 7.9 Lambda Expressions

```omscript
var double = |x| x * 2;
var add    = |a, b| a + b;
var noop   = || 0;            // zero-parameter lambda
```

Lambdas desugar to named top-level functions (`__lambda_N`) at parse time and are returned as
string literals (function names). They can be passed to `array_map`, `array_filter`,
`array_reduce`, `array_any`, `array_every`, and `array_find`.

Optional type annotations on parameters are accepted: `|x:int| x * 2`.

### 7.10 Pipe Operator

```omscript
arr |> sort |> reverse
// equivalent to: reverse(sort(arr))
```

Passes the left-hand value as the first argument to the named function on the right.

---

## 8. Operators

### 8.1 Arithmetic

| Operator | Description |
|----------|-------------|
| `+` | Addition; string concatenation if either operand is a string |
| `-` | Subtraction |
| `*` | Multiplication; string repetition if left is string and right is integer |
| `/` | Division |
| `%` | Modulo |
| `**` | Exponentiation (right-associative) |
| `-x` | Unary negation |

### 8.2 Comparison

`==`, `!=`, `<`, `<=`, `>`, `>=`

Strings: `==`/`!=` compare contents via `strcmp`; `<`/`<=`/`>`/`>=` compare lexicographically.

### 8.3 Logical (Short-Circuit)

`&&`, `||`, `!`

### 8.4 Bitwise

`&`, `|`, `^`, `~`, `<<`, `>>`

### 8.5 Null Coalescing

```omscript
x ?? fallback    // x if x != 0, else fallback
x ??= fallback   // x = x ?? fallback
```

### 8.6 Compound Assignment

`+=`, `-=`, `*=`, `/=`, `%=`, `**=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `??=`

Work on variables, array elements (`arr[i] += 1`), and struct fields (`s.x += 1`).

### 8.7 Increment / Decrement

`x++`, `x--` (postfix), `++x`, `--x` (prefix). Work on variables and array elements.

### 8.8 Ternary

```omscript
cond ? then_expr : else_expr    // right-associative
```

### 8.9 Spread

```omscript
[1, ...arr, 2]    // expand arr inline in an array literal
```

### 8.10 Operator Precedence (high to low)

| Level | Operators |
|-------|-----------|
| Postfix | `()` `[]` `.` `++` `--` |
| Unary | `-` `!` `~` `&` `++` `--` `move` |
| Exponent | `**` (right-associative) |
| Multiply | `*` `/` `%` |
| Add | `+` `-` |
| Shift | `<<` `>>` |
| Compare | `<` `<=` `>` `>=` |
| Equality | `==` `!=` |
| Bitwise AND | `&` |
| Bitwise XOR | `^` |
| Bitwise OR | `\|` |
| Logical AND | `&&` |
| Logical OR | `\|\|` |
| Null coalesce | `??` |
| Ternary | `? :` |
| Pipe | `\|>` |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `**=` `&=` `\|=` `^=` `<<=` `>>=` `??=` |

---

## 9. Control Flow

### 9.1 if / elif / else / unless

```omscript
if (cond) {
    ...
} elif (cond2) {
    ...
} else {
    ...
}

unless (cond) { ... }    // desugars to: if (!cond) { ... }
```

`elif` is a first-class keyword. Branch prediction hints:

```omscript
likely   if (common_path)  { ... }
unlikely if (rare_path)    { ... }
```

These set LLVM branch-weight metadata on the conditional branch.

### 9.2 while / until

```omscript
while (cond)  { ... }
until (cond)  { ... }    // while (!cond) { ... }
```

### 9.3 do-while / do-until

```omscript
do { ... } while (cond);
do { ... } until (cond);    // do { ... } while (!cond)
```

### 9.4 for (range-based)

```omscript
for (i in 0...10)              { ... }    // 0 <= i < 10, step +1
for (i in 0..10)               { ... }    // same (..)
for (i in 0...10...2)          { ... }    // step 2
for (i in 0...10 step 2)       { ... }    // step keyword
for (i in 10 downto 1)         { ... }    // 10 >= i > 1, step -1
for (i in 10 downto 1 step 2)  { ... }    // step -2
for (i:u32 in 0...n)           { ... }    // typed iterator
```

The loop continues while `i < upper` (ascending) or `i > lower` (descending with negative step).

### 9.5 for-each / foreach

```omscript
for (x in arr)            { ... }
foreach item in arr       { ... }
foreach (item in arr)     { ... }

// Indexed variants:
for (i, x in arr)         { ... }    // i = index, x = element
foreach (i, item in arr)  { ... }
```

Both desugar to a range for-loop over indices.

### 9.6 loop

```omscript
loop          { ... }    // infinite
loop N        { ... }    // N times
loop (N)      { ... }    // same
```

### 9.7 repeat

```omscript
repeat N       { ... }
repeat (N)     { ... }
repeat { ... } until (cond);    // post-test loop (do-while !cond)
```

### 9.8 times

```omscript
times N        { ... }
times (N)      { ... }
```

### 9.9 forever

```omscript
forever { ... }    // infinite loop
```

### 9.10 switch

```omscript
switch (expr) {
    case 1: { ... break; }
    case 2, 3: { ... break; }    // multi-value case
    default: { ... }
}
```

### 9.11 when

```omscript
when (expr) {
    1, 2, 3 => { ... }
    4       => { ... }
    _       => { ... }    // default arm
}
```

Desugars to `switch`. Each arm body is one statement. Optional trailing commas between arms.

### 9.12 break / continue

`break` exits the innermost loop. `continue` jumps to the next iteration of the innermost loop.

### 9.13 return

```omscript
return expr;
return;    // returns 0
```

### 9.14 guard

```omscript
guard (cond) else { return -1; }
// desugars to: if (!cond) { return -1; }
```

### 9.15 defer

```omscript
defer stmt;
defer { ... }
```

Executes at block exit. Multiple defers in the same block execute LIFO.

### 9.16 with

```omscript
with (var x = expr) { ... }
with (var a = e1, var b = e2) { ... }
with (const k = 5) { ... }
```

Desugars to a block containing the declarations followed by the body.

### 9.17 swap

```omscript
swap a, b;            // exchange a and b
swap a, b, c;         // circular: a<-b, b<-c, c<-old_a
```

Operands must be simple variable names.

### 9.18 try / catch / throw

```omscript
try {
    if (bad) throw 42;
    risky_fn();
} catch (err) {
    // err holds the thrown integer value
}
```

Implemented via an error-flag variable and conditional branches -- not C++ exceptions. Nested
try/catch blocks work correctly.

---

## 10. Arrays

### 10.1 Literals

```omscript
var arr = [1, 2, 3];
var copy = [...arr];
var extended = [0, ...arr, 4];
```

Heap-allocated. Memory layout: `[length_i64, elem0_i64, elem1_i64, ...]`.

### 10.2 Indexing

```omscript
var v = arr[i];      // bounds-checked read
arr[i] = value;      // bounds-checked write
arr[i] += 1;         // compound assignment
```

Out-of-bounds access prints an error message and exits the program.

### 10.3 Array Built-ins

| Function | Description |
|----------|-------------|
| `len(arr)` | Number of elements |
| `push(arr, val)` | New array with val appended |
| `pop(arr)` | New array with last element removed |
| `sort(arr)` | Sorted copy |
| `reverse(arr)` | Reversed copy |
| `sum(arr)` | Sum of all elements |
| `array_product(arr)` | Product of all elements |
| `array_fill(n, val)` | Array of n copies of val |
| `array_copy(arr)` | Shallow copy |
| `array_concat(a, b)` | Concatenate two arrays |
| `array_slice(arr, start, end)` | Sub-array [start, end) |
| `array_remove(arr, i)` | Remove element at index i |
| `array_insert(arr, i, val)` | Insert val at index i |
| `array_last(arr)` | Last element (error if empty) |
| `array_map(arr, fn)` | Apply fn to each element |
| `array_filter(arr, fn)` | Keep elements where fn(elem) != 0 |
| `array_reduce(arr, fn, init)` | Fold left: fn(acc, elem) |
| `array_contains(arr, val)` | 1 if val is present |
| `index_of(arr, val)` | First index of val, or -1 |
| `array_min(arr)` | Minimum element |
| `array_max(arr)` | Maximum element |
| `array_any(arr, fn)` | 1 if any element satisfies fn |
| `array_every(arr, fn)` | 1 if all elements satisfy fn |
| `array_find(arr, fn)` | First element satisfying fn, or 0 |
| `array_count(arr, fn)` | Count elements satisfying fn |

---

## 11. Strings

Heap-allocated null-terminated byte arrays. String literals are global constants. Most string
operations allocate new strings.

### 11.1 Operations

```omscript
var t = s + " world";    // concatenation
var r = s * 3;           // repetition -> "sss"
var c = s[2];            // character code at index 2 (integer)
s[2] = 108;              // write character in place
```

`==`/`!=` compare contents. `<`/`<=`/`>`/`>=` compare lexicographically.

### 11.2 String Built-ins

| Function | Description |
|----------|-------------|
| `len(s)` / `str_len(s)` | Byte length |
| `char_at(s, i)` | Character code at index i |
| `str_eq(a, b)` | 1 if equal |
| `str_concat(a, b)` | Concatenate |
| `str_substr(s, start, len)` | Substring |
| `str_find(s, sub)` / `str_index_of(s, sub)` | First occurrence index, or -1 |
| `str_contains(s, sub)` | 1 if found |
| `str_starts_with(s, prefix)` | 1 if starts with prefix |
| `str_ends_with(s, suffix)` | 1 if ends with suffix |
| `str_replace(s, old, new)` | Replace first occurrence |
| `str_upper(s)` | Uppercase copy |
| `str_lower(s)` | Lowercase copy |
| `str_trim(s)` | Strip leading/trailing whitespace |
| `str_repeat(s, n)` | Repeat n times |
| `str_reverse(s)` | Reverse characters |
| `str_split(s, delim)` | Split on delimiter, return array |
| `str_chars(s)` | Array of character codes |
| `str_join(arr, sep)` | Join string array with separator |
| `str_count(s, sub)` | Count non-overlapping occurrences |
| `str_pad_left(s, width, ch)` | Left-pad to width |
| `str_pad_right(s, width, ch)` | Right-pad to width |
| `to_char(n)` | Integer to single-character string |
| `char_code(s)` | ASCII code of first character |
| `is_alpha(n)` | 1 if code is a letter |
| `is_digit(n)` | 1 if code is a decimal digit |
| `to_string(n)` / `number_to_string(n)` | Integer or float to string |
| `to_int(s)` / `str_to_int(s)` | Parse as integer |
| `to_float(s)` / `str_to_float(s)` | Parse as double |
| `string_to_number(s)` | Parse as integer |

---

## 12. Structs

### 12.1 Declaration

```omscript
struct Point {
    x,
    y
}
```

Trailing comma is allowed.

### 12.2 Literal Construction

```omscript
var p = Point { x: 10, y: 20 };
```

Fields may be given in any order.

### 12.3 Access and Mutation

```omscript
var v = p.x;
p.y = 30;
p.x += 5;
```

### 12.4 Typed Fields

```omscript
struct Vec3 { float x, float y, float z }
// or equivalently:
struct Vec3 { x: float, y: float, z: float }
```

### 12.5 Field Attributes

Optimization hints preceding the field name:

```omscript
struct Particle {
    hot float x,
    cold int debug_id,
    noalias data,
    immut mass,
    move owner,
    align(64) buffer,
    range(0, 255) level
}
```

| Attribute | Description |
|-----------|-------------|
| `hot` | Frequently accessed |
| `cold` | Rarely accessed |
| `noalias` | Pointer does not alias other fields |
| `immut` | Never modified after construction |
| `move` | Participates in ownership transfer |
| `align(N)` | Align to N bytes |
| `range(min, max)` | Value in [min, max] -- enables range_metadata on loads |

### 12.6 Operator Overloading

```omscript
struct Vec2 {
    x, y,
    fn operator+(other: Vec2) -> Vec2 {
        return Vec2 { x: self.x + other.x, y: self.y + other.y };
    }
}
```

Supported: `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`.

`self` is the left-hand operand. Operator implementations are automatically `@inline`.

---

## 13. Enums

```omscript
enum Color {
    RED,           // 0
    GREEN = 10,    // 10
    BLUE           // 11
}
```

Members auto-increment from the previous value. Access as `EnumName_MEMBER`:

```omscript
var c = Color_GREEN;   // 10
if (c == Color_BLUE) { ... }
```

Enum values are plain integers at runtime.

---

## 14. Maps (Dictionaries)

Open-addressing integer-to-integer hash tables.

### 14.1 Dict Literal Syntax

```omscript
var m = {"key": 42, "other": 99};
```

Compiles to direct stores (no search loops). A literal key lookup on a literal dict is
constant-folded at compile time.

### 14.2 Map Operations

```omscript
var m = map_new();
m = map_set(m, key, value);
var v = map_get(m, key, default_value);
var found = map_has(m, key);         // 1 or 0
m = map_remove(m, key);
var sz = map_size(m);
var keys = map_keys(m);              // integer array of keys
var vals = map_values(m);            // integer array of values
```

Keys and values are 64-bit integers. String keys are stored as their pointer value cast to integer.

---

## 15. Ownership System

Optional. Normal code does not need it.

### 15.1 move

```omscript
move var dst = src;
var x = move src;
return move src;
```

Using the source variable after `move` is a compile-time error.

### 15.2 invalidate

```omscript
invalidate x;    // mark x as dead; subsequent use is a compile-time error
```

### 15.3 borrow

```omscript
borrow var ref = &source;
borrow j:u32 = &source;
```

Non-owning reference hint for alias analysis. `&` here is advisory, not an actual pointer
operation.

### 15.4 prefetch Statement

```omscript
prefetch name;
prefetch+64 name;
prefetch hot name;
prefetch immut name;
prefetch var name = expr;
prefetch+128 hot var buf = data;
```

Emits `llvm.prefetch`. `+N` prefetches N bytes ahead of the variable's value.

---

## 16. Module Imports

```omscript
import "utils.om";
import "path/to/module";    // .om appended automatically if missing
```

Top-level only. All declarations from the imported file are merged. Circular and duplicate
imports are silently skipped.

---

## 17. File I/O

```omscript
var content = file_read("path.txt");     // "" on error
file_write("out.txt", "data");           // overwrite
file_append("log.txt", "line\n");        // append
var ok = file_exists("path.txt");        // 1 or 0
```

---

## 18. Concurrency

```omscript
var tid = thread_create("function_name");    // zero-argument function, name as string literal
thread_join(tid);

var mu = mutex_new();
mutex_lock(mu);
// ... critical section ...
mutex_unlock(mu);
mutex_destroy(mu);
```

`thread_create` accepts the name of a zero-argument OmScript function as a string literal. The
wrapper is synthesized at compile time.

---

## 19. Built-in Functions

All compile directly to LLVM IR with no dispatch overhead.

### 19.1 I/O

| Function | Description |
|----------|-------------|
| `print(val)` | Print integer, float, or string without newline |
| `println(val)` | Print with newline |
| `print_char(n)` | Print character by ASCII code |
| `write(s)` | Write string to stdout |
| `input()` | Read integer from stdin |
| `input_line()` | Read line from stdin as string (newline stripped) |

### 19.2 Math

| Function | Description |
|----------|-------------|
| `abs(x)` | Absolute value |
| `min(a, b)` | Minimum |
| `max(a, b)` | Maximum |
| `sign(x)` | -1, 0, or 1 |
| `clamp(x, lo, hi)` | Clamp to [lo, hi] |
| `pow(base, exp)` | Exponentiation |
| `sqrt(x)` | Square root |
| `floor(x)` | Floor |
| `ceil(x)` | Ceiling |
| `round(x)` | Round to nearest |
| `is_even(n)` | 1 if even |
| `is_odd(n)` | 1 if odd |
| `gcd(a, b)` | Greatest common divisor |
| `lcm(a, b)` | Least common multiple |
| `is_power_of_2(n)` | 1 if power of two |

### 19.3 Trigonometry and Transcendental

| Function | Description |
|----------|-------------|
| `sin(x)` | Sine (radians) |
| `cos(x)` | Cosine |
| `tan(x)` | Tangent |
| `asin(x)` | Arcsine |
| `acos(x)` | Arccosine |
| `atan(x)` | Arctangent |
| `atan2(y, x)` | Two-argument arctangent |
| `exp(x)` | e^x |
| `exp2(x)` | 2^x |
| `log(x)` | Natural logarithm |
| `log10(x)` | Base-10 logarithm |
| `cbrt(x)` | Cube root |
| `hypot(a, b)` | sqrt(a^2 + b^2) |
| `fma(a, b, c)` | Fused multiply-add: a*b+c |
| `copysign(x, y)` | x with sign of y |
| `min_float(a, b)` | Minimum of two floats |
| `max_float(a, b)` | Maximum of two floats |

### 19.4 Precision Floating-Point

| Function | Description |
|----------|-------------|
| `fast_add(a, b)` | Add with fast-math (allows reassociation) |
| `fast_sub(a, b)` | Subtract with fast-math |
| `fast_mul(a, b)` | Multiply with fast-math |
| `fast_div(a, b)` | Divide with fast-math |
| `precise_add(a, b)` | Add without fast-math |
| `precise_sub(a, b)` | Subtract without fast-math |
| `precise_mul(a, b)` | Multiply without fast-math |
| `precise_div(a, b)` | Divide without fast-math |

### 19.5 Bitwise / Integer Intrinsics

| Function | Description |
|----------|-------------|
| `popcount(n)` | Count set bits |
| `clz(n)` | Count leading zeros |
| `ctz(n)` | Count trailing zeros |
| `bitreverse(n)` | Reverse bit order |
| `bswap(n)` | Byte-swap (endian flip) |
| `rotate_left(n, k)` | Rotate left by k bits |
| `rotate_right(n, k)` | Rotate right by k bits |
| `saturating_add(a, b)` | Signed saturating add |
| `saturating_sub(a, b)` | Signed saturating subtract |

### 19.6 Type Utilities

| Function | Description |
|----------|-------------|
| `typeof(x)` | Returns `"int"`, `"float"`, `"string"`, `"array"`, `"map"`, or `"null"` |

### 19.7 Assertions and Hints

| Function | Description |
|----------|-------------|
| `assert(cond)` | Print error and exit if cond is 0 |
| `assume(cond)` | Tell LLVM the condition is always true (UB if false) |
| `unreachable()` | Mark unreachable (UB if executed) |
| `expect(val, likely_val)` | Branch prediction hint |

### 19.8 System

| Function | Description |
|----------|-------------|
| `exit(code)` / `exit_program(code)` | Exit with code |
| `random()` | Random integer (wraps C rand()) |
| `time()` | Seconds since Unix epoch |
| `sleep(ms)` | Sleep ms milliseconds |

### 19.9 Range Generation

| Function | Description |
|----------|-------------|
| `range(start, end)` | Array [start, ..., end-1] |
| `range_step(start, end, step)` | Array with custom step (step != 0) |

---

## 20. Optimization Directives

### 20.1 Optimization Levels

| Level | Description |
|-------|-------------|
| `-O0` | No optimization. |
| `-O1` | Basic: instruction combining, CFG simplification, mem2reg. |
| `-O2` | Standard -- **default**. Full LLVM pass manager; E-graph and Superoptimizer enabled. |
| `-O3` | Aggressive. All O2 plus additional loop and peephole passes. |
| `-Ofast` | Treated as -O3. |

### 20.2 @noalias File Directive

```omscript
@noalias
```

Placed at the top level before any function: marks all pointer parameters in every function
in the file as `noalias`.

---

## 21. OPTMAX Directive

```omscript
OPTMAX=:
fn compute(x: i64, y: i64) -> i64 {
    var result: i64 = x * y;
    return result;
}
OPTMAX!:
```

Functions between `OPTMAX=:` and `OPTMAX!:` require explicit type annotations on all parameters
and local variables, and receive the most aggressive LLVM optimization configuration. Nesting is
a parse error.

---

## 22. E-Graph and Superoptimizer

### 22.1 E-Graph Equality Saturation (O2+)

Applied to the AST before codegen. 600+ algebraic rewrite rules:

- Constant folding
- Strength reduction: `x*3 -> (x<<1)+x`, `x*15 -> (x<<4)-x`
- Algebraic identities: commutativity, associativity, distributivity
- Bitwise absorption and shift combination
- Comparison simplification

### 22.2 Superoptimizer (O2+)

Applied to LLVM IR after the standard optimization pipeline. Four passes:

1. **Idiom recognition** -- `sdiv x, pow2 -> ashr`, `x % pow2 -> and`
2. **Algebraic simplification** -- 300+ peephole rewrites
3. **Branch-to-select** -- conditional branches to `select`
4. **Synthesis** -- optimal shift+add sequences for constant multiplications

---

## 23. Hardware Graph Optimization Engine

Activated by `-march=<cpu>` or `-mtune=<cpu>`. Builds a hardware execution model and performs:

- **Instruction scheduling** -- per-basic-block list scheduler with cycle-accurate port model
- **Port-diversity** -- fills different execution units each cycle to maximize IPC
- **Register-pressure tiebreaker** -- prefers instructions that free registers
- **FMA generation** -- `fadd(fmul(a,b), c) -> fma(a,b,c)`
- **Integer strength reduction** -- `imul -> shift+add` for constant multipliers
- **Software pipelining** -- loop headers get `llvm.loop.unroll.count`, `interleave.count`, and
  `vectorize.width` metadata from the hardware resource model
- **Target attributes** -- sets `target-cpu` and `target-features` on every function

Supported targets: Skylake, Haswell, Alder Lake, Zen 3/4/5, Apple M1-M4, Neoverse N2/V2,
RISC-V, and others.

---

## 24. CLI Reference

### 24.1 Commands

```
omsc <file.om>          Compile to native executable (default)
omsc compile <file.om>  Same
omsc run <file.om>      Compile and execute
omsc check <file.om>    Parse and validate only
omsc lex <file.om>      Print token stream
omsc parse <file.om>    Print AST
omsc emit-ir <file.om>  Emit LLVM IR
omsc clean              Remove build artifacts
omsc install            Install to PATH
omsc uninstall          Remove from PATH
omsc update             Update to latest version
omsc pkg <subcommand>   Package manager (install, remove, list, search, info)
omsc version            Print version
omsc help               Print usage
```

### 24.2 Compiler Options

```
-o <file>             Output file (default: a.out)
-O0 / -O1 / -O2 / -O3 / -Ofast   Optimization level (default: -O2)
-V, --verbose         Verbose output
-q, --quiet           Suppress non-error output
--emit-obj            Emit object file only
--dry-run             Validate without writing output
--time                Show timing breakdown
```

### 24.3 Codegen Options

```
-march=<cpu>           Target CPU (default: native)
-mtune=<cpu>           Tuning CPU (default: -march value)
-flto                  Full link-time optimization
-ffast-math            Unsafe floating-point optimizations
-fvectorize            SIMD vectorization (default: on)
-funroll-loops         Loop unrolling (default: on)
-floop-optimize        Polyhedral loop optimization (default: on)
-fparallelize          Auto-parallelize loops (default: on)
-fpic                  Position-independent code (default: on)
-foptmax               OPTMAX optimization (default: on)
-fstack-protector      Stack canary protection
-fegraph               E-graph saturation (default: on at O2+)
-fsuperopt             Superoptimizer (default: on at O2+)
-fsuperopt-level=N     Superoptimizer aggressiveness 0-3 (default: 2)
-fhgoe                 Hardware graph optimization (default: on)
```

Use `-fno-<flag>` to disable any flag: e.g., `-fno-lto`, `-fno-vectorize`.

### 24.4 Linker Options

```
-static        Static linking
-s, --strip    Strip debug symbols
```

---

## 25. Grammar Summary

```
program        = top_level*

top_level      = import_stmt
               | enum_decl
               | struct_decl
               | annotation* fn_decl
               | '@noalias'

import_stmt    = 'import' STRING ';'

enum_decl      = 'enum' IDENT '{' (IDENT ('=' INTEGER)? ','?)* '}'

struct_decl    = 'struct' IDENT '{' struct_member* '}'
struct_member  = operator_overload | field_decl
operator_overload = 'fn' 'operator' op '(' IDENT (':' type_ann)? ')' ('->' type_ann)? block
field_decl     = field_attr* type_name? IDENT (':' type_ann)? ','?
field_attr     = 'hot' | 'cold' | 'noalias' | 'immut' | 'move'
               | 'align' '(' INTEGER ')' | 'range' '(' INTEGER ',' INTEGER ')'
op             = '+' | '-' | '*' | '/' | '%' | '==' | '!=' | '<' | '>' | '<=' | '>='

annotation     = '@' IDENT
fn_decl        = 'fn' IDENT ('<' IDENT (',' IDENT)* '>')?
                 '(' param_list ')' ('->' type_ann)?
                 (block | '=' expr ';')
param_list     = (param (',' param)*)?
param          = ('@prefetch')? IDENT (':' type_ann)? ('=' literal)?
type_ann       = ('&')? IDENT ('[]')*
literal        = INTEGER | FLOAT | STRING

block          = '{' stmt* '}'

stmt           = ('var' | 'const') var_decl (',' var_decl)* ';'
               | ('var' | 'const') '[' (IDENT|'_') (',' (IDENT|'_'))* ']' '=' expr ';'
               | 'register' ('var' | 'const') IDENT (':' type_ann)? ('=' expr)? ';'
               | 'move' ('var' | IDENT) IDENT '=' expr ';'
               | 'borrow' ('var' | IDENT)? IDENT (':' type_ann)? '=' expr ';'
               | 'invalidate' IDENT ';'
               | 'prefetch' ('+' INTEGER)? ('hot'|'immut')* (IDENT | 'var' IDENT (':' type_ann)? ('=' expr)?) ';'
               | ('likely'|'unlikely')? 'if' '(' expr ')' stmt ('elif' '(' expr ')' stmt)* ('else' stmt)?
               | 'unless' '(' expr ')' stmt ('else' stmt)?
               | 'while' '(' expr ')' stmt
               | 'until' '(' expr ')' stmt
               | 'do' stmt ('while'|'until') '(' expr ')' ';'
               | 'for' '(' IDENT (',' IDENT)? (':' type_ann)? 'in' expr
                     (('..'|'...') expr (('..'|'...'|'step') expr)?)? ')' stmt
               | 'for' '(' IDENT (':' type_ann)? 'in' expr 'downto' expr ('step' expr)? ')' stmt
               | 'foreach' '('? IDENT (',' IDENT)? 'in' expr ')'? stmt
               | 'loop' expr? stmt
               | 'repeat' expr? stmt | 'repeat' stmt 'until' '(' expr ')' ';'
               | 'times' '('? expr ')'? stmt
               | 'forever' stmt
               | 'switch' '(' expr ')' '{' switch_case* '}'
               | 'when' '(' expr ')' '{' when_arm* '}'
               | 'guard' '(' expr ')' 'else' stmt
               | 'defer' stmt
               | 'with' '(' with_binding (',' with_binding)* ')' stmt
               | 'swap' IDENT (',' IDENT)+ ';'
               | 'return' expr? ';'
               | 'break' ';' | 'continue' ';'
               | 'try' block 'catch' '(' IDENT ')' block
               | 'throw' expr ';'
               | block | expr ';'

var_decl       = IDENT (':' type_ann)? ('=' expr)?
with_binding   = ('var' | 'const') IDENT (':' type_ann)? ('=' expr)?
switch_case    = ('case' expr (',' expr)* | 'default') ':' stmt*
when_arm       = (expr (',' expr)* | '_') '=>' stmt ','?

expr           = assignment
assignment     = pipe (assign_op assignment)?
assign_op      = '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '**='
               | '&=' | '|=' | '^=' | '<<=' | '>>=' | '??='
pipe           = ternary ('|>' IDENT)*
ternary        = null_coal ('?' expr ':' ternary)?
null_coal      = logical_or ('??' logical_or)*
logical_or     = logical_and ('||' logical_and)*
logical_and    = bitwise_or ('&&' bitwise_or)*
bitwise_or     = bitwise_xor ('|' bitwise_xor)*
bitwise_xor    = bitwise_and ('^' bitwise_and)*
bitwise_and    = equality ('&' equality)*
equality       = comparison (('==' | '!=') comparison)*
comparison     = shift (('<' | '<=' | '>' | '>=') shift)*
shift          = addition (('<<' | '>>') addition)*
addition       = multiply (('+' | '-') multiply)*
multiply       = power (('*' | '/' | '%') power)*
power          = unary ('**' power)?
unary          = ('-' | '!' | '~' | '&' | '++' | '--' | 'move') unary | postfix
postfix        = call ('++' | '--' | '[' expr ']' | '.' IDENT ('(' args ')')?)*
call           = primary ('(' args ')')?
primary        = INTEGER | FLOAT | STRING | 'true' | 'false' | 'null'
               | IDENT | '(' expr ')' | '[' array_elems ']' | '{' dict_pairs '}'
               | '|' params '|' expr | '||' expr
               | IDENT '{' field_inits '}'
array_elems    = ('...' expr | expr) (',' ('...' expr | expr))* ','?
dict_pairs     = (expr ':' expr) (',' (expr ':' expr))* ','?
field_inits    = (IDENT ':' expr ','?)*
args           = (expr (',' expr)*)?
```
