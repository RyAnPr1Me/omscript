# OmScript Language Reference

> **Version:** 2.3.9  
> **Compiler:** `omsc` — OmScript Compiler v2.3.9  
> **Standard:** C++17 · LLVM Backend · Ahead-of-Time Compilation  
> **License:** See repository root

---

## Table of Contents

1. [Overview](#1-overview)
2. [Compilation Pipeline](#2-compilation-pipeline)
3. [Type System](#3-type-system)
4. [Lexical Structure](#4-lexical-structure)
5. [Variables and Constants](#5-variables-and-constants)
6. [Functions](#6-functions)
7. [Operators](#7-operators)
8. [Control Flow](#8-control-flow)
9. [Arrays](#9-arrays)
10. [Strings](#10-strings)
11. [Structs](#11-structs)
12. [Enums](#12-enums)
13. [Maps](#13-maps)
14. [Module Imports](#14-module-imports)
15. [File I/O](#15-file-io)
16. [Concurrency](#16-concurrency)
17. [Standard Library](#17-standard-library)
18. [Optimization Levels](#18-optimization-levels)
19. [OPTMAX Directive](#19-optmax-directive)
20. [Adaptive JIT Runtime](#20-adaptive-jit-runtime)
21. [Memory Management](#21-memory-management)
22. [CLI Reference](#22-cli-reference)
23. [Building from Source](#23-building-from-source)
24. [Error Handling and Diagnostics](#24-error-handling-and-diagnostics)
25. [Complete Code Examples](#25-complete-code-examples)
26. [Grammar Summary](#26-grammar-summary)

---

## 1. Overview

OmScript is a **low-level, C-like programming language** featuring:

- **Dynamic typing** — Variables do not require explicit type annotations (though optional type hints are supported for documentation and OPTMAX optimization).
- **Ahead-of-Time (AOT) compilation** — All code compiles to native machine code through LLVM.
- **Reference-counted memory management** — Automatic deterministic deallocation via malloc/free with reference counting on strings.
- **Adaptive JIT runtime** — A lightweight JIT runtime monitors function call counts and recompiles hot functions at higher optimization levels with profile-guided hints, producing even faster native code for performance-critical paths.
- **Aggressive optimization** — Four optimization levels (O0–O3) plus a special OPTMAX directive that applies exhaustive multi-pass optimization to marked functions.
- **92 built-in standard library functions** — Math, array manipulation, string, character classification, type conversion, map/dictionary, file I/O, concurrency, and system functions, all compiled to native machine code.
- **Structs** — Named record types with field access and mutation.
- **Module system** — `import` statements with circular-import detection.
- **Generic function syntax** — Type-annotated generic parameters (type-erased at runtime).
- **Concurrency primitives** — POSIX thread creation/join and mutex operations.

### Design Philosophy

OmScript prioritizes **performance** and **simplicity**. The language provides the essential building blocks (functions, variables, loops, conditionals, arrays, structs, maps, modules, and I/O) without a garbage collector. Every stdlib function compiles directly to LLVM IR, producing native machine instructions with zero runtime dispatch overhead.

---

## 2. Compilation Pipeline

### 2.1 Pipeline Stages

The OmScript compiler (`omsc`) transforms source code to a native executable through eight stages:

```
Source Code (.om)
       │
       ▼
   ┌──────────┐
   │  LEXER   │  Tokenizes source into a stream of tokens
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │  PARSER  │  Builds an Abstract Syntax Tree (AST)
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │  E-GRAPH │  Equality-saturation rewrites on AST (O2+)
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │ CODEGEN  │  Translates AST → LLVM IR
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │ LLVM OPT │  Standard LLVM optimization pipeline
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │SUPEROPT  │  Post-pipeline superoptimizer (O2+)
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │   HGOE   │  Hardware-graph target-aware opts (O2+, -march/-mtune)
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │ LLVM BE  │  LLVM compiles IR → native object file (.o)
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │  LINKER  │  GCC links object file → executable
   └──────────┘
```

### 2.2 Stage Details

| Stage | Input | Output | Tool |
|-------|-------|--------|------|
| **Lexing** | Source text (`.om` file) | Token stream | `Lexer::tokenize()` |
| **Parsing** | Token stream | Abstract Syntax Tree | `Parser::parse()` |
| **E-Graph Optimization** | AST | Optimized AST | `egraph::optimizeProgram()` |
| **Code Generation** | AST | LLVM IR Module | `CodeGenerator::generate()` |
| **LLVM Optimization** | LLVM IR Module | Optimized LLVM IR | `runOptimizationPasses()` |
| **Superoptimizer** | Optimized LLVM IR | Further-optimized IR | `superopt::superoptimizeModule()` |
| **HGOE** | Optimized LLVM IR | Target-tuned IR | `hgoe::optimizeModule()` |
| **Object Emission** | Optimized LLVM IR | Native object file (`.o`) | `CodeGenerator::writeObjectFile()` |
| **Linking** | Object file | Executable binary | System `gcc` |

### 2.3 Intermediate Representations

You can inspect each stage's output:

```bash
# View tokens
omsc lex program.om

# View AST
omsc parse program.om

# View LLVM IR
omsc emit-ir program.om

# Full compilation (default)
omsc program.om -o myprogram
```

### 2.4 Target Architecture

OmScript auto-detects the host architecture and emits native code for it. Supported backends:

- **x86-64** (primary target)
- **AArch64** (ARM64)
- **ARM** (32-bit)

The target triple is determined at compile time from the host system (e.g., `x86_64-pc-linux-gnu`).

---

## 3. Type System

### 3.1 Runtime Value Types

OmScript is **dynamically typed**. At runtime, values are represented by a tagged union:

| Type | Internal Representation | Description |
|------|------------------------|-------------|
| `INTEGER` | `int64_t` (64-bit signed) | Whole numbers, booleans (0 = false, nonzero = true) |
| `FLOAT` | `double` (64-bit IEEE 754) | Floating-point numbers |
| `STRING` | `RefCountedString` | Reference-counted heap-allocated string |
| `NONE` | — | Absence of a value |

### 3.2 Compile-Time Representation

All values are represented as `i64` (64-bit integer) in the LLVM IR. This is the "default type" used throughout code generation:

- **Integers** are stored directly as `i64`.
- **Booleans** are `i64` where `0` = false, nonzero = true.
- **Pointers** (arrays, strings) are cast to `i64` via `PtrToInt` and back via `IntToPtr`.
- **Floats** use `double` for arithmetic and are bitcast to/from `i64` for uniform storage.

### 3.3 Truthiness

The following values are **falsy**:

- Integer `0`
- Float `0.0`
- `NONE`

Everything else is **truthy**, including negative numbers and non-empty strings.

### 3.4 Optional Type Annotations

Variable and parameter declarations may include optional type hints. These are currently used for documentation and OPTMAX optimization hints but are not enforced at compile time:

```javascript
var x: int = 42;        // type hint: int
fn add(a: int, b: int) { return a + b; }
```

---

## 4. Lexical Structure

### 4.1 Character Set

OmScript source files are UTF-8 text. Identifiers and keywords use ASCII characters only.

### 4.2 Comments

```javascript
// Single-line comment (extends to end of line)

/* Multi-line comment
   can span multiple lines */
```

Block comments (`/* */`) are **not nestable**. The lexer tracks line and column numbers inside comments for error reporting.

### 4.3 Identifiers

Identifiers start with a letter or underscore, followed by letters, digits, or underscores:

```
identifier := [a-zA-Z_][a-zA-Z0-9_]*
```

### 4.4 Keywords

The following 24 words are reserved:

| Keyword | Purpose |
|---------|---------|
| `fn` | Function declaration |
| `return` | Return from function |
| `if` | Conditional branch |
| `else` | Alternative branch |
| `while` | While loop |
| `do` | Do-while loop |
| `for` | For loop (range-based and for-each) |
| `var` | Variable declaration |
| `const` | Constant declaration |
| `break` | Break out of loop or switch case |
| `continue` | Skip to next iteration |
| `in` | Range/array iteration keyword |
| `switch` | Multi-way branch |
| `case` | Switch case label |
| `default` | Switch default label |
| `try` | Error handling block |
| `catch` | Error handler |
| `throw` | Throw an error value |
| `enum` | Enum declaration |
| `struct` | Struct/record type declaration |
| `import` | Module import statement |
| `true` | Boolean true (1) |
| `false` | Boolean false (0) |
| `null` | Null literal |

### 4.5 Literals

#### Integer Literals

Decimal integer literals:

```javascript
0
42
1000000
-7          // Unary minus applied to 7
```

Hexadecimal, octal, and binary literals are also supported:

```javascript
0xFF        // Hex: 255
0x1A        // Hex: 26
0o77        // Octal: 63
0o10        // Octal: 8
0b1010      // Binary: 10
0b11111111  // Binary: 255
```

The prefix is case-insensitive (`0x` and `0X` are equivalent, likewise for `0o`/`0O` and `0b`/`0B`). At least one digit must follow the prefix.

All numeric literals (decimal, hex, octal, binary, and float) support **underscore separators** for readability. Underscores are stripped during lexing and do not affect the value:

```javascript
1_000_000     // Decimal: 1000000
0xFF_FF       // Hex: 65535
0b1010_0101   // Binary: 165
0o7_7         // Octal: 63
3.14_159      // Float: 3.14159
```

#### Float Literals

Floating-point literals with a decimal point:

```javascript
3.14
0.5
100.0
```

#### String Literals

Double-quoted strings with escape sequence support:

```javascript
"hello world"
"line one\nline two"
"tab\there"
"quote: \"escaped\""
"backslash: \\"
```

**Escape Sequences:**

| Escape | Character |
|--------|-----------|
| `\n` | Newline (0x0A) |
| `\t` | Tab (0x09) |
| `\r` | Carriage return (0x0D) |
| `\0` | Null (0x00) |
| `\b` | Backspace (0x08) |
| `\f` | Form feed (0x0C) |
| `\v` | Vertical tab (0x0B) |
| `\\` | Backslash |
| `\"` | Double quote |
| `\xHH` | Hex escape (exactly two hex digits, e.g. `\x41` → `A`) |

Unterminated strings and unterminated escape sequences (backslash at end of string) produce compile errors.

#### Multi-line String Literals

Triple-quoted strings (`"""..."""`) support embedded newlines without escape sequences:

```javascript
var poem = """Roses are red,
Violets are blue,
OmScript is fast,
And so are you.""";
```

Multi-line strings preserve all characters between the opening and closing `"""` exactly as written, including newlines and spaces. They do **not** process escape sequences.

### 4.6 Special Tokens

| Token | Meaning |
|-------|---------|
| `OPTMAX=:` | Begin OPTMAX optimization block |
| `OPTMAX!:` | End OPTMAX optimization block |
| `...` | Range operator (in for-loop ranges) |

---

## 5. Variables and Constants

### 5.1 Variable Declaration

Variables are declared with `var` and optionally initialized:

```javascript
var x = 10;          // Initialized
var y;               // Default-initialized to 0
var z: int = 42;     // With optional type annotation
```

Uninitialized variables default to `0`.

### 5.2 Constant Declaration

Constants are declared with `const` and **must** be initialized:

```javascript
const MAX_SIZE = 100;
```

Attempting to reassign a constant produces a compile error:

```javascript
const x = 5;
x = 10;    // ERROR: Cannot modify constant 'x'
```

### 5.3 Assignment

```javascript
x = 42;             // Simple assignment
x += 5;             // Compound: x = x + 5
x -= 3;             // Compound: x = x - 3
x *= 2;             // Compound: x = x * 2
x /= 4;             // Compound: x = x / 4
x %= 3;             // Compound: x = x % 3
```

Compound assignment operators (`+=`, `-=`, `*=`, `/=`, `%=`) are desugared by the parser into their equivalent binary + assignment form.

### 5.4 Scoping

OmScript uses **block scoping**. Variables declared inside a block `{ }` are not visible outside it:

```javascript
fn main() {
    var x = 1;
    {
        var x = 2;      // Shadows outer x
        var y = x + 3;  // y = 5
    }
    // x is 1 here; y is not accessible
    var y = x + 4;      // y = 5
    return y;            // Returns 5
}
```

Variables in inner scopes **shadow** identically-named variables in outer scopes.

---

## 6. Functions

### 6.1 Function Declaration

Functions are declared with the `fn` keyword:

```javascript
fn functionName(param1, param2) {
    // body
    return value;
}
```

With optional type annotations:

```javascript
fn add(a: int, b: int) {
    return a + b;
}
```

With an optional return-type annotation:

```javascript
fn square(n: int) -> int {
    return n * n;
}
```

### 6.2 Generic Functions

Functions may declare one or more **type parameters** enclosed in angle brackets. Type parameters are used for documentation and tooling only; all values are represented as `i64` at runtime (type-erased), so no code is specialised per type.

```javascript
fn identity<T>(x: T) -> T {
    return x;
}

fn first<T>(arr) -> T {
    return arr[0];
}

fn pair<A, B>(a: A, b: B) {
    return a;
}

fn main() {
    println(identity(42));        // 42
    println(first([10, 20, 30])); // 10
    return 0;
}
```

### 6.3 The `main` Function

Every OmScript program must have a `main` function. This is the entry point. The return value of `main` becomes the process exit code (modulo 256):

```javascript
fn main() {
    return 0;    // Exit code 0
}
```

### 6.4 Function Calls

```javascript
var result = factorial(5);
print(result);
```

Functions support forward references — a function may call another function defined later in the file. Recursive and mutually recursive calls are supported.

### 6.5 Default Parameters

Parameters can have default values. When a function is called with fewer arguments than parameters, the missing arguments use their default values. Default values must be literal expressions (integer, float, or string). Non-default parameters must come before default parameters.

```javascript
fn greet(name, greeting = "Hello") {
    println(greeting);
    println(name);
    return 0;
}

fn main() {
    greet("Alice");           // Uses default: greeting = "Hello"
    greet("Bob", "Hi");      // Overrides: greeting = "Hi"
    return 0;
}
```

Multiple default parameters are supported:

```javascript
fn create(x, y = 0, z = 0) {
    return x + y + z;
}

fn main() {
    print(create(1));         // 1  (y=0, z=0)
    print(create(1, 2));      // 3  (z=0)
    print(create(1, 2, 3));   // 6
    return 0;
}
```

### 6.6 Return Values

All functions return an `i64` value. If no `return` statement is reached, the function returns `0` by default.

```javascript
fn nothing() {
    // No return statement
}
// nothing() returns 0
```

### 6.7 Recursion

OmScript supports recursion:

```javascript
fn factorial(n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}
```

---

## 7. Operators

### 7.1 Operator Precedence Table

Operators are listed from **highest** to **lowest** precedence:

| Precedence | Operators | Associativity | Description |
|:----------:|-----------|:-------------:|-------------|
| 1 | `()` `[]` | Left | Grouping, array indexing |
| 2 | `x++` `x--` | Left | Postfix increment/decrement |
| 3 | `++x` `--x` `-x` `!` `~` | Right | Prefix increment/decrement, unary minus, logical NOT, bitwise NOT |
| 4 | `**` | Right | Exponentiation |
| 5 | `*` `/` `%` | Left | Multiplication, division, modulo |
| 6 | `+` `-` | Left | Addition, subtraction |
| 7 | `<<` `>>` | Left | Bitwise left shift, logical right shift |
| 8 | `<` `<=` `>` `>=` | Left | Relational comparison |
| 9 | `==` `!=` | Left | Equality comparison |
| 10 | `&` | Left | Bitwise AND |
| 11 | `^` | Left | Bitwise XOR |
| 12 | `\|` | Left | Bitwise OR |
| 13 | `&&` | Left | Logical AND (short-circuit) |
| 14 | `\|\|` | Left | Logical OR (short-circuit) |
| 14.5 | `??` | Left | Null coalescing |
| 15 | `? :` | Right | Ternary conditional |
| 15.5 | `\|>` | Left | Pipe forward |
| 16 | `=` `+=` `-=` `*=` `/=` `%=` | Right | Assignment |

### 7.2 Arithmetic Operators

| Operator | Example | Description |
|----------|---------|-------------|
| `+` | `a + b` | Addition |
| `-` | `a - b` | Subtraction |
| `*` | `a * b` | Multiplication |
| `/` | `a / b` | Integer division (truncates toward zero) |
| `%` | `a % b` | Modulo (remainder) |
| `**` | `a ** b` | Exponentiation (right-associative) |
| `-` (unary) | `-a` | Negation |

```javascript
2 ** 8        // 256
3 ** 3        // 27
2 ** 3 ** 2   // 512 (right-associative: 2 ** (3 ** 2) = 2 ** 9)
2 ** -1       // 0 (negative exponent → 0 for integer power)
```

**Division and modulo by zero** are detected in the generated machine code at runtime. They print an error and terminate the program with exit code 1:

```
Runtime error: division by zero
Runtime error: modulo by zero
```

### 7.3 Comparison Operators

All comparison operators return `1` (true) or `0` (false):

| Operator | Example | Description |
|----------|---------|-------------|
| `==` | `a == b` | Equal to |
| `!=` | `a != b` | Not equal to |
| `<` | `a < b` | Less than |
| `<=` | `a <= b` | Less than or equal |
| `>` | `a > b` | Greater than |
| `>=` | `a >= b` | Greater than or equal |

### 7.4 Logical Operators

| Operator | Example | Description |
|----------|---------|-------------|
| `&&` | `a && b` | Logical AND (short-circuit) |
| `\|\|` | `a \|\| b` | Logical OR (short-circuit) |
| `!` | `!a` | Logical NOT |

**Short-circuit evaluation**: `&&` does not evaluate the right operand if the left is falsy. `||` does not evaluate the right operand if the left is truthy.

```javascript
// y is never set to 1 because 0 is falsy
if (0 && (y = 1)) { }

// y is never set to 2 because 1 is truthy
if (1 || (y = 2)) { }
```

### 7.5 Bitwise Operators

| Operator | Example | Description |
|----------|---------|-------------|
| `&` | `a & b` | Bitwise AND |
| `\|` | `a \| b` | Bitwise OR |
| `^` | `a ^ b` | Bitwise XOR |
| `~` | `~a` | Bitwise NOT (complement) |
| `<<` | `a << n` | Left shift |
| `>>` | `a >> n` | Logical right shift (zero-filling) |

```javascript
var a = 12 & 10;     // 1100 & 1010 = 1000 = 8
var b = 12 | 10;     // 1100 | 1010 = 1110 = 14
var c = 12 ^ 10;     // 1100 ^ 1010 = 0110 = 6
var d = ~0;           // All bits set = -1
var e = 1 << 4;       // 16
var f = 32 >> 2;      // 8
```

### 7.6 Increment and Decrement

| Operator | Example | Description |
|----------|---------|-------------|
| `++x` | `var b = ++a;` | Prefix: increments `a`, returns **new** value |
| `x++` | `var b = a++;` | Postfix: increments `a`, returns **old** value |
| `--x` | `var b = --a;` | Prefix: decrements `a`, returns **new** value |
| `x--` | `var b = a--;` | Postfix: decrements `a`, returns **old** value |

```javascript
var a = 5;
var b = ++a;    // a = 6, b = 6  (prefix: new value)
var c = 10;
var d = c++;    // c = 11, d = 10 (postfix: old value)
```

Increment and decrement operators also work on **array elements**:

```javascript
var arr = [10, 20, 30];
arr[0]++;           // arr[0] = 11
var old = arr[1]--; // old = 20, arr[1] = 19
var val = ++arr[2]; // val = 31, arr[2] = 31
```

### 7.7 Ternary Operator

```javascript
var result = condition ? valueIfTrue : valueIfFalse;
```

Ternary expressions can be nested:

```javascript
var sign = (x > 0) ? 1 : ((x < 0) ? -1 : 0);
```

### 7.8 Null Coalescing Operator

The `??` operator returns the left operand if it is non-zero (truthy), otherwise the right operand:

```javascript
var result = value ?? defaultValue;
// Equivalent to: value != 0 ? value : defaultValue
```

The right operand is only evaluated if the left operand is zero (short-circuit evaluation). `??` is left-associative and can be chained:

```javascript
var x = a ?? b ?? c;  // First non-zero value, or c
```

### 7.9 Compound Assignment

| Operator | Equivalent |
|----------|------------|
| `x += y` | `x = x + y` |
| `x -= y` | `x = x - y` |
| `x *= y` | `x = x * y` |
| `x /= y` | `x = x / y` |
| `x %= y` | `x = x % y` |
| `x &= y` | `x = x & y` |
| `x \|= y` | `x = x \| y` |
| `x ^= y` | `x = x ^ y` |
| `x <<= y` | `x = x << y` |
| `x >>= y` | `x = x >> y` |

### 7.10 Pipe Operator

The pipe operator `|>` passes the left operand as the argument to the function on the right. It is syntactic sugar for a function call and supports both user-defined and standard library functions.

```javascript
// expr |> fn  is equivalent to  fn(expr)
var result = [1, 2, 3] |> len;           // len([1, 2, 3]) → 3

fn double(x) { return x * 2; }
var y = 5 |> double;                      // double(5) → 10

// Chaining: left-associative, evaluated left to right
var n = [1, 2, 3, 4, 5, 6] |> len;       // 6
```

### 7.11 Lambda Expressions

Lambda expressions create anonymous functions inline. They are desugared at parse time into named helper functions, so they can be used wherever a function name string literal is expected (such as `array_map`, `array_filter`, `array_reduce`).

**Syntax:** `|params| body_expression`

```javascript
// Single parameter
var doubled = array_map([1, 2, 3], |x| x * 2);    // [2, 4, 6]

// Multiple parameters
var sum = array_reduce([1, 2, 3], |acc, x| acc + x, 0);  // 6

// Filter with lambda predicate
var evens = array_filter([1, 2, 3, 4], |x| x % 2 == 0);  // [2, 4]

// Zero parameters
var always42 = || 42;
```

> **Note:** Lambdas are compile-time constructs. They do not capture variables from the enclosing scope — they are pure functions of their parameters.

### 7.12 Spread Operator

The spread operator `...` unpacks array elements into a new array literal. It can appear anywhere inside `[...]` brackets alongside regular elements.

```javascript
var a = [1, 2, 3];
var b = [4, 5, 6];

var combined = [...a, ...b];           // [1, 2, 3, 4, 5, 6]
var prepended = [0, ...a];             // [0, 1, 2, 3]
var sandwiched = [0, ...a, 99, ...b];  // [0, 1, 2, 3, 99, 4, 5, 6]
```

The spread operator computes the total length at runtime and allocates a single result array.

---

## 8. Control Flow

### 8.1 If / Else

```javascript
if (condition) {
    // then branch
}

if (condition) {
    // then branch
} else {
    // else branch
}

// Single-statement (no braces)
if (x > 0) return x;
else return -x;
```

### 8.2 While Loop

```javascript
while (condition) {
    // body
}
```

Example:

```javascript
var i = 0;
var sum = 0;
while (i < 10) {
    sum = sum + i;
    i = i + 1;
}
```

### 8.3 Do-While Loop

Executes the body **at least once**, then checks the condition:

```javascript
do {
    // body
} while (condition);
```

Example:

```javascript
var sum = 0;
var i = 1;
do {
    sum += i;
    i++;
} while (i <= 5);
// sum = 15
```

### 8.4 For Loop (Range-Based)

OmScript's for loop iterates over a range of integers:

```javascript
// Ascending: 0, 1, 2, 3, 4
for (i in 0...5) {
    print(i);
}

// With optional type annotation
for (i: int in 0...10) {
    // ...
}

// With explicit step
for (i in 0...20...2) {
    // i = 0, 2, 4, 6, ..., 18
}

// Descending range with negative step
for (i in 5...0...-1) {
    // i = 5, 4, 3, 2, 1
}
```

**Syntax:** `for (iterator in start...end[...step]) body`

- `start` is **inclusive**
- `end` is **exclusive**
- `step` defaults to `1` (or `-1` if `start > end`)
- The iterator variable is loop-scoped

#### For-Each Loop (Array Iteration)

The `for...in` syntax also supports iterating over arrays:

```javascript
var arr = [10, 20, 30, 40, 50];
var total = 0;
for (x in arr) {
    total = total + x;
}
// total = 150
```

When the right-hand side of `in` is an array (rather than a range with `...`), the loop iterates over each element.

### 8.5 Break and Continue

```javascript
while (i < 100) {
    if (i == 50) break;       // Exit the loop
    if (i % 2 == 0) {
        i++;
        continue;             // Skip to next iteration
    }
    sum += i;
    i++;
}
```

`break` and `continue` work in `while`, `do-while`, and `for` loops. Using them outside a loop produces a compile error.

### 8.6 Blocks

Braces `{ }` introduce a new scope:

```javascript
{
    var x = 10;    // x is only visible inside this block
}
// x is not accessible here
```

### 8.7 Try / Catch / Throw

OmScript supports structured error handling via `try`, `catch`, and `throw`:

```javascript
try {
    // Code that may throw an error
    if (x < 0) {
        throw 42;  // throw an integer error code
    }
    var result = risky_operation();
} catch (err) {
    // err contains the thrown value (42 in this case)
    println(err);
}
```

**Key points:**
- `throw` accepts any integer expression as an error value.
- The `catch` block binds the thrown value to the named variable.
- If no `throw` occurs in the `try` block, the `catch` block is skipped.
- Try/catch blocks can be nested.

```javascript
try {
    try {
        throw 1;
    } catch (inner) {
        // inner == 1
    }
    // Execution continues here after inner catch
    throw 2;
} catch (outer) {
    // outer == 2
}
```

### 8.8 Enums

Enums declare named integer constants at the top level of a program:

```javascript
enum Color {
    RED,        // 0 (auto-incremented from 0)
    GREEN = 10, // explicit value
    BLUE        // 11 (auto-incremented from previous)
}

fn main() {
    var c = Color_GREEN;  // accessed as EnumName_MemberName
    if (c == Color_GREEN) {
        println("green!");
    }
    return 0;
}
```

**Key points:**
- Members default to 0 for the first value and auto-increment by 1.
- Explicit values can be assigned with `= value`.
- Enum members are accessed as `EnumName_MemberName` (e.g., `Color_RED`).
- Enums are compile-time constants — they produce no runtime overhead.

### 8.9 Switch / Case / Default

The `switch` statement provides multi-way branching on integer values:

```javascript
fn classify(x) {
    switch (x) {
        case 1:
            return 10;
        case 2:
            return 20;
        case 3:
            return 30;
        default:
            return 0;
    }
}
```

**Key points:**
- Case values must be integer constants (float literals produce a compile error).
- Use `break` to exit a case (fall-through occurs without it).
- The `default` case handles any value not matched by an explicit `case`.
- `continue` inside a `switch` nested within a loop jumps to the enclosing loop's continue target.

---

## 9. Arrays

### 9.1 Array Literals

Arrays are created with square bracket syntax:

```javascript
var arr = [10, 20, 30, 40, 50];
var empty = [];
var mixed = [1, 2 + 3, factorial(5)];
```

### 9.2 Internal Representation

Arrays are stack-allocated as contiguous `i64` slots:

```
Slot 0: length (number of elements)
Slot 1: element 0
Slot 2: element 1
...
Slot N: element N-1
```

The array value is stored as a pointer cast to `i64`. The `len()` builtin reads slot 0; indexing reads slot `index + 1`.

### 9.3 Array Indexing

```javascript
var arr = [10, 20, 30];
var first = arr[0];     // 10
var last = arr[2];      // 30
```

**Bounds checking** is performed at runtime. Accessing an out-of-bounds index prints an error and terminates the program:

```
Runtime error: array index out of bounds
```

### 9.4 Array Iteration

```javascript
var arr = [10, 20, 30, 40, 50];
var sum = 0;
for (i in 0...len(arr)) {
    sum = sum + arr[i];
}
// sum = 150
```

### 9.5 Array Mutation

Arrays can be mutated in-place using stdlib functions:

```javascript
var arr = [1, 2, 3, 4, 5];
swap(arr, 0, 4);       // arr = [5, 2, 3, 4, 1]
reverse(arr);           // arr = [1, 4, 3, 2, 5]
```

---

## 10. Strings

### 10.1 String Literals

Strings are enclosed in double quotes:

```javascript
print("hello world");
print("line 1\nline 2");
```

### 10.2 String Support

Strings are supported as:

- **Global string constants** — String literals are emitted as LLVM `CreateGlobalString` constants.
- **Print argument** — `print("message")` detects string literal arguments and uses `%s\n` format.
- **Return type** — String values are stored as pointer-to-i64 in the uniform value representation.

### 10.3 Runtime String Type

At runtime, strings are fully dynamic:

- Reference-counted with `RefCountedString` (malloc-based).
- Support concatenation (`+`), comparison (`==`, `<`), and conversion (`toString()`).
- Automatic deallocation when the reference count reaches zero.

---

## 11. Structs

### 11.1 Declaration

Structs declare named record types at the top level. Fields are separated by commas:

```javascript
struct Point { x, y }
struct Color { r, g, b }
struct Person { name, age, score }
```

### 11.2 Creating Struct Instances

Use a **struct literal** with field names and values:

```javascript
struct Point { x, y }

fn main() {
    var p = Point { x: 10, y: 20 };
    return 0;
}
```

### 11.3 Field Access

Use dot notation to read a field:

```javascript
struct Point { x, y }

fn main() {
    var p = Point { x: 10, y: 20 };
    println(p.x);   // 10
    println(p.y);   // 20
    return 0;
}
```

### 11.4 Field Assignment

Use dot notation on the left-hand side to update a field:

```javascript
struct Point { x, y }

fn main() {
    var p = Point { x: 10, y: 20 };
    p.x = 30;
    println(p.x);   // 30
    return 0;
}
```

### 11.5 Structs as Function Arguments and Return Values

Structs are passed and returned like any other value:

```javascript
struct Point { x, y }

fn make_point(a, b) {
    return Point { x: a, y: b };
}

fn sum(p) {
    return p.x + p.y;
}

fn main() {
    var p = make_point(3, 4);
    println(sum(p));   // 7
    return 0;
}
```

**Key points:**
- All field names in a struct literal must match the declared fields (order is flexible).
- Field values may be any expression.
- Structs are value types passed by reference internally — mutation via dot-assign affects the original variable.

---

## 12. Enums

Enums declare named integer constants at the top level. See [section 8.8](#88-enums) for full details.

```javascript
enum Direction { NORTH, SOUTH = 10, EAST, WEST }

fn main() {
    var d = Direction_EAST;   // 11
    return d;
}
```

---

## 13. Maps

Maps (dictionaries) store key-value pairs where keys are strings and values are `i64`.

### 13.1 Creating a Map

```javascript
var m = map_new();
```

### 13.2 Setting and Getting Values

```javascript
m = map_set(m, "x", 42);
var v = map_get(m, "x", 0);   // 0 is the default if key absent
println(v);                    // 42
```

### 13.3 Checking for Keys

```javascript
var exists = map_has(m, "x");   // 1 if present, 0 otherwise
```

### 13.4 Removing Keys

```javascript
m = map_remove(m, "x");
```

### 13.5 Querying Size and Iterating

```javascript
var sz = map_size(m);
var keys   = map_keys(m);    // returns an array of key strings
var values = map_values(m);  // returns an array of values
```

### 13.6 Full Example

```javascript
fn main() {
    var m = map_new();
    m = map_set(m, "a", 1);
    m = map_set(m, "b", 2);
    m = map_set(m, "c", 3);

    println(map_get(m, "b", 0));   // 2
    println(map_has(m, "d"));      // 0
    println(map_size(m));          // 3

    m = map_remove(m, "a");
    println(map_size(m));          // 2
    return 0;
}
```

---

## 14. Module Imports

### 14.1 Import Syntax

The `import` statement includes another `.om` source file before compilation:

```javascript
import "path/to/module";
```

- The `.om` extension is added automatically if omitted.
- Paths are relative to the importing file's directory.
- Circular imports are detected and silently skipped.

### 14.2 Module Files

A module is an ordinary `.om` file that exports functions:

```javascript
// modules/math_utils.om
fn square(n) { return n * n; }
fn cube(n)   { return n * n * n; }
fn add(a, b) { return a + b; }
```

### 14.3 Using Imported Functions

After importing, all functions from the module are available in the current file:

```javascript
import "modules/math_utils";

fn main() {
    println(square(5));   // 25
    println(cube(3));     // 27
    println(add(10, 20)); // 30
    return 0;
}
```

### 14.4 Multiple Imports

```javascript
import "modules/math_utils";
import "modules/string_utils";

fn main() {
    println(square(4));        // 16
    println(greet("World"));   // Hello, World!
    return 0;
}
```

### 14.5 Circular Import Detection

If file A imports file B which imports file A, the second import of A is silently ignored. This prevents infinite recursion during compilation.

---

## 15. File I/O

### 15.1 Reading Files

```javascript
var content = file_read("data.txt");   // returns string, or "" on error
```

### 15.2 Writing Files

```javascript
file_write("output.txt", "Hello, World!\n");   // overwrites the file
```

### 15.3 Appending to Files

```javascript
file_append("log.txt", "New entry\n");   // appends to existing file
```

### 15.4 Checking File Existence

```javascript
var exists = file_exists("data.txt");   // 1 if exists, 0 otherwise
```

### 15.5 Full Example

```javascript
fn main() {
    if (!file_exists("out.txt")) {
        file_write("out.txt", "first line\n");
    }
    file_append("out.txt", "second line\n");
    var text = file_read("out.txt");
    println(text);
    return 0;
}
```

---

## 16. Concurrency

OmScript provides POSIX thread and mutex primitives.

### 16.1 Threads

#### Creating a Thread

```javascript
var handle = thread_create("function_name");
```

- The argument is the **name** of a zero-argument function defined in the current program.
- The function runs concurrently in a new POSIX thread.
- Returns an opaque thread handle.

#### Joining a Thread

```javascript
thread_join(handle);
```

Blocks until the thread completes.

### 16.2 Mutexes

```javascript
var m = mutex_new();     // create a mutex
mutex_lock(m);           // acquire the lock
// ... critical section ...
mutex_unlock(m);         // release the lock
mutex_destroy(m);        // free the mutex
```

### 16.3 Example

```javascript
fn worker() {
    println(42);
    return 0;
}

fn main() {
    var m = mutex_new();
    mutex_lock(m);
    mutex_unlock(m);
    mutex_destroy(m);

    var t = thread_create("worker");
    thread_join(t);

    return 0;
}
```

**Key points:**
- Thread functions must be zero-parameter functions defined in the same program.
- Mutex operations must be balanced (each lock must have a matching unlock).
- Functions that use concurrency primitives do not receive the `NoSync`/`NoFree` LLVM attributes.

---

## 17. Standard Library

OmScript provides **92 built-in functions**. All stdlib functions are compiled directly to native machine code via LLVM IR.

### 17.1 I/O Functions

#### `print(value)`

Prints a value followed by a newline.

- **Integer argument:** Prints using `%lld\n` format via `printf`.
- **String literal argument:** Prints using `%s\n` format via `printf`.
- **Returns:** `0` for all argument types.

```javascript
print(42);              // Output: 42
print("hello world");   // Output: hello world
```

#### `print_char(code)`

Prints a single ASCII character (no newline) using `putchar`.

- **Parameter:** `code` — ASCII character code (integer).
- **Returns:** The character code.

```javascript
print_char(72);     // Output: H
print_char(105);    // Output: i
print_char(10);     // Output: (newline)
```

#### `input()`

Reads an integer from standard input using `scanf("%lld")`.

- **Parameters:** None.
- **Returns:** The integer read from stdin.

```javascript
var n = input();
print(n);
```

#### `input_line()`

Reads a full line from standard input as a string. Strips the trailing newline character. Returns an empty string on EOF.

- **Parameters:** None.
- **Returns:** A heap-allocated string containing the line read from stdin.

```javascript
var line = input_line();
println(line);
```

#### `println(value)`

Prints a value followed by a newline. Functionally identical to `print()` — provided as an explicit alias for clarity.

- **Returns:** `0`.

```javascript
println(42);           // Output: 42
println("hello");      // Output: hello
```

#### `write(value)`

Prints a value **without** a trailing newline. Useful for building output incrementally.

- **Returns:** `0`.

```javascript
write("hello ");
write("world");    // Output: hello world (on same line)
```

#### `exit_program(code)`

Terminates the program immediately with the given exit code.

- **Parameter:** `code` — integer exit code.
- **Returns:** Never returns (process terminates).

```javascript
if (error) {
    exit_program(1);
}
```

### 17.2 Math Functions

#### `abs(x)`

Returns the absolute value of `x`.

- **Implementation:** `x >= 0 ? x : -x` (branchless via LLVM `select`).

```javascript
abs(-10)    // 10
abs(5)      // 5
abs(0)      // 0
```

#### `min(a, b)`

Returns the smaller of two signed integers.

```javascript
min(3, 7)     // 3
min(-5, -1)   // -5
```

#### `max(a, b)`

Returns the larger of two signed integers.

```javascript
max(3, 7)     // 7
max(-5, -1)   // -1
```

#### `sign(x)`

Returns the sign of `x` as -1, 0, or 1.

```javascript
sign(42)    // 1
sign(-7)    // -1
sign(0)     // 0
```

#### `clamp(value, lo, hi)`

Clamps `value` to the range `[lo, hi]`.

- **Implementation:** `max(lo, min(value, hi))`

```javascript
clamp(5, 0, 10)     // 5
clamp(-3, 0, 10)    // 0
clamp(15, 0, 10)    // 10
```

#### `pow(base, exponent)`

Computes integer exponentiation (`base^exponent`).

- **Implementation:** Loop multiplying `base` × `base` for `exponent` iterations using LLVM PHI nodes.
- **Returns** `1` for `exponent <= 0`.

```javascript
pow(2, 0)     // 1
pow(2, 3)     // 8
pow(3, 2)     // 9
```

#### `sqrt(x)`

Computes the integer square root (floor) using Newton's method.

- **Returns** `0` for `x <= 0`.
- Includes a convergence guarantee to prevent infinite loops.

```javascript
sqrt(0)      // 0
sqrt(1)      // 1
sqrt(9)      // 3
sqrt(10)     // 3  (floor)
sqrt(99)     // 9  (floor)
```

#### `is_even(x)`

Returns `1` if `x` is even, `0` otherwise.

- **Implementation:** `(x & 1) == 0` (bitwise check, no branching).

```javascript
is_even(4)    // 1
is_even(7)    // 0
is_even(0)    // 1
```

#### `is_odd(x)`

Returns `1` if `x` is odd, `0` otherwise.

- **Implementation:** `x & 1` (single bitwise AND).

```javascript
is_odd(3)     // 1
is_odd(8)     // 0
```

#### `log2(n)`

Returns the integer base-2 logarithm (floor) of `n`. Returns `-1` for `n <= 0`.

- **Implementation:** Loop counting right-shifts until zero.

```javascript
log2(1)       // 0
log2(2)       // 1
log2(8)       // 3
log2(1024)    // 10
log2(7)       // 2  (floor)
log2(0)       // -1
log2(-5)      // -1
```

#### `gcd(a, b)`

Returns the greatest common divisor of `a` and `b` using the Euclidean algorithm. Works with negative numbers (uses absolute values internally).

- **Implementation:** Iterative Euclidean algorithm with `abs()` preprocessing.

```javascript
gcd(12, 8)    // 4
gcd(100, 75)  // 25
gcd(7, 13)    // 1
gcd(0, 5)     // 5
gcd(-12, 8)   // 4
```

#### `floor(x)`

Returns the largest integer less than or equal to `x`. Converts float to integer.

```javascript
floor(3.7)    // 3
floor(3.2)    // 3
floor(-1.5)   // -2
```

#### `ceil(x)`

Returns the smallest integer greater than or equal to `x`. Converts float to integer.

```javascript
ceil(3.2)     // 4
ceil(3.7)     // 4
ceil(-1.5)    // -1
```

#### `round(x)`

Returns the nearest integer to `x`, rounding half away from zero.

```javascript
round(3.4)    // 3
round(3.5)    // 4
round(3.7)    // 4
```

#### `to_int(x)`

Converts a float to an integer by truncation (towards zero).

```javascript
to_int(7.9)   // 7
to_int(-3.7)  // -3
to_int(42)    // 42  (identity for integers)
```

#### `to_float(x)`

Converts an integer to a floating-point value.

```javascript
to_float(10)  // 10.0
to_float(-3)  // -3.0
```

### 17.3 Array Functions

#### `len(array)`

Returns the number of elements in an array.

- **Implementation:** Loads the length from slot 0 of the array's internal representation.

```javascript
var arr = [10, 20, 30];
len(arr)    // 3
```

#### `sum(array)`

Returns the sum of all elements in an array.

- **Implementation:** LLVM IR loop with PHI node accumulator.

```javascript
var arr = [10, 20, 30, 40];
sum(arr)    // 100
```

#### `swap(array, i, j)`

Swaps elements at indices `i` and `j` in-place.

- **Returns:** `0`.

```javascript
var arr = [10, 20, 30, 40];
swap(arr, 0, 3);
// arr is now [40, 20, 30, 10]
```

#### `reverse(array)`

Reverses the array in-place using a two-pointer approach.

- **Returns:** The array value (pointer-as-i64).

```javascript
var arr = [1, 2, 3, 4, 5];
reverse(arr);
// arr is now [5, 4, 3, 2, 1]
```

#### `push(array, value)`

Returns a new array with `value` appended to the end. The original array is not modified; the variable must be reassigned.

```javascript
var arr = [1, 2, 3];
arr = push(arr, 4);
// arr is now [1, 2, 3, 4], len(arr) == 4
```

#### `pop(array)`

Removes and returns the last element of the array. The array length is decreased in-place.

```javascript
var arr = [10, 20, 30];
var last = pop(arr);
// last == 30, len(arr) == 2
```

#### `index_of(array, value)`

Returns the zero-based index of the first occurrence of `value` in the array, or `-1` if not found.

```javascript
index_of([10, 20, 30], 20)   // 1
index_of([10, 20, 30], 99)   // -1
```

#### `array_contains(array, value)`

Returns `1` if `value` exists in the array, `0` otherwise.

```javascript
array_contains([10, 20, 30], 20)  // 1
array_contains([10, 20, 30], 99)  // 0
```

#### `sort(array)`

Sorts the array in-place in ascending order using bubble sort.

```javascript
var arr = [50, 10, 30, 20, 40];
sort(arr);
// arr is now [10, 20, 30, 40, 50]
```

#### `array_fill(size, value)`

Creates a new array of `size` elements, all initialized to `value`.

```javascript
var arr = array_fill(5, 42);
// arr == [42, 42, 42, 42, 42]
```

#### `array_concat(array1, array2)`

Returns a new array containing all elements of `array1` followed by all elements of `array2`.

```javascript
var merged = array_concat([1, 2, 3], [4, 5, 6]);
// merged == [1, 2, 3, 4, 5, 6]
```

#### `array_slice(array, start, end)`

Returns a new array containing elements from index `start` (inclusive) to `end` (exclusive).

```javascript
var sliced = array_slice([10, 20, 30, 40, 50], 1, 4);
// sliced == [20, 30, 40]
```

#### `array_copy(array)`

Returns a new heap-allocated copy of an array. Modifications to the copy do not affect the original.

```javascript
var original = [1, 2, 3];
var copy = array_copy(original);
copy[0] = 99;
print(original[0]);  // 1 (unaffected)
print(copy[0]);      // 99
```

#### `array_remove(array, index)`

Removes the element at the given index, shifts remaining elements left, decrements the array length, and returns the removed value. Aborts with an error if the index is out of bounds.

```javascript
var arr = [10, 20, 30, 40, 50];
var removed = array_remove(arr, 2);  // removed == 30
// arr == [10, 20, 40, 50], len(arr) == 4
```

#### `array_map(array, "function_name")`

Applies a named function to each element of the array and returns a new array with the results. The function name must be a string literal (resolved at compile time) and the function must accept at least one argument. Lambda expressions can also be used.

```javascript
fn double(x) { return x * 2; }
fn main() {
    var arr = [1, 2, 3, 4, 5];
    var doubled = array_map(arr, "double");
    // doubled == [2, 4, 6, 8, 10]

    // With lambda:
    var tripled = array_map(arr, |x| x * 3);
    // tripled == [3, 6, 9, 12, 15]
    return 0;
}
```

#### `array_filter(array, "function_name")`

Returns a new array containing only the elements for which the named predicate function returns a non-zero value. The function name must be a string literal and the function must accept at least one argument. Lambda expressions can also be used.

```javascript
fn is_even(x) { return x % 2 == 0; }
fn main() {
    var arr = [1, 2, 3, 4, 5, 6];
    var evens = array_filter(arr, "is_even");
    // evens == [2, 4, 6]

    // With lambda:
    var odds = array_filter(arr, |x| x % 2 != 0);
    // odds == [1, 3, 5]
    return 0;
}
```

#### `array_reduce(array, "function_name", initial)`

Reduces an array to a single value by applying a named two-argument function `(accumulator, element)` across all elements, starting with the given initial value. The function name must be a string literal and the function must accept at least two arguments. Lambda expressions can also be used.

```javascript
fn add(acc, x) { return acc + x; }
fn multiply(acc, x) { return acc * x; }
fn main() {
    var arr = [1, 2, 3, 4, 5];
    var total = array_reduce(arr, "add", 0);      // 15
    var product = array_reduce(arr, "multiply", 1); // 120

    // With lambda:
    var sum = array_reduce(arr, |a, b| a + b, 0);  // 15
    return 0;
}
```

### 17.4 Character Functions

#### `to_char(code)`

Identity function — returns the integer value. Provided for semantic clarity when working with character codes.

```javascript
to_char(65)    // 65 (represents 'A')
```

#### `is_alpha(code)`

Returns `1` if the character code is an ASCII letter (A–Z or a–z), `0` otherwise.

```javascript
is_alpha(65)    // 1  ('A')
is_alpha(97)    // 1  ('a')
is_alpha(48)    // 0  ('0')
is_alpha(32)    // 0  (' ')
```

#### `is_digit(code)`

Returns `1` if the character code is an ASCII digit (0–9), `0` otherwise.

```javascript
is_digit(48)    // 1  ('0')
is_digit(57)    // 1  ('9')
is_digit(65)    // 0  ('A')
```

### 17.5 String Functions

#### `str_len(s)`

Returns the number of characters in string `s` (equivalent to C `strlen`).

```javascript
var s = "hello";
str_len(s)    // 5
str_len("")   // 0
```

#### `char_at(s, index)`

Returns the ASCII character code of the character at position `index` in string `s`. Aborts with a runtime error if `index` is negative or out of range.

```javascript
var s = "hello";
char_at(s, 0)    // 104  ('h')
char_at(s, 4)    // 111  ('o')
```

#### `str_eq(a, b)`

Returns `1` if strings `a` and `b` have identical contents, `0` otherwise. Uses `strcmp` internally. Use this instead of `==` when comparing string variables, since `==` compares pointer values.

```javascript
str_eq("hello", "hello")    // 1
str_eq("hello", "world")    // 0
```

#### `str_concat(a, b)`

Returns a new string that is the concatenation of strings `a` and `b`. This is equivalent to `a + b` for strings but provides an explicit function interface.

```javascript
str_concat("hello", " world")   // "hello world"
str_concat("foo", "bar")        // "foobar"
str_concat("test", "")          // "test"
```

#### `to_string(n)`

Converts an integer to its string representation. Returns a heap-allocated string.

- **Implementation:** Uses `snprintf` with a 21-byte buffer (enough for any 64-bit signed integer).

```javascript
to_string(42)       // "42"
to_string(-100)     // "-100"
to_string(0)        // "0"
print(to_string(12345));  // Output: 12345
```

#### `str_find(s, ch)`

Finds the first occurrence of a character code `ch` in string `s`. Returns the zero-based index, or `-1` if not found.

- **Implementation:** Uses `memchr` for efficient single-character search.

```javascript
str_find("hello", 104)    // 0  ('h' at index 0)
str_find("hello", 111)    // 4  ('o' at index 4)
str_find("hello", 122)    // -1 ('z' not found)
```

#### `str_substr(s, start, length)`

Returns a new string that is a substring of `s` starting at index `start` with the given `length`.

```javascript
str_substr("hello world", 6, 5)  // "world"
str_substr("abcdef", 0, 3)       // "abc"
```

#### `str_upper(s)`

Returns a new string with all characters converted to uppercase.

```javascript
str_upper("hello")    // "HELLO"
str_upper("Hello!")   // "HELLO!"
```

#### `str_lower(s)`

Returns a new string with all characters converted to lowercase.

```javascript
str_lower("WORLD")    // "world"
str_lower("Hello!")   // "hello!"
```

#### `str_contains(s, substring)`

Returns `1` if `substring` is found within `s`, `0` otherwise.

```javascript
str_contains("hello world", "world")  // 1
str_contains("hello world", "xyz")    // 0
```

#### `str_index_of(s, substring)`

Returns the zero-based index of the first occurrence of `substring` in `s`, or `-1` if not found.

```javascript
str_index_of("hello world", "world")  // 6
str_index_of("hello world", "xyz")    // -1
```

#### `str_replace(s, old, new)`

Returns a new string with the first occurrence of `old` replaced by `new`. If `old` is not found, returns a copy of the original string.

```javascript
str_replace("hello world", "world", "there")  // "hello there"
str_replace("abcabc", "b", "x")               // "axcabc"
```

#### `str_trim(s)`

Returns a new string with leading and trailing whitespace removed.

```javascript
str_trim("  hello  ")     // "hello"
str_trim("\t text \n")    // "text"
```

#### `str_starts_with(s, prefix)`

Returns `1` if `s` starts with `prefix`, `0` otherwise.

```javascript
str_starts_with("hello world", "hello")  // 1
str_starts_with("hello world", "world")  // 0
```

#### `str_ends_with(s, suffix)`

Returns `1` if `s` ends with `suffix`, `0` otherwise.

```javascript
str_ends_with("hello world", "world")  // 1
str_ends_with("hello world", "hello")  // 0
```

#### `str_repeat(s, count)`

Returns a new string that is `s` repeated `count` times.

```javascript
str_repeat("ab", 3)   // "ababab"
str_repeat("x", 5)    // "xxxxx"
```

#### `str_reverse(s)`

Returns a new string with the characters of `s` in reverse order.

```javascript
str_reverse("hello")  // "olleh"
str_reverse("abc")    // "cba"
```

#### `str_split(s, delimiter)`

Splits a string by a single-character delimiter, returning an array of substring values.

```javascript
var parts = str_split("a,b,c", ",");
// parts == ["a", "b", "c"], len(parts) == 3
```

#### `str_to_int(s)`

Parses a string as a base-10 integer. Returns the parsed value.

```javascript
str_to_int("42")     // 42
str_to_int("-10")    // -10
```

#### `str_to_float(s)`

Parses a string as a floating-point number.

```javascript
str_to_float("3.14")   // 3.14
str_to_float("-2.5")   // -2.5
```

#### `str_chars(s)`

Converts a string into an array of integer character codes (ASCII values).

```javascript
var chars = str_chars("ABC");
// chars == [65, 66, 67]
```

### 17.5.1 System Functions

#### `random()`

Returns a pseudo-random non-negative integer. Automatically seeds the random number generator on first call using `time()`.

```javascript
var r = random();  // e.g. 1804289383
```

#### `time()`

Returns the current Unix timestamp (seconds since January 1, 1970).

```javascript
var t = time();  // e.g. 1709258765
```

#### `sleep(ms)`

Pauses execution for the specified number of milliseconds.

```javascript
sleep(1000);  // sleep for 1 second
```

### 17.6 Utility Functions

#### `typeof(x)`

Returns a type tag for `x` based on the static LLVM IR type of the expression:

| Tag | Meaning |
|-----|---------|
| `1` | Integer (default for `i64` values, including arrays) |
| `2` | Float (double-precision) |
| `3` | String (pointer type or tracked string variable) |

```javascript
typeof(42)         // 1  (integer)
typeof(3.14)       // 2  (float)
typeof("hello")    // 3  (string)
var f = 2.71828;
typeof(f)          // 2  (float variable)
var s = "world";
typeof(s)          // 3  (string variable)
```

> **Note:** The type tag reflects the *static* type known at compile time.  A variable holding the result of a function call that could return either an integer or a string will have tag `1` unless the compiler can prove it is a string.

#### `assert(condition)`

Aborts the program with a runtime error message if `condition` is falsy (zero). Returns `1` if the assertion passes.

```javascript
assert(1);          // passes, returns 1
assert(x > 0);      // passes if x > 0, otherwise aborts
assert(0);          // always aborts: "Runtime error: assertion failed"
```

### 17.7 Stdlib Summary Table

| Function | Args | Returns | Description |
|----------|:----:|---------|-------------|
| `print(x)` | 1 | 0 | Print integer or string |
| `print_char(c)` | 1 | c | Print single ASCII character |
| `input()` | 0 | integer | Read integer from stdin |
| `input_line()` | 0 | string | Read a line from stdin as string |
| `abs(x)` | 1 | \|x\| | Absolute value |
| `min(a, b)` | 2 | min | Minimum of two values |
| `max(a, b)` | 2 | max | Maximum of two values |
| `sign(x)` | 1 | -1/0/1 | Sign of value |
| `clamp(v, lo, hi)` | 3 | clamped | Clamp to range |
| `pow(base, exp)` | 2 | base^exp | Integer exponentiation |
| `sqrt(x)` | 1 | floor(√x) | Integer square root |
| `is_even(x)` | 1 | 0/1 | Even check |
| `is_odd(x)` | 1 | 0/1 | Odd check |
| `log2(n)` | 1 | floor(log₂n) | Integer base-2 logarithm (-1 if n ≤ 0) |
| `gcd(a, b)` | 2 | gcd | Greatest common divisor |
| `len(arr)` | 1 | length | Array length |
| `sum(arr)` | 1 | total | Sum of array elements |
| `swap(arr, i, j)` | 3 | 0 | Swap array elements |
| `reverse(arr)` | 1 | arr | Reverse array in-place |
| `to_char(code)` | 1 | code | Identity (semantic alias) |
| `is_alpha(code)` | 1 | 0/1 | Alphabetic check |
| `is_digit(code)` | 1 | 0/1 | Digit check |
| `str_len(s)` | 1 | length | Length of a string |
| `char_at(s, i)` | 2 | code | ASCII code of character at index `i` |
| `str_eq(a, b)` | 2 | 0/1 | String equality (1 if equal, 0 otherwise) |
| `str_concat(a, b)` | 2 | string | Concatenation of strings `a` and `b` |
| `to_string(n)` | 1 | string | Convert integer to string representation |
| `str_find(s, ch)` | 2 | index | Index of first occurrence of char (-1 if not found) |
| `floor(x)` | 1 | int | Floor of float value |
| `ceil(x)` | 1 | int | Ceiling of float value |
| `round(x)` | 1 | int | Round float to nearest integer |
| `to_int(x)` | 1 | int | Convert float to integer (truncation) |
| `to_float(x)` | 1 | float | Convert integer to float |
| `str_substr(s, i, n)` | 3 | string | Substring from index `i` of length `n` |
| `str_upper(s)` | 1 | string | Uppercase version of string |
| `str_lower(s)` | 1 | string | Lowercase version of string |
| `str_contains(s, sub)` | 2 | 0/1 | Whether string contains substring |
| `str_index_of(s, sub)` | 2 | index | Index of substring (-1 if not found) |
| `str_replace(s, old, new)` | 3 | string | Replace first occurrence of `old` with `new` |
| `str_trim(s)` | 1 | string | Remove leading/trailing whitespace |
| `str_starts_with(s, p)` | 2 | 0/1 | Whether string starts with prefix |
| `str_ends_with(s, p)` | 2 | 0/1 | Whether string ends with suffix |
| `str_repeat(s, n)` | 2 | string | Repeat string `n` times |
| `str_reverse(s)` | 1 | string | Reverse string characters |
| `push(arr, val)` | 2 | array | Append value to array (returns new array) |
| `pop(arr)` | 1 | value | Remove and return last element |
| `index_of(arr, val)` | 2 | index | Index of value in array (-1 if not found) |
| `array_contains(arr, v)` | 2 | 0/1 | Whether array contains value |
| `sort(arr)` | 1 | arr | Sort array in-place (ascending) |
| `array_fill(n, val)` | 2 | array | Create array of `n` elements all set to `val` |
| `array_concat(a, b)` | 2 | array | Concatenate two arrays |
| `array_slice(arr, s, e)` | 3 | array | Slice array from index `s` to `e` (exclusive) |
| `array_copy(arr)` | 1 | array | Create a deep copy of an array |
| `array_remove(arr, i)` | 2 | value | Remove element at index `i` and return it |
| `array_map(arr, "fn")` | 2 | array | Apply named function to each element |
| `array_filter(arr, "fn")` | 2 | array | Keep elements where named function returns non-zero |
| `array_reduce(arr, "fn", init)` | 3 | value | Reduce array using named 2-arg function |
| `typeof(x)` | 1 | 1/2/3 | Type tag: 1=int, 2=float, 3=string |
| `assert(cond)` | 1 | 1 | Abort with error if `cond` is falsy |
| `println(x)` | 1 | 0 | Print value with newline (alias for print) |
| `write(x)` | 1 | 0 | Print value without trailing newline |
| `exit_program(code)` | 1 | — | Terminate process with exit code |
| `random()` | 0 | int | Pseudo-random integer (auto-seeded) |
| `time()` | 0 | int | Current Unix timestamp (seconds) |
| `sleep(ms)` | 1 | 0 | Sleep for given milliseconds |
| `str_to_int(s)` | 1 | int | Parse string as base-10 integer |
| `str_to_float(s)` | 1 | float | Parse string as float |
| `str_split(s, delim)` | 2 | array | Split string by delimiter into array |
| `str_chars(s)` | 1 | array | Convert string to array of char codes |
| `char_code(s, i)` | 2 | int | ASCII code of character at index `i` in string `s` |
| `number_to_string(n)` | 1 | string | Convert integer or float to string |
| `string_to_number(s)` | 1 | int/float | Parse string as integer or float |
| `file_read(path)` | 1 | string | Read entire file contents as string |
| `file_write(path, s)` | 2 | 0 | Write string to file (overwrite) |
| `file_append(path, s)` | 2 | 0 | Append string to file |
| `file_exists(path)` | 1 | 0/1 | Check whether file exists |
| `map_new()` | 0 | map | Create an empty map |
| `map_set(m, k, v)` | 3 | map | Set key `k` to value `v`; returns updated map |
| `map_get(m, k, d)` | 3 | value | Get value for key `k`, or default `d` |
| `map_has(m, k)` | 2 | 0/1 | Check whether key `k` exists |
| `map_remove(m, k)` | 2 | map | Remove key `k`; returns updated map |
| `map_size(m)` | 1 | int | Number of keys in map |
| `map_keys(m)` | 1 | array | Array of all keys |
| `map_values(m)` | 1 | array | Array of all values |
| `range(start, end)` | 2 | array | Array of integers from `start` to `end-1` |
| `range_step(s, e, step)` | 3 | array | Array of integers from `s` to `e` with given step |
| `thread_create(fn)` | 1 | handle | Start named function in new thread |
| `thread_join(h)` | 1 | 0 | Wait for thread `h` to finish |
| `mutex_new()` | 0 | mutex | Create a new mutex |
| `mutex_lock(m)` | 1 | 0 | Acquire mutex `m` |
| `mutex_unlock(m)` | 1 | 0 | Release mutex `m` |
| `mutex_destroy(m)` | 1 | 0 | Destroy mutex `m` |

---

## 18. Optimization Levels

### 18.1 O0 — No Optimization

- Fastest compilation, largest and slowest output.
- LLVM IR is emitted directly without any transformation passes.
- Useful for debugging.

### 18.2 O1 — Basic Optimization

- Instruction combining and reassociation.
- CFG simplification.
- Dead code elimination.
- Minor peephole optimizations.

### 18.3 O2 — Moderate Optimization (Default)

All O1 passes plus:

- **mem2reg** — Promotes stack allocas to SSA registers.
- **SROA** — Scalar Replacement of Aggregates.
- **GVN** — Global Value Numbering (eliminates redundant computations).
- **Early CSE** — Common Subexpression Elimination.
- Additional dead code elimination passes.

### 18.4 O3 — Aggressive Optimization

All O2 passes plus:

| Pass | Description |
|------|-------------|
| **LICM** | Loop Invariant Code Motion — moves constant expressions out of loops |
| **Loop Rotation** | Transforms loops into a more optimizable form |
| **Loop Simplification** | Canonicalizes loop structure |
| **Loop Inst Simplify** | Simplifies instructions within loops |
| **Loop Unrolling** | Unrolls loops for reduced branch overhead |
| **Code Sinking** | Moves instructions closer to their use |
| **Merged Load/Store Motion** | Combines memory operations |
| **Straightline Strength Reduce** | Replaces expensive ops with cheaper ones |
| **N-Ary Reassociate** | Advanced algebraic reassociation |
| **Tail Call Elimination** | Converts tail calls to jumps |
| Second cleanup round | Instruction combining + CFG simplification + DCE |

### 18.5 Constant Folding (AST-Level)

Before LLVM optimization, the code generator performs AST-level constant folding:

- **Arithmetic:** `3 + 4` → `7`, `10 * 2` → `20`
- **Comparison:** `5 > 3` → `1`
- **Bitwise:** `0xFF & 0x0F` → `15`
- **Unary:** `-(-5)` → `5`, `!0` → `1`, `~0` → `-1`
- **Identity:** `x + 0` → `x`, `x * 1` → `x`, `x * 0` → `0`
- **Ternary:** `1 ? a : b` → `a`, `0 ? a : b` → `b`

---

## 19. OPTMAX Directive

### 19.1 Overview

`OPTMAX` is a compiler directive that marks functions for **maximum optimization**. Functions between `OPTMAX=:` and `OPTMAX!:` receive:

1. **AST-level algebraic optimization** before code generation.
2. **A 3-iteration fixed-point optimization pipeline** after code generation.
3. **OPTMAX isolation** — non-OPTMAX user functions cannot be called from OPTMAX functions (stdlib functions are allowed).

### 19.2 Syntax

```javascript
OPTMAX=:
fn highly_optimized(n: int) {
    var total: int = 0;
    for (i: int in 0...n) {
        total = total + i;
    }
    return total;
}
OPTMAX!:

fn main() {
    return highly_optimized(100);
}
```

### 19.3 OPTMAX Optimization Pipeline

The OPTMAX pipeline runs **3 iterations**, each consisting of 4 phases:

**Phase 1 — Early Canonicalization:**
SROA, Early CSE, mem2reg, instruction combining, reassociation, GVN, CFG simplification, dead code elimination.

**Phase 2 — Loop Optimization:**
LICM, loop rotation, loop simplification, loop instruction simplification, straightline strength reduction, loop unrolling.

**Phase 3 — Post-Loop:**
Code sinking, merged load/store motion, straightline strength reduction, n-ary reassociation, tail call elimination, constant hoisting, CFG flattening.

**Phase 4 — Final Cleanup:**
Instruction combining, CFG simplification, dead code elimination.

Each pass may expose new optimization opportunities that the next iteration can exploit.

### 19.4 Restrictions

- OPTMAX functions **can** call other OPTMAX functions.
- OPTMAX functions **can** call any stdlib built-in function (they compile to native code).
- OPTMAX functions **cannot** call non-OPTMAX user-defined functions (produces a compile error).

---

## 20. Adaptive JIT Runtime

### 20.1 Overview

OmScript is an **AOT-compiled language** — all code compiles to native machine code through LLVM. When using `omsc run`, the program executes through a lightweight **adaptive JIT runtime** that automatically recompiles hot functions with even more aggressive optimizations.

### 20.2 Two-Tier Execution Model

```
┌─────────────────────────────────────────────────────┐
│  Tier 1 — Initial JIT (fast startup)                │
│  • Module compiled at O2 via LLVM MCJIT             │
│  • Call-counting dispatch prologs injected           │
│  • Execution begins immediately                     │
└────────────────────┬────────────────────────────────┘
                     │  function call count reaches threshold
                     ▼
┌─────────────────────────────────────────────────────┐
│  Tier 2 — Adaptive Recompile (hot path)             │
│  • Clean IR parsed into fresh context               │
│  • Function annotated with real call count (PGO)    │
│  • Full O3 pipeline re-run with profile hints       │
│  • JIT-compiled via MCJIT                           │
│  • New native pointer stored for future calls       │
└─────────────────────────────────────────────────────┘
```

### 20.3 How It Works

1. **Initial compilation**: When `omsc run` is invoked, the module's LLVM IR is JIT-compiled at O2. Every non-`main` function receives a lightweight dispatch prolog that atomically increments a per-function call counter.

2. **Hot function detection**: When a function's call count first reaches the recompile threshold, the runtime triggers adaptive recompilation.

3. **Profile-guided recompilation**: The runtime:
   - Loads a clean (counter-free) copy of the module's bitcode
   - Annotates the hot function with its real call count as a PGO entry count
   - Re-runs the full LLVM O3 pipeline — the inliner, branch layout, loop vectorizer, and unroller all see the function as hot and optimize accordingly
   - JIT-compiles the result and stores the new native function pointer

4. **Fast path**: After recompilation, future calls to the function take a fast path — one volatile load and a well-predicted branch, then a direct call to the O3-PGO-optimized native code with zero counter overhead.

### 20.4 AOT Compilation Path

When compiling to an executable (the default `omsc build` path), all code is AOT-compiled through the standard LLVM pipeline:

```
Source → Lexer → Parser → AST → E-Graph (O2+) → CodeGen (LLVM IR) → LLVM Optimizer → Superoptimizer (O2+) → HGOE (O2+) → Native Object → Linker → Executable
```

The adaptive JIT runtime is only used during `omsc run` for interactive/development workflows.

---

## 21. Memory Management

### 21.1 Overview

OmScript uses **reference counting** for automatic memory management of heap-allocated data (strings).

### 21.2 Stack Allocation

Most values (integers, array slots) are **stack-allocated** via LLVM `alloca` in the function's entry block. This means:

- Arrays are stack-allocated (not heap-allocated).
- Local variables are stack-allocated.
- No heap allocation for numeric types.

### 21.3 Reference-Counted Strings

Strings use a `RefCountedString` type with the following layout:

```
┌──────────────────────────────────┐
│  StringData (heap-allocated)     │
├──────────┬───────────┬───────────┤
│ refCount │  length   │  chars[]  │
│ (size_t) │ (size_t)  │ (flex)    │
└──────────┴───────────┴───────────┘
```

**Operations:**

| Operation | Effect |
|-----------|--------|
| `allocate(str)` | `malloc` with refCount = 1 |
| Copy constructor | `retain()` → increment refCount |
| Assignment | `release()` old, `retain()` new |
| Destructor | `release()` → decrement refCount, `free` when 0 |

### 21.4 Characteristics

- **Deterministic:** Objects are freed immediately when the last reference is released.
- **No GC pauses:** No stop-the-world garbage collection.
- **Minimal overhead:** 16 bytes (two `size_t`) per unique string, plus the string data.
- **Zero-copy sharing:** Multiple references to the same string share memory.

---

## 22. CLI Reference

### 22.1 Basic Usage

```bash
omsc [command] <source.om> [options]
```

### 22.2 Commands

| Command | Aliases | Description |
|---------|---------|-------------|
| *(default)* | `compile`, `build`, `-c`, `-b`, `--compile`, `--build` | Compile source to executable |
| `run` | `-r`, `--run` | Compile and immediately run |
| `check` | `--check` | Validate syntax without compiling |
| `lex` | `tokens`, `-l`, `--lex`, `--tokens`, `--dump-tokens` | Print lexer token stream |
| `parse` | `emit-ast`, `-p`, `-a`, `--parse`, `--ast`, `--emit-ast` | Print parsed AST |
| `emit-ir` | `-e`, `-i`, `--emit-ir`, `--ir` | Print LLVM IR |
| `clean` | `-C`, `--clean` | Remove compiled outputs |
| `pkg` | `--pkg`, `package` | Package manager (install, remove, list, search, info) |
| `install` | `--install` | Install omsc to your PATH |
| `update` | `--update` | Update to latest version |
| `uninstall` | `--uninstall` | Uninstall omsc from PATH |
| `help` | `-h`, `--help` | Show help message |
| `version` | `-v`, `--version` | Show compiler version |

### 22.3 Options

| Option | Description |
|--------|-------------|
| `-o <file>`, `--output <file>` | Set output filename (default: `a.out`) |
| `-O0`, `-O1`, `-O2`, `-O3` | Optimization level (default: `-O2`) |
| `-k`, `--keep-temps` | Keep temporary files when using `run` |
| `-V`, `--verbose` | Verbose output |
| `-q`, `--quiet` | Suppress non-error output |
| `--time` | Show timing breakdown |
| `--emit-obj` | Emit object file only |
| `--dry-run` | Validate without writing files |
| `--dump-ast` | Dump AST during compilation |
| `-s`, `--strip` | Strip symbols from output |
| `-static` | Static linking |
| `--` | Separator between compiler args and runtime args |

### 22.3.1 Codegen Options

| Option | Description |
|--------|-------------|
| `-march=<cpu>` | Target CPU architecture (default: `native`) |
| `-mtune=<cpu>` | Tuning CPU (default: same as `-march`) |
| `-flto` | Full link-time optimization |
| `-ffast-math` | Unsafe floating-point optimizations |
| `-fvectorize` | SIMD vectorization hints (default: on) |
| `-funroll-loops` | Loop unrolling (default: on) |
| `-floop-optimize` | Polyhedral loop optimizations (default: on) |
| `-fpic` | Position-independent code (default: on) |
| `-foptmax` | OPTMAX block optimization (default: on) |
| `-fjit` | Adaptive JIT runtime (default: on) |
| `-fstack-protector` | Stack protection |

Use `-fno-<flag>` to disable any `-f` flag (e.g., `-fno-lto`, `-fno-vectorize`).

### 22.4 Examples

```bash
# Compile to executable
omsc program.om -o myapp

# Compile and run
omsc run program.om

# Validate syntax only
omsc check program.om

# View lexer output
omsc lex program.om

# View AST
omsc parse program.om

# View LLVM IR
omsc emit-ir program.om

# Compile and run, keep temp files
omsc run program.om --keep-temps

# Compile with aggressive optimization
omsc program.om -O3 -march=native -flto

# Show timing breakdown
omsc run program.om --time

# Package management
omsc pkg install <package>
omsc pkg list

# Clean up generated files
omsc clean program.om
```

### 22.5 Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Compilation error or runtime error (div-by-zero, OOB) |
| *N* | `main()` return value modulo 256 |

---

## 23. Building from Source

### 23.1 Prerequisites

- **CMake** 3.13 or later
- **C++17** compatible compiler (GCC, Clang)
- **LLVM** development libraries (headers + shared libraries)
- **GCC** (used as the linker for final executable output)

### 23.2 Build Steps

```bash
# Clone the repository
git clone https://github.com/RyAnPr1Me/omscript.git
cd omscript

# Create build directory
mkdir build && cd build

# Configure
cmake ..

# Build
make -j$(nproc)

# The compiler is now at ./build/omsc
```

### 23.3 CMake Configuration

| Setting | Value |
|---------|-------|
| C++ Standard | C++17 |
| Target | `omsc` executable |
| LLVM Components | core, executionengine, mcjit, interpreter, native, support, passes, target, transformutils, analysis, asmparser, codegen, mc, mcparser, bitreader |

### 23.4 Running Tests

```bash
# From repository root
bash run_tests.sh
```

The test suite verifies:
- CLI commands (help, version, lex, parse, emit-ir, build, run)
- 30+ example programs with expected return values
- Compile-failure cases (const reassignment, break outside loop, etc.)
- Optimization levels (O3 compilation)

---

## 24. Error Handling and Diagnostics

### 24.1 Compile-Time Errors

The compiler reports errors with **file, line, and column** information:

```
Error at line 4, column 12: Undefined variable 'z'
Error: Cannot modify constant variable 'PI'
Error: 'break' statement outside of loop
Error: 'continue' statement outside of loop
```

### 24.2 Runtime Errors

Runtime errors print a message to stderr and terminate with exit code 1:

```
Runtime error: division by zero
Runtime error: modulo by zero
Runtime error: array index out of bounds
```

Division by zero and modulo by zero are detected in generated code and call `exit(1)`. Array out-of-bounds triggers `llvm.trap` after printing the error.

### 24.3 Lexer Errors

```
Unterminated string literal
Unterminated block comment
Unterminated escape sequence at end of string
```

### 24.4 Code Generator Errors

```
Unknown function: nonexistent_func
Function 'add' expects 2 argument(s), but 3 provided
Built-in function 'abs' expects 1 argument, but 2 provided
OPTMAX function "optimized_fn" cannot invoke non-OPTMAX function "regular_fn"
```

---

## 25. Complete Code Examples

### 25.1 Hello World

```javascript
fn main() {
    print("hello world");
    return 0;
}
```

### 25.2 Factorial (Recursion)

```javascript
fn factorial(n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

fn main() {
    var result = factorial(5);
    print(result);     // Output: 120
    return result;     // Exit code: 120
}
```

### 25.3 Fibonacci (Iteration)

```javascript
fn fibonacci(n) {
    if (n <= 1) {
        return n;
    }
    var a = 0;
    var b = 1;
    var i = 2;
    while (i <= n) {
        var temp = a + b;
        a = b;
        b = temp;
        i = i + 1;
    }
    return b;
}

fn main() {
    return fibonacci(10);   // 55
}
```

### 25.4 GCD (Euclidean Algorithm)

```javascript
fn gcd(a, b) {
    while (b != 0) {
        var temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

fn main() {
    return gcd(48, 18);   // 6
}
```

### 25.5 Array Operations

```javascript
fn main() {
    var arr = [10, 20, 30, 40, 50];
    var total = 0;

    // Iterate with len()
    for (i in 0...len(arr)) {
        total = total + arr[i];
    }
    print(total);          // Output: 150

    // Use stdlib functions
    print(sum(arr));       // Output: 150
    swap(arr, 0, 4);       // arr = [50, 20, 30, 40, 10]
    reverse(arr);          // arr = [10, 40, 30, 20, 50]

    return sum(arr);       // 150
}
```

### 25.6 Math Stdlib Demo

```javascript
fn main() {
    var total = 0;
    total = total + abs(-10);              // 10
    total = total + min(3, 7);             // 3
    total = total + max(3, 7);             // 7
    total = total + sign(-42);             // -1
    total = total + clamp(15, 0, 10);      // 10
    total = total + pow(2, 8);             // 256
    total = total + sqrt(144);             // 12
    total = total + is_even(42);           // 1
    total = total + is_odd(7);             // 1
    return total;                          // 299
}
```

### 25.7 Character Classification

```javascript
fn main() {
    // Print "Hello" using print_char
    print_char(72);     // H
    print_char(101);    // e
    print_char(108);    // l
    print_char(108);    // l
    print_char(111);    // o
    print_char(10);     // newline

    // Check character types
    var result = 0;
    result = result + is_alpha(65);    // 1 ('A')
    result = result + is_digit(48);    // 1 ('0')
    result = result + is_alpha(48);    // 0 ('0' is not alpha)
    return result;                     // 2
}
```

### 25.8 OPTMAX Optimization

```javascript
OPTMAX=:
fn compute_sum(n: int) {
    var total: int = 0;
    for (i: int in 0...n) {
        total = total + i;
    }
    return total;
}
OPTMAX!:

fn main() {
    return compute_sum(100);    // 4950
}
```

### 25.9 All Control Flow

```javascript
fn main() {
    var result = 0;

    // If/else
    if (1) { result = result + 1; }
    else { result = result + 100; }

    // While loop
    var i = 0;
    while (i < 5) {
        result = result + 1;
        i++;
    }

    // Do-while loop
    var j = 0;
    do {
        result = result + 1;
        j++;
    } while (j < 3);

    // For loop with range
    for (k in 0...4) {
        result = result + 1;
    }

    // Break and continue
    for (m in 0...10) {
        if (m == 3) continue;
        if (m == 7) break;
        result = result + 1;
    }

    // Ternary
    result = result + ((result > 10) ? 1 : 0);

    return result;
    // 1 + 5 + 3 + 4 + 6 + 1 = 20
}
```

### 25.10 Descending Range with Step

```javascript
fn sum_down(start, end, step) {
    var total = 0;
    for (i in start...end...step) {
        total = total + i;
    }
    return total;
}

fn main() {
    return sum_down(5, 0, -1);   // 5+4+3+2+1 = 15
}
```

---

## 26. Grammar Summary

### 26.1 EBNF Grammar

```ebnf
program        = { import_stmt | enum_decl | struct_decl | function_decl } ;

import_stmt    = "import" STRING ";" ;
enum_decl      = "enum" IDENTIFIER "{" enum_member { "," enum_member } "}" ;
enum_member    = IDENTIFIER [ "=" INTEGER ] ;
struct_decl    = "struct" IDENTIFIER "{" IDENTIFIER { "," IDENTIFIER } "}" ;
function_decl  = "fn" IDENTIFIER [ "<" type_param_list ">" ]
                 "(" [ param_list ] ")" [ "->" IDENTIFIER ] block ;
type_param_list = IDENTIFIER { "," IDENTIFIER } ;
param_list     = parameter { "," parameter } ;
parameter      = IDENTIFIER [ ":" IDENTIFIER ] [ "=" literal ] ;

block          = "{" { statement } "}" ;

statement      = var_decl
               | const_decl
               | return_stmt
               | if_stmt
               | while_stmt
               | do_while_stmt
               | for_stmt
               | switch_stmt
               | try_stmt
               | break_stmt
               | continue_stmt
               | throw_stmt
               | expr_stmt
               | block ;

var_decl       = "var" IDENTIFIER [ ":" IDENTIFIER ] [ "=" expression ] ";" ;
const_decl     = "const" IDENTIFIER [ ":" IDENTIFIER ] "=" expression ";" ;
return_stmt    = "return" [ expression ] ";" ;
if_stmt        = "if" "(" expression ")" statement [ "else" statement ] ;
while_stmt     = "while" "(" expression ")" statement ;
do_while_stmt  = "do" statement "while" "(" expression ")" ";" ;
for_stmt       = "for" "(" IDENTIFIER [ ":" IDENTIFIER ] "in"
                   expression [ "..." expression [ "..." expression ] ] ")" statement ;
switch_stmt    = "switch" "(" expression ")" "{" { case_clause } [ default_clause ] "}" ;
case_clause    = "case" INTEGER ":" { statement } ;
default_clause = "default" ":" { statement } ;
try_stmt       = "try" block "catch" "(" IDENTIFIER ")" block ;
throw_stmt     = "throw" expression ";" ;
break_stmt     = "break" ";" ;
continue_stmt  = "continue" ";" ;
expr_stmt      = expression ";" ;

expression     = assignment ;
assignment     = pipe [ ( "=" | "+=" | "-=" | "*=" | "/=" | "%="
                        | "&=" | "|=" | "^=" | "<<=" | ">>=" ) assignment ]
               | field_assign ;
field_assign   = postfix "." IDENTIFIER "=" expression ;
pipe           = ternary { "|>" IDENTIFIER } ;
ternary        = null_coalesce [ "?" expression ":" ternary ] ;
null_coalesce  = logical_or { "??" logical_or } ;
logical_or     = logical_and { "||" logical_and } ;
logical_and    = bitwise_or { "&&" bitwise_or } ;
bitwise_or     = bitwise_xor { "|" bitwise_xor } ;
bitwise_xor    = bitwise_and { "^" bitwise_and } ;
bitwise_and    = equality { "&" equality } ;
equality       = comparison { ( "==" | "!=" ) comparison } ;
comparison     = shift { ( "<" | "<=" | ">" | ">=" ) shift } ;
shift          = additive { ( "<<" | ">>" ) additive } ;
additive       = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = power { ( "*" | "/" | "%" ) power } ;
power          = unary [ "**" power ] ;
unary          = ( "-" | "!" | "~" | "++" | "--" ) unary | postfix ;
postfix        = primary { "++" | "--" | "[" expression "]"
                         | "(" [ arg_list ] ")"
                         | "." IDENTIFIER } ;
primary        = INTEGER | FLOAT | STRING
               | "true" | "false" | "null"
               | IDENTIFIER
               | IDENTIFIER "{" field_init { "," field_init } "}"
               | "(" expression ")"
               | "[" [ spread_or_expr { "," spread_or_expr } ] "]"
               | lambda ;
field_init     = IDENTIFIER ":" expression ;
spread_or_expr = [ "..." ] expression ;
lambda         = "|" [ param_list ] "|" expression ;
arg_list       = expression { "," expression } ;
```

### 26.2 Token Reference

| Category | Tokens |
|----------|--------|
| **Literals** | `INTEGER`, `FLOAT`, `STRING`, `IDENTIFIER` |
| **Keywords** | `fn`, `return`, `if`, `else`, `while`, `do`, `for`, `var`, `const`, `break`, `continue`, `in`, `switch`, `case`, `default`, `try`, `catch`, `throw`, `enum`, `struct`, `import`, `true`, `false`, `null` |
| **Arithmetic** | `+`, `-`, `*`, `/`, `%`, `**` |
| **Comparison** | `==`, `!=`, `<`, `<=`, `>`, `>=` |
| **Logical** | `&&`, `\|\|`, `!` |
| **Bitwise** | `&`, `\|`, `^`, `~`, `<<`, `>>` |
| **Assignment** | `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `\|=`, `^=`, `<<=`, `>>=` |
| **Inc/Dec** | `++`, `--` |
| **Special** | `...` (range/spread), `?` (ternary), `??` (null coalesce), `\|>` (pipe), `=>` (fat arrow) |
| **Delimiters** | `(`, `)`, `{`, `}`, `[`, `]`, `;`, `,`, `:`, `.` |
| **Pragmas** | `OPTMAX=:`, `OPTMAX!:` |
| **Meta** | `END_OF_FILE`, `INVALID` |

---

*This document describes OmScript Compiler v2.3.9. For the latest updates, see the repository at [github.com/RyAnPr1Me/omscript](https://github.com/RyAnPr1Me/omscript).*
