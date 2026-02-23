# OmScript Language Reference

> **Version:** 1.0  
> **Compiler:** `omsc` — OmScript Compiler v1.0  
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
11. [Standard Library](#11-standard-library)
12. [Optimization Levels](#12-optimization-levels)
13. [OPTMAX Directive](#13-optmax-directive)
14. [Bytecode VM and Runtime](#14-bytecode-vm-and-runtime)
15. [Memory Management](#15-memory-management)
16. [CLI Reference](#16-cli-reference)
17. [Building from Source](#17-building-from-source)
18. [Error Handling and Diagnostics](#18-error-handling-and-diagnostics)
19. [Complete Code Examples](#19-complete-code-examples)
20. [Grammar Summary](#20-grammar-summary)

---

## 1. Overview

OmScript is a **low-level, C-like programming language** featuring:

- **Dynamic typing** — Variables do not require explicit type annotations (though optional type hints are supported for documentation and OPTMAX optimization).
- **Ahead-of-Time (AOT) compilation** — All code compiles to native machine code through LLVM. There is no interpreter in the default compilation path.
- **Reference-counted memory management** — Automatic deterministic deallocation via malloc/free with reference counting on strings.
- **Hybrid architecture** — A bytecode VM exists as an alternative backend for future dynamic compilation scenarios. The primary path is always native code.
- **Aggressive optimization** — Four optimization levels (O0–O3) plus a special OPTMAX directive that applies exhaustive multi-pass optimization to marked functions.
- **25 built-in standard library functions** — Math, array manipulation, string, character classification, and I/O, all compiled to native machine code.

### Design Philosophy

OmScript prioritizes **performance** and **simplicity**. The language is deliberately minimal — it provides the essential building blocks (functions, variables, loops, conditionals, arrays, basic I/O) without the overhead of classes, modules, or a garbage collector. Every stdlib function compiles directly to LLVM IR, producing native machine instructions with zero runtime dispatch overhead.

---

## 2. Compilation Pipeline

### 2.1 Pipeline Stages

The OmScript compiler (`omsc`) transforms source code to a native executable through five stages:

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
   │ CODEGEN  │  Translates AST → LLVM IR, runs optimization passes
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
| **Code Generation** | AST | LLVM IR Module | `CodeGenerator::generate()` |
| **Object Emission** | LLVM IR Module | Native object file (`.o`) | `CodeGenerator::writeObjectFile()` |
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

In the LLVM compilation path, all values are represented as `i64` (64-bit integer). This is the "default type" used throughout code generation:

- **Integers** are stored directly as `i64`.
- **Booleans** are `i64` where `0` = false, nonzero = true.
- **Pointers** (arrays, strings) are cast to `i64` via `PtrToInt` and back via `IntToPtr`.
- **Floats** are not yet fully supported in the LLVM compilation path.

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

The following 12 words are reserved:

| Keyword | Purpose |
|---------|---------|
| `fn` | Function declaration |
| `return` | Return from function |
| `if` | Conditional branch |
| `else` | Alternative branch |
| `while` | While loop |
| `do` | Do-while loop |
| `for` | For loop (range-based) |
| `var` | Variable declaration |
| `const` | Constant declaration |
| `break` | Break out of loop |
| `continue` | Skip to next iteration |
| `in` | Range iteration keyword |

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

### 6.2 The `main` Function

Every OmScript program must have a `main` function. This is the entry point. The return value of `main` becomes the process exit code (modulo 256):

```javascript
fn main() {
    return 0;    // Exit code 0
}
```

### 6.3 Function Calls

```javascript
var result = factorial(5);
print(result);
```

Functions must be declared before they are called (single-pass compilation). Recursive calls are supported.

### 6.4 Return Values

All functions return an `i64` value. If no `return` statement is reached, the function returns `0` by default.

```javascript
fn nothing() {
    // No return statement
}
// nothing() returns 0
```

### 6.5 Recursion

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
| 7 | `<<` `>>` | Left | Bitwise left shift, arithmetic right shift |
| 8 | `<` `<=` `>` `>=` | Left | Relational comparison |
| 9 | `==` `!=` | Left | Equality comparison |
| 10 | `&` | Left | Bitwise AND |
| 11 | `^` | Left | Bitwise XOR |
| 12 | `\|` | Left | Bitwise OR |
| 13 | `&&` | Left | Logical AND (short-circuit) |
| 14 | `\|\|` | Left | Logical OR (short-circuit) |
| 15 | `? :` | Right | Ternary conditional |
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
| `>>` | `a >> n` | Arithmetic right shift (sign-extending) |

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

### 7.8 Compound Assignment

| Operator | Equivalent |
|----------|------------|
| `x += y` | `x = x + y` |
| `x -= y` | `x = x - y` |
| `x *= y` | `x = x * y` |
| `x /= y` | `x = x / y` |
| `x %= y` | `x = x % y` |

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

In the current LLVM compilation path, strings are supported as:

- **Global string constants** — String literals are emitted as LLVM `CreateGlobalString` constants.
- **Print argument** — `print("message")` detects string literal arguments and uses `%s\n` format.
- **Return type** — String values are stored as pointer-to-i64 in the uniform value representation.

### 10.3 Runtime String Type

In the bytecode/VM runtime, strings are fully dynamic:

- Reference-counted with `RefCountedString` (malloc-based).
- Support concatenation (`+`), comparison (`==`, `<`), and conversion (`toString()`).
- Automatic deallocation when the reference count reaches zero.

---

## 11. Standard Library

OmScript provides **25 built-in functions**. All stdlib functions are compiled directly to native machine code via LLVM IR — they never go through the bytecode interpreter.

### 11.1 I/O Functions

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

### 11.2 Math Functions

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

### 11.3 Array Functions

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

### 11.4 Character Functions

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

### 11.5 String Functions

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

### 11.6 Utility Functions

#### `typeof(x)`

Returns a type tag for `x`. In the native LLVM compilation path, all values are represented as `i64`, so `typeof` always returns `1` (integer).

```javascript
typeof(42)         // 1  (integer)
typeof(3.14)       // 1  (always 1 in native path)
typeof("hello")    // 1  (always 1 in native path)
```

#### `assert(condition)`

Aborts the program with a runtime error message if `condition` is falsy (zero). Returns `1` if the assertion passes.

```javascript
assert(1);          // passes, returns 1
assert(x > 0);      // passes if x > 0, otherwise aborts
assert(0);          // always aborts: "Runtime error: assertion failed"
```

### 11.7 Stdlib Summary Table

| Function | Args | Returns | Description |
|----------|:----:|---------|-------------|
| `print(x)` | 1 | 0 | Print integer or string |
| `print_char(c)` | 1 | c | Print single ASCII character |
| `input()` | 0 | integer | Read integer from stdin |
| `abs(x)` | 1 | \|x\| | Absolute value |
| `min(a, b)` | 2 | min | Minimum of two values |
| `max(a, b)` | 2 | max | Maximum of two values |
| `sign(x)` | 1 | -1/0/1 | Sign of value |
| `clamp(v, lo, hi)` | 3 | clamped | Clamp to range |
| `pow(base, exp)` | 2 | base^exp | Integer exponentiation |
| `sqrt(x)` | 1 | floor(√x) | Integer square root |
| `is_even(x)` | 1 | 0/1 | Even check |
| `is_odd(x)` | 1 | 0/1 | Odd check |
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
| `typeof(x)` | 1 | 1 | Type tag (always 1/integer in native path) |
| `assert(cond)` | 1 | 1 | Abort with error if `cond` is falsy |

---

## 12. Optimization Levels

### 12.1 O0 — No Optimization

- Fastest compilation, largest and slowest output.
- LLVM IR is emitted directly without any transformation passes.
- Useful for debugging.

### 12.2 O1 — Basic Optimization

- Instruction combining and reassociation.
- CFG simplification.
- Dead code elimination.
- Minor peephole optimizations.

### 12.3 O2 — Moderate Optimization (Default)

All O1 passes plus:

- **mem2reg** — Promotes stack allocas to SSA registers.
- **SROA** — Scalar Replacement of Aggregates.
- **GVN** — Global Value Numbering (eliminates redundant computations).
- **Early CSE** — Common Subexpression Elimination.
- Additional dead code elimination passes.

### 12.4 O3 — Aggressive Optimization

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

### 12.5 Constant Folding (AST-Level)

Before LLVM optimization, the code generator performs AST-level constant folding:

- **Arithmetic:** `3 + 4` → `7`, `10 * 2` → `20`
- **Comparison:** `5 > 3` → `1`
- **Bitwise:** `0xFF & 0x0F` → `15`
- **Unary:** `-(-5)` → `5`, `!0` → `1`, `~0` → `-1`
- **Identity:** `x + 0` → `x`, `x * 1` → `x`, `x * 0` → `0`
- **Ternary:** `1 ? a : b` → `a`, `0 ? a : b` → `b`

---

## 13. OPTMAX Directive

### 13.1 Overview

`OPTMAX` is a compiler directive that marks functions for **maximum optimization**. Functions between `OPTMAX=:` and `OPTMAX!:` receive:

1. **AST-level algebraic optimization** before code generation.
2. **A 3-iteration fixed-point optimization pipeline** after code generation.
3. **OPTMAX isolation** — non-OPTMAX user functions cannot be called from OPTMAX functions (stdlib functions are allowed).

### 13.2 Syntax

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

### 13.3 OPTMAX Optimization Pipeline

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

### 13.4 Restrictions

- OPTMAX functions **can** call other OPTMAX functions.
- OPTMAX functions **can** call any stdlib built-in function (they compile to native code).
- OPTMAX functions **cannot** call non-OPTMAX user-defined functions (produces a compile error).

---

## 14. Bytecode VM and Runtime

### 14.1 Overview

OmScript includes a **stack-based bytecode virtual machine** as an alternative backend. The primary compilation path is LLVM-to-native; the bytecode VM exists for future dynamic compilation scenarios.

### 14.2 Bytecode Opcodes

| Opcode | Category | Description |
|--------|----------|-------------|
| `PUSH_INT` | Stack | Push 64-bit integer |
| `PUSH_FLOAT` | Stack | Push 64-bit float |
| `PUSH_STRING` | Stack | Push string (length-prefixed) |
| `POP` | Stack | Discard top of stack |
| `ADD` | Arithmetic | Pop two, push sum |
| `SUB` | Arithmetic | Pop two, push difference |
| `MUL` | Arithmetic | Pop two, push product |
| `DIV` | Arithmetic | Pop two, push quotient |
| `MOD` | Arithmetic | Pop two, push remainder |
| `NEG` | Arithmetic | Negate top of stack |
| `EQ` | Comparison | Pop two, push 1 if equal |
| `NE` | Comparison | Pop two, push 1 if not equal |
| `LT` | Comparison | Pop two, push 1 if less than |
| `LE` | Comparison | Pop two, push 1 if less or equal |
| `GT` | Comparison | Pop two, push 1 if greater than |
| `GE` | Comparison | Pop two, push 1 if greater or equal |
| `AND` | Logical | Pop two, push logical AND |
| `OR` | Logical | Pop two, push logical OR |
| `NOT` | Logical | Pop one, push logical NOT |
| `LOAD_VAR` | Variables | Push variable by name |
| `STORE_VAR` | Variables | Pop value, store to named variable |
| `JUMP` | Control | Unconditional jump (absolute offset) |
| `JUMP_IF_FALSE` | Control | Conditional jump if top is falsy |
| `CALL` | Control | Call function by name |
| `RETURN` | Control | Return from function |
| `HALT` | Control | Stop execution |

### 14.3 VM Architecture

```
┌─────────────────────┐
│    Value Stack       │  ← Stack-based computation
├─────────────────────┤
│  Global Variables    │  ← unordered_map<string, Value>
├─────────────────────┤
│    Last Return       │  ← Most recent return value
├─────────────────────┤
│  Bytecode Stream     │  ← Instruction pointer (ip)
└─────────────────────┘
```

### 14.4 Bytecode Encoding

All multi-byte values are encoded in **little-endian** order:

- **Integers:** 8 bytes (int64_t)
- **Floats:** 8 bytes (double, IEEE 754)
- **Strings:** 2-byte length prefix (uint16_t) + UTF-8 bytes
- **Shorts:** 2 bytes (uint16_t, used for jump offsets)

### 14.5 Stdlib in Bytecode Mode

Stdlib built-in functions are **not available** in bytecode mode. Attempting to call a stdlib function from the bytecode path produces an error:

```
Stdlib function 'abs' must be compiled to native code, not bytecode
```

This is by design — stdlib functions always take the LLVM IR → native code path for maximum performance.

---

## 15. Memory Management

### 15.1 Overview

OmScript uses **reference counting** for automatic memory management of heap-allocated data (strings).

### 15.2 Stack Allocation

Most values (integers, array slots) are **stack-allocated** via LLVM `alloca` in the function's entry block. This means:

- Arrays are stack-allocated (not heap-allocated).
- Local variables are stack-allocated.
- No heap allocation for numeric types.

### 15.3 Reference-Counted Strings

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

### 15.4 Characteristics

- **Deterministic:** Objects are freed immediately when the last reference is released.
- **No GC pauses:** No stop-the-world garbage collection.
- **Minimal overhead:** 16 bytes (two `size_t`) per unique string, plus the string data.
- **Zero-copy sharing:** Multiple references to the same string share memory.

---

## 16. CLI Reference

### 16.1 Basic Usage

```bash
omsc [command] <source.om> [options]
```

### 16.2 Commands

| Command | Aliases | Description |
|---------|---------|-------------|
| *(default)* | `compile`, `build`, `-c`, `-b`, `--compile`, `--build` | Compile source to executable |
| `run` | `-r`, `--run` | Compile and immediately run |
| `lex` | `tokens`, `-l`, `--lex`, `--tokens` | Print lexer token stream |
| `parse` | `-p`, `-a`, `--parse`, `--ast` | Print parsed AST |
| `emit-ir` | `-e`, `-i`, `--emit-ir`, `--ir` | Print LLVM IR |
| `clean` | `-C`, `--clean` | Remove compiled outputs |
| `help` | `-h`, `--help` | Show help message |
| `version` | `-v`, `--version` | Show compiler version |

### 16.3 Options

| Option | Description |
|--------|-------------|
| `-o <file>`, `--output <file>` | Set output filename (default: `a.out`) |
| `-k`, `--keep-temps` | Keep temporary files when using `run` |
| `--` | Separator between compiler args and runtime args |

### 16.4 Examples

```bash
# Compile to executable
omsc program.om -o myapp

# Compile and run
omsc run program.om

# View lexer output
omsc lex program.om

# View AST
omsc parse program.om

# View LLVM IR
omsc emit-ir program.om

# Compile and run, keep temp files
omsc run program.om --keep-temps

# Clean up generated files
omsc clean program.om
```

### 16.5 Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Compilation error or runtime error (div-by-zero, OOB) |
| *N* | `main()` return value modulo 256 |

---

## 17. Building from Source

### 17.1 Prerequisites

- **CMake** 3.13 or later
- **C++17** compatible compiler (GCC, Clang)
- **LLVM** development libraries (headers + shared libraries)
- **GCC** (used as the linker for final executable output)

### 17.2 Build Steps

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

### 17.3 CMake Configuration

| Setting | Value |
|---------|-------|
| C++ Standard | C++17 |
| Target | `omsc` executable |
| LLVM Components | core, executionengine, mcjit, interpreter, native, irreader, support, passes, target, transformutils, analysis, asmparser, codegen, mc, mcparser |

### 17.4 Running Tests

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

## 18. Error Handling and Diagnostics

### 18.1 Compile-Time Errors

The compiler reports errors with **file, line, and column** information:

```
Error at line 4, column 12: Undefined variable 'z'
Error: Cannot modify constant variable 'PI'
Error: 'break' statement outside of loop
Error: 'continue' statement outside of loop
```

### 18.2 Runtime Errors

Runtime errors print a message to stderr and terminate with exit code 1:

```
Runtime error: division by zero
Runtime error: modulo by zero
Runtime error: array index out of bounds
```

Division by zero and modulo by zero are detected in generated code and call `exit(1)`. Array out-of-bounds triggers `llvm.trap` after printing the error.

### 18.3 Lexer Errors

```
Unterminated string literal
Unterminated block comment
Unterminated escape sequence at end of string
```

### 18.4 Code Generator Errors

```
Unknown function: nonexistent_func
Function 'add' expects 2 argument(s), but 3 provided
Built-in function 'abs' expects 1 argument, but 2 provided
OPTMAX function "optimized_fn" cannot invoke non-OPTMAX function "regular_fn"
```

---

## 19. Complete Code Examples

### 19.1 Hello World

```javascript
fn main() {
    print("hello world");
    return 0;
}
```

### 19.2 Factorial (Recursion)

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

### 19.3 Fibonacci (Iteration)

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

### 19.4 GCD (Euclidean Algorithm)

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

### 19.5 Array Operations

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

### 19.6 Math Stdlib Demo

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

### 19.7 Character Classification

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

### 19.8 OPTMAX Optimization

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

### 19.9 All Control Flow

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

### 19.10 Descending Range with Step

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

## 20. Grammar Summary

### 20.1 EBNF Grammar

```ebnf
program        = { function_decl } ;
function_decl  = "fn" IDENTIFIER "(" [ param_list ] ")" block ;
param_list     = parameter { "," parameter } ;
parameter      = IDENTIFIER [ ":" IDENTIFIER ] ;

block          = "{" { statement } "}" ;

statement      = var_decl
               | const_decl
               | return_stmt
               | if_stmt
               | while_stmt
               | do_while_stmt
               | for_stmt
               | break_stmt
               | continue_stmt
               | expr_stmt
               | block ;

var_decl       = "var" IDENTIFIER [ ":" IDENTIFIER ] [ "=" expression ] ";" ;
const_decl     = "const" IDENTIFIER [ ":" IDENTIFIER ] "=" expression ";" ;
return_stmt    = "return" [ expression ] ";" ;
if_stmt        = "if" "(" expression ")" statement [ "else" statement ] ;
while_stmt     = "while" "(" expression ")" statement ;
do_while_stmt  = "do" statement "while" "(" expression ")" ";" ;
for_stmt       = "for" "(" IDENTIFIER [ ":" IDENTIFIER ] "in"
                   expression "..." expression [ "..." expression ] ")" statement ;
break_stmt     = "break" ";" ;
continue_stmt  = "continue" ";" ;
expr_stmt      = expression ";" ;

expression     = assignment ;
assignment     = ternary [ ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" ) assignment ] ;
ternary        = logical_or [ "?" expression ":" ternary ] ;
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
postfix        = primary { "++" | "--" | "[" expression "]" | "(" [ arg_list ] ")" } ;
primary        = INTEGER | FLOAT | STRING
               | IDENTIFIER
               | "(" expression ")"
               | "[" [ expression { "," expression } ] "]" ;
arg_list       = expression { "," expression } ;
```

### 20.2 Token Reference

| Category | Tokens |
|----------|--------|
| **Literals** | `INTEGER`, `FLOAT`, `STRING`, `IDENTIFIER` |
| **Keywords** | `fn`, `return`, `if`, `else`, `while`, `do`, `for`, `var`, `const`, `break`, `continue`, `in` |
| **Arithmetic** | `+`, `-`, `*`, `/`, `%` |
| **Comparison** | `==`, `!=`, `<`, `<=`, `>`, `>=` |
| **Logical** | `&&`, `\|\|`, `!` |
| **Bitwise** | `&`, `\|`, `^`, `~`, `<<`, `>>` |
| **Assignment** | `=`, `+=`, `-=`, `*=`, `/=`, `%=` |
| **Inc/Dec** | `++`, `--` |
| **Delimiters** | `(`, `)`, `{`, `}`, `[`, `]`, `;`, `,`, `:`, `.` |
| **Special** | `...` (range), `?` (ternary), `OPTMAX=:`, `OPTMAX!:` |
| **Meta** | `END_OF_FILE`, `INVALID` |

---

*This document describes OmScript Compiler v1.0. For the latest updates, see the repository at [github.com/RyAnPr1Me/omscript](https://github.com/RyAnPr1Me/omscript).*
