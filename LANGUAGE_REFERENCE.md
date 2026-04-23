# OmScript Language Reference

## Table of Contents

**Part 1 — Language Core**
1. [Overview](#1-overview)
2. [Lexical Structure](#2-lexical-structure)
3. [Preprocessor](#3-preprocessor)
4. [Type System Overview](#4-type-system-overview)
5. [Variables, Constants, and Comptime](#5-variables-constants-and-comptime)
6. [Functions](#6-functions)
7. [Control Flow](#7-control-flow)
8. [Loops](#8-loops)
9. [Operators and Expressions](#9-operators-and-expressions)
10. [Collection Literals and Indexing](#10-collection-literals-and-indexing)

**Part 2 — Standard Library and Semantics**
11. [Arrays — Complete API](#11-arrays--complete-api)
12. [Strings — Complete API](#12-strings--complete-api)
13. [Dictionaries / Maps — Complete API](#13-dictionaries--maps--complete-api)
14. [Structs](#14-structs)
15. [Enums](#15-enums)
16. [Error Handling](#16-error-handling)
17. [Memory and Ownership System](#17-memory-and-ownership-system)
18. [OPTMAX](#18-optmax)
19. [Built-in Functions](#19-built-in-functions)
20. [Concurrency](#20-concurrency)
21. [File I/O](#21-file-io)
22. [Lambda Expressions](#22-lambda-expressions)
23. [Import / Module System](#23-import--module-system)

**Part 3 — Toolchain and Internals**
24. [Compiler CLI Reference](#24-compiler-cli-reference)
25. [Compilation Pipeline (Internal)](#25-compilation-pipeline-internal)
26. [Advanced Optimization Features](#26-advanced-optimization-features)
27. [Integer Type-Cast Reference](#27-integer-type-cast-reference)
28. [CF-CTRE — Cross-Function Compile-Time Reasoning Engine](#28-cf-ctre--cross-function-compile-time-reasoning-engine)
29. [std::synthesize — Compile-Time Program Synthesis](#29-stdsynthesize--compile-time-program-synthesis)
30. [Build System and Project Layout](#30-build-system-and-project-layout)
31. [Quick-Start Cheat Sheet](#31-quick-start-cheat-sheet)
32. [Glossary](#32-glossary)
33. [Version & Compatibility](#33-version--compatibility)

---

## 1. Overview

OmScript is a statically-typed, compiled programming language designed for high-performance computing with an emphasis on optimization, control, and clarity. It compiles to native code via LLVM, offering manual control over optimization strategies while maintaining modern language ergonomics.

### Design Goals

- **Performance**: Native compilation through LLVM with aggressive optimization support
- **Control**: Fine-grained control over optimization strategies, vectorization, and memory layout
- **Safety with escape hatches**: Type safety by default with explicit mechanisms for unsafe operations
- **Ergonomics**: Modern syntax with type inference, pattern matching, and functional programming features
- **Compiler transparency**: Direct access to compiler optimization hints and runtime characteristics

### Source of Truth

This reference is derived exclusively from the OmScript compiler implementation (lexer, parser, preprocessor, semantic analyzer, and code generator). Every feature, keyword, operator, and semantic rule documented here is verified against the source code in `src/` and `include/` directories. All code examples are working programs drawn from or validated against the `examples/` test suite.

### Compilation Pipeline

OmScript source code undergoes the following compilation stages:

1. **Preprocessor** — Expands macros (`#define`), processes conditional compilation directives (`#if`, `#ifdef`), and handles file inclusion (`import`)
2. **Lexer** — Tokenizes source text into a stream of lexical tokens (keywords, identifiers, literals, operators, punctuation)
3. **Parser** — Constructs an Abstract Syntax Tree (AST) from the token stream, enforcing syntactic rules
4. **Semantic Analysis** — Validates type consistency, resolves identifiers, enforces mandatory type annotations, and checks control-flow constraints
5. **Code Generation** — Traverses the AST to emit LLVM IR, applying function annotations and optimization hints
6. **Optimization Passes** — LLVM's optimization pipeline transforms the IR (inlining, vectorization, constant folding, dead-code elimination, loop transformations)
7. **Object Code Emission** — LLVM backend generates native machine code for the target architecture
8. **Linking** — Links object files with the OmScript runtime library to produce the final executable

### High-Level Feature Map

- **Lexical structure**: Keywords, identifiers, literals (integer, float, string, bytes, interpolated), operators, comments (§2)
- **Preprocessor**: Macros, conditional compilation, predefined macros (§3)
- **Type system**: Scalar types (signed/unsigned integers, floats, bool, string), composite types (arrays, dicts, structs, enums, pointers, SIMD vectors, bigint), mandatory type annotations (§4)
- **Variables and constants**: `var`, `const`, `register var`, `global`, `comptime`, compound assignment, destructuring (§5)
- **Functions**: Declaration syntax, parameters, return types, default parameters, expression-body functions, generics, annotations (`@inline`, `@hot`, `@pure`, `@vectorize`, etc.), tail calls, lambdas (§6)
- **Control flow**: `if`/`elif`/`else`, `unless`, `guard`, `switch`, `when`, `defer`, `with`, branch hints (§7)
- **Loops**: `while`, `do`/`while`, `until`, `for` (ranges, downto, step), `foreach`, `loop`, `repeat`, `forever`, `times`, `parallel`, `pipeline`, loop annotations (`@loop(unroll=N)`, `@loop(vectorize)`) (§8)
- **Operators**: Arithmetic, comparison, logical, bitwise, null-coalescing, range, spread, pipe-forward, address-of, precedence table (§9)
- **Collections**: Array literals, indexing, slicing, dict literals, struct literals, enum access (§10)

---

## 2. Lexical Structure

### 2.1 Source Encoding and Whitespace

OmScript source files are UTF-8 encoded text. Whitespace characters (space `0x20`, tab `0x09`, carriage return `0x0D`, line feed `0x0A`) separate tokens and are otherwise ignored outside of string literals. The lexer tracks line and column numbers for diagnostic messages.

### 2.2 Comments

Two comment forms are recognized:

**Line comments**: `//` introduces a comment extending to the end of the line.

```omscript
// This is a line comment
var x: int = 42;  // Inline comment
```

**Block comments**: `/* ... */` delimits a comment that may span multiple lines. Block comments do not nest.

```omscript
/* This is a block comment
   spanning multiple lines */
var y: int = 10;

/* Inline block comment */ var z: int = 5;
```

**Unterminated block comment**: A block comment without a closing `*/` causes a lexical error.

**Documentation comments**: There is no special syntax for documentation comments (`/** ... */` is treated as a regular block comment).

### 2.3 Identifiers

An **identifier** is a sequence of characters matching the pattern:

```
[a-zA-Z_][a-zA-Z0-9_]*
```

- Must start with a letter (`a-z`, `A-Z`) or underscore (`_`)
- Subsequent characters may include digits (`0-9`)
- Case-sensitive: `myVar`, `MyVar`, and `MYVAR` are distinct identifiers

**Reserved identifiers**: All keywords (§2.4) are reserved and cannot be used as user-defined identifiers.

**Backtick-quoted identifiers**: Custom infix operators may use backtick-quoted identifiers (e.g., `` `add` ``) to define operator symbols. The identifier between backticks can contain any characters except backticks. Empty backtick identifiers are disallowed.

### 2.4 Keywords

The following identifiers are reserved as keywords. They are grouped by category for clarity:

**Control flow:**
| Keyword | Purpose |
|---------|---------|
| `if` | Conditional branch |
| `elif` | Else-if chain |
| `else` | Alternative branch |
| `unless` | Inverted conditional (sugar for `if !`) |
| `guard` | Early-exit pattern |
| `switch` | Multi-way branch |
| `case` | Switch case label |
| `when` | Pattern-matching switch |
| `default` | Default switch case |

**Loops:**
| Keyword | Purpose |
|---------|---------|
| `while` | Pre-test loop |
| `do` | Post-test loop (with `while` or `until`) |
| `until` | Inverted loop condition |
| `for` | Counted/ranged loop |
| `foreach` | Collection iteration |
| `loop` | Infinite loop or counted loop |
| `repeat` | Counted loop or post-test loop |
| `forever` | Infinite loop (alias) |
| `times` | Execute block N times |
| `parallel` | Parallelization hint for loops |
| `break` | Exit loop |
| `continue` | Skip to next iteration |

**Declarations:**
| Keyword | Purpose |
|---------|---------|
| `fn` | Function declaration |
| `var` | Mutable variable |
| `const` | Immutable constant |
| `register` | Register-allocation hint |
| `global` | Global variable scope |
| `struct` | Structure type |
| `enum` | Enumeration type |

**Exception handling:**
| Keyword | Purpose |
|---------|---------|
| `catch` | Top-level handler block: `catch(N) { ... }` (see §16) |
| `throw` | Raise an integer error code: `throw 42;` (see §16) |
| `try` | **Reserved** for future use — not currently a parser keyword (no `try { }` block exists in the language) |

**Ownership and memory:**
| Keyword | Purpose |
|---------|---------|
| `move` | Transfer ownership |
| `borrow` | Borrow reference |
| `reborrow` | Re-borrow from existing borrow |
| `mut` | Mutable borrow annotation |
| `invalidate` | Explicit invalidation |
| `freeze` | Mark variable read-only |

**Literals:**
| Keyword | Purpose |
|---------|---------|
| `true` | Boolean true |
| `false` | Boolean false |
| `null` | Null literal |

**Operators and punctuation:**
| Keyword | Purpose |
|---------|---------|
| `in` | Membership test / for-loop iterator |
| `return` | Function return |

**Compiler hints:**
| Keyword | Purpose |
|---------|---------|
| `prefetch` | Memory prefetch hint |
| `likely` | Branch likely hint |
| `unlikely` | Branch unlikely hint |
| `comptime` | Compile-time evaluation |

**Special constructs:**
| Keyword | Purpose |
|---------|---------|
| `defer` | Execute at scope exit (LIFO) |
| `with` | Scoped variable binding |
| `import` | File inclusion |
| `swap` | Swap two variables |
| `pipeline` | Staged execution pipeline |
| `stage` | Named pipeline stage |

**Optimization markers:**
| Token | Purpose |
|-------|---------|
| `OPTMAX=:` | Begin OPTMAX optimization region |
| `OPTMAX!:` | End OPTMAX optimization region |

### 2.5 Literals

#### 2.5.1 Integer Literals

An **integer literal** is a sequence of digits with optional base prefix and digit separators.

**Decimal**: `[0-9][0-9_]*`
```omscript
var a: int = 42;
var b: int = 1_000_000;  // Underscores improve readability
```

**Hexadecimal**: `0[xX][0-9a-fA-F][0-9a-fA-F_]*`
```omscript
var h: int = 0xFF;        // 255
var h2: int = 0xDEAD_BEEF; // Underscores allowed
```

**Octal**: `0[oO][0-7][0-7_]*`
```omscript
var o: int = 0o77;   // 63
var o2: int = 0o10;  // 8
```

**Binary**: `0[bB][01][01_]*`
```omscript
var b: int = 0b1111;      // 15
var b2: int = 0b1000_0000; // 128
```

**Rules**:
- Underscore `_` may appear between digits as a visual separator (not at start or end)
- Invalid underscore placement (e.g., `1__2`, `_123`, `123_`) causes a lexical error
- Integer literals are signed 64-bit by default; type annotation or context determines final width
- Out-of-range literals cause a lexical error
- Leading `0` alone is decimal zero, not octal (octal requires `0o` prefix)

**No suffix notation**: OmScript does not support literal suffixes like `42i32` or `100u64`. Use type annotations or casts instead.

#### 2.5.2 Floating-Point Literals

A **floating-point literal** contains a decimal point or exponent.

**Decimal form**: `[0-9][0-9_]*\.[0-9_]*`
```omscript
var f: float = 3.14;
var f2: float = 0.5;
var f3: float = 123.456;
```

**Scientific notation**: `[0-9][0-9_]*(\.[0-9_]*)?[eE][+-]?[0-9]+`
```omscript
var e1: float = 1e5;      // 100000.0
var e2: float = 1.5e-3;   // 0.0015
var e3: float = 2E10;     // 20000000000.0
var e4: float = 3e+2;     // 300.0
```

**Rules**:
- Must contain either a decimal point `.` or an exponent `e`/`E`
- Underscores allowed in digit sequences (same rules as integers)
- Exponent may have optional `+` or `-` sign
- Out-of-range literals cause a lexical error
- Default type is `f64` (double-precision); type annotation determines final precision

#### 2.5.3 Boolean Literals

**Boolean literals**: `true` and `false` (keywords)

```omscript
var flag: bool = true;
var done: bool = false;
```

Type: `bool` (1-bit logical value, represented as `i1` in LLVM)

#### 2.5.4 Character Literals

OmScript **does not have a distinct character literal syntax** like `'c'`. Single characters are represented as:
- Single-character strings: `"A"`
- Integer character codes: `65` (ASCII 'A')
- The `char_at` function extracts characters from strings
- The `char_code` function converts characters to ASCII codes

#### 2.5.5 String Literals

A **string literal** is a sequence of characters enclosed in double quotes `"..."`.

**Basic form**:
```omscript
var s: string = "hello, world";
var empty: string = "";
var quote: string = "She said \"hi\"";
```

**Multi-line strings**: Triple-quoted strings preserve literal newlines.
```omscript
var poem: string = """
    Roses are red,
    Violets are blue,
    OmScript compiles fast,
    And optimizes too.
""";
```

**Escape sequences**:
| Escape | Meaning | Byte Value |
|--------|---------|------------|
| `\n` | Newline (LF) | `0x0A` |
| `\t` | Horizontal tab | `0x09` |
| `\r` | Carriage return | `0x0D` |
| `\b` | Backspace | `0x08` |
| `\f` | Form feed | `0x0C` |
| `\v` | Vertical tab | `0x0B` |
| `\\` | Backslash | `0x5C` |
| `\"` | Double quote | `0x22` |
| `\xHH` | Hex byte (two hex digits) | Variable |

**Forbidden escapes**:
- `\0` (null byte) is rejected — would truncate C-string representation at runtime
- `\x00` (null byte via hex escape) is rejected — same rationale
- Unknown escape sequences (e.g., `\q`) cause a lexical error

**Unterminated string**: Missing closing `"` causes a lexical error.

#### 2.5.6 Bytes Literals

A **bytes literal** is a hexadecimal byte array: `0x"AABBCC..."`.

**Syntax**: `0x"` followed by pairs of hexadecimal digits (optionally separated by whitespace or underscores), terminated by `"`.

```omscript
var data: int[] = 0x"DEADBEEF";      // [0xDE, 0xAD, 0xBE, 0xEF]
var spaced: int[] = 0x"01 02 03 04"; // [0x01, 0x02, 0x03, 0x04]
var short: int[] = 0x"FF";           // [0xFF]
```

**Rules**:
- Each byte requires **exactly two** hex digits (`0-9`, `a-f`, `A-F`)
- Odd digit count causes a lexical error
- Whitespace (` `, `\t`) and underscores `_` are ignored as separators
- Result is an array of `u8` values
- Length is half the number of hex digits

#### 2.5.7 Interpolated Strings

An **interpolated string** embeds expressions in a string template: `$"text {expr} text"`.

**Syntax**: `$"` opens an interpolated string. Curly braces `{...}` enclose expressions to be converted to strings and concatenated.

```omscript
var name: string = "Alice";
var age: int = 30;
var msg: string = $"Hello, {name}! You are {age} years old.";
// Result: "Hello, Alice! You are 30 years old."
```

**Desugaring**: Interpolated strings desugar to concatenation chains:
```omscript
$"Hello {x}" 
// becomes:
"" + "Hello " + str(x)
```

The leading empty string ensures string concatenation context, so numeric expressions are auto-converted via `to_string`.

**Escaping braces**: Literal `{` and `}` require backslash escapes:
```omscript
var code: string = $"Use \{curly\} braces for blocks";
// Result: "Use {curly} braces for blocks"
```

**Complex expressions**: Any expression is valid inside `{...}`:
```omscript
var x: int = 5;
var s: string = $"x squared is {x * x}";            // "x squared is 25"
var t: string = $"flag is {x > 3 ? "yes" : "no"}";  // "flag is yes"
var arr: int[] = [1, 2, 3];
var info: string = $"length is {len(arr)}";         // "length is 3"
```

**Nested interpolations**: Not supported directly; use intermediate variables.

#### 2.5.8 Null Literal

The **null literal** is the keyword `null`.

```omscript
var p: ptr = null;  // Null pointer
```

Type: Context-dependent (typically pointer types). The semantics of `null` depend on the type system and are implementation-defined.

### 2.6 Operators and Punctuation

The following tokens represent operators and punctuation. They are listed with their symbolic forms and semantic categories (precise precedence and semantics in §9).

**Arithmetic operators:**
| Token | Symbol | Name |
|-------|--------|------|
| `PLUS` | `+` | Addition |
| `MINUS` | `-` | Subtraction / Unary negation |
| `STAR` | `*` | Multiplication |
| `STAR_STAR` | `**` | Exponentiation |
| `SLASH` | `/` | Division |
| `PERCENT` | `%` | Modulus |

**Comparison operators:**
| Token | Symbol | Name |
|-------|--------|------|
| `EQ` | `==` | Equality |
| `NE` | `!=` | Inequality |
| `LT` | `<` | Less than |
| `LE` | `<=` | Less than or equal |
| `GT` | `>` | Greater than |
| `GE` | `>=` | Greater than or equal |

**Logical operators:**
| Token | Symbol | Name |
|-------|--------|------|
| `AND` | `&&` | Logical AND (short-circuit) |
| `OR` | `||` | Logical OR (short-circuit) |
| `NOT` | `!` | Logical NOT |

**Bitwise operators:**
| Token | Symbol | Name |
|-------|--------|------|
| `AMPERSAND` | `&` | Bitwise AND / Address-of |
| `PIPE` | `|` | Bitwise OR |
| `CARET` | `^` | Bitwise XOR |
| `TILDE` | `~` | Bitwise NOT |
| `LSHIFT` | `<<` | Left shift |
| `RSHIFT` | `>>` | Right shift (arithmetic for signed, logical for unsigned) |

**Assignment operators:**
| Token | Symbol | Name |
|-------|--------|------|
| `ASSIGN` | `=` | Assignment |
| `PLUS_ASSIGN` | `+=` | Add-assign |
| `MINUS_ASSIGN` | `-=` | Subtract-assign |
| `STAR_ASSIGN` | `*=` | Multiply-assign |
| `SLASH_ASSIGN` | `/=` | Divide-assign |
| `PERCENT_ASSIGN` | `%=` | Modulus-assign |
| `AMPERSAND_ASSIGN` | `&=` | Bitwise AND-assign |
| `PIPE_ASSIGN` | `|=` | Bitwise OR-assign |
| `CARET_ASSIGN` | `^=` | Bitwise XOR-assign |
| `LSHIFT_ASSIGN` | `<<=` | Left-shift-assign |
| `RSHIFT_ASSIGN` | `>>=` | Right-shift-assign |
| `STAR_STAR_ASSIGN` | `**=` | Exponent-assign |
| `NULL_COALESCE_ASSIGN` | `??=` | Null-coalesce-assign |
| `AND_ASSIGN` | `&&=` | Logical AND-assign |
| `OR_ASSIGN` | `||=` | Logical OR-assign |

**Increment/decrement:**
| Token | Symbol | Name |
|-------|--------|------|
| `PLUSPLUS` | `++` | Increment (prefix/postfix) |
| `MINUSMINUS` | `--` | Decrement (prefix/postfix) |

**Special operators:**
| Token | Symbol | Name |
|-------|--------|------|
| `QUESTION` | `?` | Ternary conditional |
| `NULL_COALESCE` | `??` | Null coalescing |
| `PIPE_FORWARD` | `|>` | Pipe-forward (function application) |
| `SPREAD` | `...` | Spread operator |
| `RANGE` | `...` | Inclusive range |
| `DOT_DOT` | `..` | Exclusive range |
| `ARROW` | `->` | Function return type |
| `FAT_ARROW` | `=>` | Lambda expression (in pattern matching) |
| `SCOPE` | `::` | Scope resolution |
| `DOT` | `.` | Member access |

**Delimiters:**
| Token | Symbol | Name |
|-------|--------|------|
| `LPAREN` | `(` | Left parenthesis |
| `RPAREN` | `)` | Right parenthesis |
| `LBRACE` | `{` | Left brace |
| `RBRACE` | `}` | Right brace |
| `LBRACKET` | `[` | Left bracket |
| `RBRACKET` | `]` | Right bracket |
| `SEMICOLON` | `;` | Statement terminator |
| `COMMA` | `,` | Separator |
| `COLON` | `:` | Type annotation |
| `AT` | `@` | Annotation prefix |

**Note on `...` vs `..`**: Both tokens share the first two characters. The lexer disambiguates by longest-match: if three dots appear consecutively, the token is `RANGE` (`...`); otherwise two dots form `DOT_DOT` (`..`).

### 2.7 Reserved and Contextual Tokens

**OPTMAX markers**: The lexer recognizes two special 8-character sequences used to delimit optimization regions:
- `OPTMAX=:` — Begin OPTMAX region (token `OPTMAX_START`)
- `OPTMAX!:` — End OPTMAX region (token `OPTMAX_END`)

These tokens are **reserved** and may not appear in ordinary expressions or identifiers. They are used exclusively to bracket high-optimization function definitions.

**Example**:
```omscript
OPTMAX=:
fn fast_sum(n: int) -> int {
    var total: int = 0;
    for (i: int in 0...n) {
        total = total + i;
    }
    return total;
}
OPTMAX!:
```

**Backtick identifiers**: Identifiers enclosed in backticks (`` `name` ``) are contextual tokens used for defining custom infix operators. They are scanned as `BACKTICK_IDENT` tokens. See §6 for usage in operator definitions.

**Contextual keywords (not in the lexer's keyword table):** A handful of names act as keywords *only inside specific grammar productions* — outside those contexts they are ordinary identifiers and may be used as variable / function names:

| Word | Where it is contextual | Outside that context |
|------|------------------------|----------------------|
| `downto` | Second token of a `for (i in HI downto LO ...)` range loop (§8.5) | Plain identifier |
| `step` | Optional trailing modifier of a `for (i in A...B step N)` loop (§8.5) | Plain identifier |
| `deopt` | Optional marker in `assume(c) else deopt { ... }` (§7.10) | Plain identifier |
| `as` | Import alias (`import "x.om" as foo`) and explicit casts | Plain identifier |
| `from` | Inside specific destructuring forms | Plain identifier |

These do not appear in the keyword table in §2.4 and you may shadow them, but doing so usually hurts readability. Treat them as reserved in idiomatic code.

---

## 3. Preprocessor

The **preprocessor** runs before lexical analysis, transforming source text by expanding macros, resolving conditional compilation directives, and performing file inclusion. Preprocessor directives begin with `#` at the start of a line (leading whitespace is allowed).

### 3.1 Directive List

#### 3.1.1 `#define` — Define Macro

**Object-like macro** (constant substitution):
```omscript
#define PI 3.14159
#define MAX_SIZE 1024
```

**Function-like macro** (parameterized):
```omscript
#define SQUARE(x) x * x
#define MAX(a, b) ((a) > (b) ? (a) : (b))
```

**Syntax**:
- Object-like: `#define NAME body`
- Function-like: `#define NAME(param1, param2, ...) body`
- No space allowed between `NAME` and `(` in function-like macros
- Macro body extends to end of line (or use `\` for continuation)
- Macros are substituted in subsequent source text
- Recursive expansion is supported (depth limit: 64)

**Stringification and token pasting**: Not currently implemented. Macros perform textual substitution only.

#### 3.1.2 `#undef` — Undefine Macro

Removes a macro definition:
```omscript
#define TEMP 42
#undef TEMP
// TEMP is no longer defined
```

#### 3.1.3 `#if` / `#elif` / `#else` / `#endif` — Conditional Compilation

Conditionally include source text based on compile-time expressions:
```omscript
#define DEBUG 1
#if DEBUG
    println("Debug mode enabled");
#elif RELEASE
    println("Release mode");
#else
    println("Unknown mode");
#endif
```

**Evaluation**:
- `#if` and `#elif` conditions are integer expressions evaluated at preprocessing time
- Macros in conditions are expanded before evaluation
- Supported operators: `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`, `!`, `(`, `)`
- `defined(NAME)` checks if macro `NAME` is defined (returns 1 or 0)
- Undefined identifiers evaluate to 0

**Example with `defined`**:
```omscript
#if defined(WINDOWS) && !defined(DEBUG)
    #define LOG_FILE "release.log"
#endif
```

#### 3.1.4 `#ifdef` / `#ifndef` — Test Macro Definition

Shorthand for testing macro presence:
```omscript
#ifdef DEBUG
    println("Debugging");
#endif

#ifndef NDEBUG
    println("Assertions enabled");
#endif
```

Equivalent to:
- `#ifdef X` → `#if defined(X)`
- `#ifndef X` → `#if !defined(X)`

#### 3.1.5 `#error` — Emit Compilation Error

Causes compilation to fail with a message:
```omscript
#ifndef VERSION
    #error "VERSION macro must be defined"
#endif
```

#### 3.1.6 `#warning` — Emit Compilation Warning

Emits a warning message (compilation continues):
```omscript
#warning "This feature is experimental"
```

#### 3.1.7 `#line` — Set Line Number (RESERVED — NOT IMPLEMENTED)

**Status:** Reserved syntactically. The current preprocessor (`src/preprocessor.cpp`) does **not** recognize `#line` as a known directive — it falls through to the generic "unknown preprocessor directive" warning. Generated-code line attribution must be done via host-side rewriting until this directive is wired up.

#### 3.1.8 `#info` — Emit Informational Message

Like `#warning` but tagged as `info:` instead of `warning:`. Compilation continues. Use for benign build-time annotations (which optimization tier was selected, which feature flags are on, etc.):
```omscript
#info "fast-path build enabled"
```
Implementation: appended to the preprocessor's accumulated warnings list and surfaced through the same diagnostic channel as `#warning`.

#### 3.1.9 `#assert` — Compile-Time Assertion

**Syntax:**
```ebnf
assert_directive ::= '#assert' const_expression [ '"' message '"' ]
```

Evaluates `const_expression` using the same `#if` expression evaluator (§3.5). If the result is `0`, the build fails with a diagnostic of the form `#assert failed: <message>` (or `#assert failed: compile-time assertion failed: <expression>` if no message is given).

```omscript
#assert __VERSION__ >= 2 "OmScript ≥ 2 required"
#assert defined(TARGET_X86_64) "this module is x86-64-only"
```

Use for build-time invariants (feature-flag combinations, ABI assumptions) that should fail loudly rather than silently mis-compile.

#### 3.1.10 `#require` — Minimum Compiler Version

**Syntax:**
```ebnf
require_directive ::= '#require' '"' version_string '"'
```

Fails the build if the running compiler's `__VERSION__` is older than `version_string` (string-comparison via the compiler's internal `cmpVersion`). Emits an error of the form `#require: compiler version <current> is older than required <X.Y.Z>`.

```omscript
#require "1.2.0"
```

#### 3.1.11 `#counter` — Define an Auto-Incrementing Macro

**Syntax:**
```ebnf
counter_directive ::= '#counter' identifier
```

Creates a macro that yields a fresh, monotonically increasing integer on every textual expansion (starts at `0`). Useful for generating unique IDs in macro-heavy code:
```omscript
#counter UNIQ
const int id1 = UNIQ;   // 0
const int id2 = UNIQ;   // 1
const int id3 = UNIQ;   // 2
```
Distinct from the predefined `__COUNTER__` macro (§3.3) — `__COUNTER__` is a single global counter shared across the whole translation unit, while `#counter` lets you declare *named* counters with independent state.

#### 3.1.12 `#pragma` — Vendor-Specific Hints (Currently a No-Op)

`#pragma` lines are accepted by the preprocessor and silently consumed. No pragma names are currently recognized — the directive is reserved for forward-compatible insertion of compiler-specific hints without breaking older builds.

```omscript
#pragma optimize_for_size   // accepted but ignored today
```

#### 3.1.13 `import` — File Inclusion

Includes another OmScript source file (processed by the parser, not the preprocessor directly, but conceptually similar to `#include`):
```omscript
import "utilities.om"
import "math_helpers.om" as math
```

**Syntax**:
- `import "path"` — Include file at `path` (relative to current file)
- `import "path" as alias` — Include file and namespace it under `alias`
- Circular imports are detected and rejected
- Imported files are preprocessed and parsed recursively

**Note**: `import` is technically a parser-level keyword, not a preprocessor directive, but it is conceptually similar to `#include` in C.

### 3.2 Macro Expansion Rules

**Textual substitution**: Macro bodies are substituted verbatim at the point of invocation.

**Function-like macros**: Arguments are collected by matching parentheses, separated by commas. Nested parentheses and string literals are handled correctly.

**Recursion**: Macros may expand recursively up to a depth of 64. Circular macros cause infinite recursion and trigger an error at the depth limit.

**Example**:
```omscript
#define ADD(a, b) a + b
#define DOUBLE(x) ADD(x, x)

var result = DOUBLE(5);  // Expands to: 5 + 5
```

**Stringification (`#`) and token pasting (`##`)**: Implemented for
function-like macros.

* `#param` in the macro body is replaced by a string literal whose contents
  are the textual argument with leading/trailing whitespace trimmed and
  embedded `"` / `\` escaped:

  ```omscript
  #define STR(x) #x
  var s:string = STR(world);   // expands to: var s:string = "world";
  ```

* `a ## b` collapses two tokens into one by removing the `##` and any
  surrounding whitespace.  Multiple consecutive pastes are supported:

  ```omscript
  #define CONCAT(a, b) a ## b
  var CONCAT(my, Var):i64 = 42;   // expands to: var myVar:i64 = 42;
  ```

### 3.3 Predefined Macros

The preprocessor defines the following macros automatically:

| Macro | Value | Description |
|-------|-------|-------------|
| `__FILE__` | `"filename"` | Current source file name (string) |
| `__LINE__` | `123` | Current line number (integer) |
| `__VERSION__` | `"0.1.0"` | OmScript compiler version (string) |
| `__OS__` | `"linux"` / `"windows"` / `"macos"` | Target operating system (string) |
| `__ARCH__` | `"x86_64"` / `"aarch64"` / `"arm"` | Target architecture (string) |
| `__COUNTER__` | `0`, `1`, ... | Global counter, increments on each use (integer) |

**Example**:
```omscript
#define LOG(msg) println(__FILE__, ":", __LINE__, " - ", msg)

fn main() {
    LOG("Starting program");  // Outputs: "example.om:5 - Starting program"
}
```

**`__COUNTER__` macro**: Special predefined macro that increments globally each time it is referenced:
```omscript
var id1 = __COUNTER__;  // 0
var id2 = __COUNTER__;  // 1
var id3 = __COUNTER__;  // 2
```

**Custom counter macros**: Use `#counter NAME` to define a named counter:
```omscript
#counter MY_ID
var a = MY_ID;  // 0
var b = MY_ID;  // 1
var c = MY_ID;  // 2
```

### 3.4 Line Continuation

A backslash `\` at the end of a line continues the preprocessor directive to the next line:
```omscript
#define LONG_MACRO \
    some_function(arg1, \
                  arg2, \
                  arg3)
```

**Note**: Line continuation is implemented in the preprocessor; the lexer sees the joined line as a single logical line.

### 3.5 Conditional Expressions Allowed in `#if`

The preprocessor supports a subset of C-like expression syntax in `#if` and `#elif` conditions:

**Operators** (in precedence order, high to low):
1. Grouping: `(` `)`
2. Unary: `!`, `-`, `+`
3. Multiplicative: `*`, `/`, `%`
4. Additive: `+`, `-`
5. Relational: `<`, `<=`, `>`, `>=`
6. Equality: `==`, `!=`
7. Logical AND: `&&`
8. Logical OR: `||`

**`defined(NAME)` operator**: Returns 1 if macro `NAME` is defined, 0 otherwise. Can be used with or without parentheses:
```omscript
#if defined(DEBUG)
#if defined DEBUG
```

**Integer literals**: Decimal, hex (`0x...`), octal (`0o...`), binary (`0b...`) literals are supported.

**Undefined identifiers**: Expand to `0`.

**Example**:
```omscript
#define VERSION 2
#if VERSION >= 2 && defined(FEATURE_X)
    // Code for version 2+ with FEATURE_X
#endif
```

---

## 4. Type System Overview

OmScript is **statically typed** with **mandatory type annotations** on variable declarations (with limited inference). The type system includes scalar types (integers, floats, booleans, strings), composite types (arrays, dictionaries, structs, enums), pointer types, SIMD vectors, and arbitrary-precision integers (bigint).

### 4.1 Type Annotation Syntax

Type annotations use the colon `:` separator: `name: type`.

**Variable declarations**:
```omscript
var x: int = 42;
var s: string = "hello";
```

**Function parameters**:
```omscript
fn add(a: int, b: int) -> int {
    return a + b;
}
```

**Return types**: Arrow `->` precedes the return type.

### 4.2 Mandatory Type Annotations on Declarations

**User-written variable declarations require explicit type annotations**. The compiler enforces this rule to ensure clarity and prevent accidental type mismatches.

**Valid**:
```omscript
var x: int = 10;
const pi: float = 3.14;
```

**Invalid** (causes a parse error):
```omscript
var y = 20;       // Error: missing type annotation
const name = "Alice";  // Error: missing type annotation
```

**Exceptions**:
- Compiler-generated variables (e.g., for-loop iterators, destructuring temporaries) are exempt
- Variables with inherited type from multi-declaration (see §5.3)

**Recent enforcement**: The mandatory type annotation rule was recently added; older code examples in the test suite may omit annotations, but current compiler versions require them.

### 4.3 Scalar Types

#### Integer Types

OmScript provides signed and unsigned integer types of various widths:

| Type | Width | Signedness | Range | LLVM Type |
|------|-------|------------|-------|-----------|
| `i8` | 8 bits | Signed | -128 to 127 | `i8` |
| `i16` | 16 bits | Signed | -32,768 to 32,767 | `i16` |
| `i32` | 32 bits | Signed | -2³¹ to 2³¹-1 | `i32` |
| `i64` | 64 bits | Signed | -2⁶³ to 2⁶³-1 | `i64` |
| `u8` | 8 bits | Unsigned | 0 to 255 | `i8` |
| `u16` | 16 bits | Unsigned | 0 to 65,535 | `i16` |
| `u32` | 32 bits | Unsigned | 0 to 2³²-1 | `i32` |
| `u64` | 64 bits | Unsigned | 0 to 2⁶⁴-1 | `i64` |
| `int` | 64 bits | Signed | Alias for `i64` | `i64` |
| `uint` | 64 bits | Unsigned | Alias for `u64` | `i64` |

**Arbitrary-width integers**: Types `i1` through `i256` and `u1` through `u256` are also recognized (e.g., `i37`, `u100`), allowing precise bit-width control.

**Default type**: Unadorned integer literals default to `i64` unless context requires otherwise.

**Signedness and operations**:
- Signed integers use two's complement representation
- Unsigned integers use zero-extension
- Division `/` and modulus `%` on signed integers truncate toward zero
- Right-shift `>>` is arithmetic (sign-extending) for signed integers, logical (zero-filling) for unsigned

**Overflow behavior**: Integer overflow is **undefined behavior** for signed integers (enabling optimizations); unsigned integers wrap modulo 2ⁿ.

#### Floating-Point Types

| Type | Width | Precision | Range | LLVM Type |
|------|-------|-----------|-------|-----------|
| `f32` | 32 bits | Single (IEEE 754) | ≈1.2e-38 to ≈3.4e38 | `float` |
| `f64` | 64 bits | Double (IEEE 754) | ≈2.2e-308 to ≈1.8e308 | `double` |
| `float` | 64 bits | Alias for `f64` | Same as `f64` | `double` |
| `double` | 64 bits | Alias for `f64` | Same as `f64` | `double` |

**Default type**: Float literals default to `f64`.

**NaN and infinity**: IEEE 754 special values (NaN, ±∞) are supported. Use `is_nan(x)` and `is_inf(x)` predicates to test.

**Fast math**: Functions annotated with `@fastmath` may reorder floating-point operations, enable algebraic reassociation, and assume no NaN/Inf values (trading precision for speed).

#### Boolean Type

| Type | Width | Values | LLVM Type |
|------|-------|--------|-----------|
| `bool` | 1 bit (logical) | `true`, `false` | `i1` |

**Memory representation**: `bool` occupies 1 byte in memory (for alignment) but is represented as `i1` in LLVM (1-bit integer).

**Conversion**: Integer-to-bool: `0` → `false`, non-zero → `true`. Bool-to-integer: `false` → `0`, `true` → `1`.

#### String Type

| Type | Representation | Encoding | LLVM Type |
|------|----------------|----------|-----------|
| `string` | Heap-allocated, immutable | UTF-8 | `ptr` (opaque) |

**Semantics**:
- Strings are immutable heap objects (reference-counted in the runtime)
- Indexing `s[i]` returns an integer ASCII/UTF-8 byte value
- Concatenation `s1 + s2` creates a new string
- String length `len(s)` returns the byte count (not character count if multibyte UTF-8)
- Empty string `""` is a valid string

**String interning**: Compile-time string literals may be interned (implementation detail).

#### Character and Byte Types

OmScript **does not have dedicated `char` or `byte` types**. Characters are represented as:
- Single-character strings: `"A"`
- Integer ASCII/UTF-8 codes: `65`

The `char_at(s, i)` function extracts a single-character string from `s[i]`. The `char_code(s)` function returns the integer code of the first byte.

#### Void Type

| Type | Meaning | LLVM Type |
|------|---------|-----------|
| `void` | No value / absent return | `void` |

**Usage**: Functions without a `return` statement or with `return;` (no expression) have return type `void`.

```omscript
fn print_hello() -> void {
    println("Hello");
}
```

**Implicit `void`**: If no return type is specified and the function lacks a `return` statement, the return type is inferred as `void`.

### 4.4 Composite Types

#### 4.4.1 Array Type: `T[]`

**Syntax**: `T[]` denotes an array of elements of type `T`.

**Representation**: Heap-allocated, dynamically-sized array (reference-counted).

**LLVM type**: Opaque pointer `ptr` to runtime array object.

**Operations**:
- Indexing: `arr[i]` (zero-based)
- Slicing: `arr[i..j]` (half-open range), `arr[i...j]` (closed range)
- Length: `len(arr)`
- Mutation: `arr[i] = value` (element assignment)
- Concatenation: `arr1 + arr2` (creates new array)

**Example**:
```omscript
var nums: int[] = [1, 2, 3, 4, 5];
var first: int = nums[0];         // 1
var slice: int[] = nums[1..3];    // [2, 3]
var length: int = len(nums);      // 5
```

**Multi-dimensional arrays**: `T[][]` (array of arrays):
```omscript
var matrix: int[][] = [[1, 2], [3, 4]];
var cell: int = matrix[0][1];  // 2
```

#### 4.4.2 Dictionary Type: `dict` / `dict[K, V]`

**Syntax**:
- `dict` (untyped dictionary)
- `dict[K, V]` (typed dictionary with key type `K` and value type `V`) — **not yet fully implemented**; use `dict`

**Representation**: Hash map (heap-allocated, reference-counted).

**LLVM type**: Opaque pointer `ptr` to runtime map object.

**Operations**:
- Creation: `map_new()` or literal `{ k1: v1, k2: v2, ... }`
- Access: `map_get(d, key, default)`
- Mutation: `map_set(d, key, value)` (returns new map)
- Check: `map_has(d, key)` (returns 1 or 0)
- Removal: `map_remove(d, key)` (returns new map)
- Size: `map_size(d)`
- Keys: `map_keys(d)` (returns array of keys)
- Values: `map_values(d)` (returns array of values)

**Example**:
```omscript
var ages: dict = { "Alice": 30, "Bob": 25 };
var alice_age: int = map_get(ages, "Alice", 0);  // 30
ages = map_set(ages, "Charlie", 35);
var has_bob: int = map_has(ages, "Bob");         // 1
```

#### 4.4.3 SIMD Vector Types: `vec4<f32>`, `vec8<i32>`, etc.

**Status**: SIMD vector types are **mentioned in comments** but **not currently implemented** in the parser or type system. This is a future feature.

**Anticipated syntax**: `vecN<T>` where `N` is 2, 4, 8, 16, etc., and `T` is a scalar type (e.g., `vec4<f32>` for 4-element float vector).

#### 4.4.4 Pointer Type: `ptr` / `ptr<T>`

**Syntax**:
- `ptr` — Generic pointer (element type unknown)
- `ptr<T>` — Typed pointer to elements of type `T`

**Representation**: Raw memory address (64-bit on most architectures).

**LLVM type**: `ptr` (LLVM opaque pointer)

**Operations**:
- Address-of: `&x` (produces `ptr<T>` if `x` is type `T`)
- Dereference: **Not directly supported** (use runtime functions)
- Null: `null` literal

**Safety**: Pointers are **unsafe**. The compiler does not track aliasing or lifetime. Use `borrow` and `move` for safer alternatives.

**Example**:
```omscript
var x: int = 42;
var p: ptr<int> = &x;  // Pointer to x
```

#### 4.4.5 Reference Type: `ref` / `&T`

**Status**: Reference types are **mentioned in comments** (e.g., `reborrow ref = &src`) but **not fully formalized** in the type system. This is a future feature or internal representation.

**Syntax**: `&T` may denote a borrowed reference to `T`.

**Semantics**: References are similar to pointers but enforce borrow-checking rules (see §5 for `borrow` and `move` keywords).

#### 4.4.6 Struct Type

**Syntax**: `struct Name { field1, field2, ... }`

**Declaration**:
```omscript
struct Point {
    x,
    y
}
```

**Fields are untyped** in the declaration (dynamic typing); type information is inferred from usage.

**Representation**: Opaque pointer to heap-allocated struct object (reference-counted).

**LLVM type**: Opaque `ptr`.

**Operations**:
- Creation: `Point { x: 10, y: 20 }`
- Field access: `p.x`, `p.y`
- Field assignment: `p.x = 30`

**Example**:
```omscript
struct Point { x, y }

fn main() {
    var p: Point = Point { x: 10, y: 20 };
    println(p.x);  // 10
    p.y = 25;
    println(p.y);  // 25
}
```

#### 4.4.7 Enum Type

**Syntax**: `enum Name { VARIANT1, VARIANT2 = value, ... }`

**Declaration**:
```omscript
enum Color {
    RED,           // Auto-assigned 0
    GREEN = 10,    // Explicit value
    BLUE           // Auto-assigned 11
}
```

**Variants** are global integer constants. Naming convention: `EnumName_VARIANT`.

**Access**: `Color_RED`, `Color_GREEN`, `Color_BLUE`

**Scope resolution** (alternative syntax): `Color::RED` (desugars to `Color_RED`).

**Example**:
```omscript
enum Status {
    OK = 0,
    ERROR = 1
}

fn main() {
    var s: int = Status_OK;
    if (s == Status_OK) {
        println("Success");
    }
}
```

#### 4.4.8 BigInt Type

**Syntax**: `bigint`

**Representation**: Heap-allocated arbitrary-precision integer (opaque pointer to GMP `mpz_t` or similar).

**LLVM type**: Opaque `ptr`.

**Operations**: Provided by runtime functions (see §10 for function list):
- Creation: `bigint(value)`, `bigint("123456789012345678901234567890")`
- Arithmetic: `bigint_add`, `bigint_sub`, `bigint_mul`, `bigint_div`, `bigint_mod`, `bigint_pow`
- Comparison: `bigint_cmp`, `bigint_eq`, `bigint_lt`, `bigint_le`, `bigint_gt`, `bigint_ge`
- Conversion: `bigint_tostring`, `bigint_to_i64`
- Bitwise: `bigint_shl`, `bigint_shr`, `bigint_bit_length`
- Predicates: `bigint_is_zero`, `bigint_is_negative`

**Example**:
```omscript
var big1: bigint = bigint("99999999999999999999");
var big2: bigint = bigint("11111111111111111111");
var sum: bigint = bigint_add(big1, big2);
println(bigint_tostring(sum));  // "111111111111111111110"
```

### 4.5 Type Inference

OmScript supports **limited type inference**:

**Where inference is allowed**:
- Function parameter types may be omitted if unused in the function body (rare; not recommended)
- Lambda parameters: types are inferred from context (e.g., `|x| x * 2` infers `x` from usage)
- Expression types: intermediate expression types are inferred (e.g., `var x: int = 1 + 2;` infers `1 + 2` as `int`)

**Where inference is forbidden**:
- Variable declarations: **explicit type annotation required** (user-written `var`/`const` statements)
- Function return types: **explicit annotation recommended** (omission may infer `void`)

**Type propagation in multi-variable declarations**: In multi-variable declarations, the type annotation on the first variable propagates to subsequent variables without explicit annotation (see §5.3).

---

## 5. Variables, Constants, and Comptime

### 5.1 `var` Declaration

**Syntax**: `var name: type = initializer;`

**Semantics**:
- Declares a **mutable** variable
- Type annotation is **mandatory** (see §4.2)
- Initializer is **optional**; uninitialized variables have undefined value (implementation may zero-initialize)

**Examples**:
```omscript
var x: int = 42;
var y: int;  // Uninitialized (undefined value)
var s: string = "hello";
var arr: int[] = [1, 2, 3];
```

**Scope**: Variables are block-scoped. See §5.10 for scope rules.

### 5.2 `const` Declaration

**Syntax**: `const name: type = initializer;`

**Semantics**:
- Declares an **immutable** variable (constant)
- Type annotation is **mandatory**
- Initializer is **required** (constants must be initialized at declaration)
- Reassignment is forbidden (compile-time error)

**Examples**:
```omscript
const pi: float = 3.14159;
const max_size: int = 1024;
const greeting: string = "Hello, world!";
```

**Compile-time constants**: `const` variables may be evaluated at compile time if the initializer is a constant expression (see §5.7 for `comptime`).

### 5.3 Multi-Variable Declarations

**Syntax**: `var a: type = init1, b = init2, c = init3;`

**Semantics**:
- Declares multiple variables in a single statement
- The type annotation on the **first** variable propagates to subsequent variables **without explicit annotation**
- Subsequent variables may override the type with their own annotation

**Examples**:
```omscript
var a: int = 1, b = 2, c = 3;  // a, b, c all have type int
var x: int = 10, y: float = 3.14;  // x is int, y is float (explicit override)
const p: int = 5, q = 10;  // p and q both const int
```

**Type propagation rule**: If a variable in the multi-declaration lacks a type annotation, it inherits the type from the most recent explicitly-typed variable in the same declaration.

### 5.4 `register var` — Register-Allocation Hint

**Syntax**: `register var name: type = initializer;`

**Semantics**:
- Hints to the compiler that `name` should be allocated in a CPU register (via LLVM's `mem2reg` promotion)
- The variable is not allocated on the stack; all uses are in SSA (single static assignment) form
- Useful for hot loop variables where stack allocation would hinder optimization

**Example**:
```omscript
fn tight_loop(n: int) -> int {
    register var sum: int = 0;  // Force sum into register
    for (i: int in 0...n) {
        sum = sum + i;
    }
    return sum;
}
```

**Limitations**:
- Cannot take the address of a `register var` (no `&sum`)
- Applies only to local variables in function scope
- Ignored if the variable is not promotable (e.g., address-taken or escaped)

### 5.5 `global var` / `global const`

**Syntax**:
- `global var name: type = initializer;`
- `global const name: type = initializer;`

**Semantics**:
- Declares a **global** variable or constant (program-wide scope)
- Accessible from all functions in the module
- Global variables are allocated in static memory (not stack or heap)
- Initializer must be a constant expression (evaluated at compile time or load time)

**Example**:
```omscript
global const VERSION: string = "1.0.0";
global var counter: int = 0;

fn increment() {
    counter = counter + 1;
}

fn main() {
    println(VERSION);
    increment();
    println(counter);  // 1
}
```

**Imported globals**: Globals from imported files are namespaced under the import alias (e.g., `math::PI` if imported as `import "math.om" as math`).

### 5.6 `frozen` / `freeze` — Read-Only After Initialization

**Syntax**:
- `freeze variable;` — Mark variable as read-only after this point
- **No `frozen` keyword for declaration** (use `freeze` statement to lock a variable)

**Semantics**:
- `freeze x;` marks `x` as immutable for subsequent code
- Attempts to reassign `x` after `freeze` cause a compile-time error
- Useful for "initialize-once, use-many" patterns

**Example**:
```omscript
fn setup() {
    var config: int = load_config();
    freeze config;  // config is now read-only
    // config = 42;  // Error: cannot reassign frozen variable
    return config;
}
```

**Scope**: `freeze` applies from the point of the statement to the end of the variable's scope.

### 5.7 `comptime { ... }` Blocks — Compile-Time Evaluation

**Syntax**: `comptime { statements return expr; }`

**Semantics**:
- Executes code at **compile time** and replaces the block with the resulting value
- Useful for computing constants, configuration, or metaprogramming
- The block must end with `return expr;` where `expr` is the compile-time value
- Only constant expressions and control flow (no I/O, no heap allocation, no function calls to runtime functions) are allowed

**Example**:
```omscript
var factorial_5: int = comptime {
    var result: int = 1;
    for (i: int in 1...6) {
        result = result * i;
    }
    return result;
};
// factorial_5 = 120 at compile time
```

**Restrictions**:
- No calls to runtime-only functions (e.g., `println`, `file_read`)
- No mutable global state (each `comptime` block is isolated)
- No recursion (evaluation is iterative within the block)
- If evaluation fails (e.g., infinite loop, runtime-only operation), compilation error occurs

**Use cases**:
- Precompute lookup tables
- Generate repetitive code patterns
- Compile-time configuration checks

### 5.8 Assignment and Compound Assignment

**Simple assignment**: `variable = expression;`

```omscript
var x: int = 10;
x = 20;
```

**Compound assignment operators**:
| Operator | Meaning | Example | Equivalent |
|----------|---------|---------|------------|
| `+=` | Add-assign | `x += 5` | `x = x + 5` |
| `-=` | Subtract-assign | `x -= 3` | `x = x - 3` |
| `*=` | Multiply-assign | `x *= 2` | `x = x * 2` |
| `/=` | Divide-assign | `x /= 4` | `x = x / 4` |
| `%=` | Modulus-assign | `x %= 7` | `x = x % 7` |
| `&=` | Bitwise AND-assign | `x &= 0xFF` | `x = x & 0xFF` |
| `|=` | Bitwise OR-assign | `x |= 0x01` | `x = x | 0x01` |
| `^=` | Bitwise XOR-assign | `x ^= 0xAA` | `x = x ^ 0xAA` |
| `<<=` | Left-shift-assign | `x <<= 2` | `x = x << 2` |
| `>>=` | Right-shift-assign | `x >>= 1` | `x = x >> 1` |
| `**=` | Exponent-assign | `x **= 3` | `x = x ** 3` |
| `??=` | Null-coalesce-assign | `x ??= 0` | `x = x ?? 0` |
| `&&=` | Logical AND-assign | `x &&= y` | `x = x && y` |
| `||=` | Logical OR-assign | `x ||= y` | `x = x || y` |

**Semantics**:
- All compound assignments evaluate the left-hand side **once**
- Short-circuit semantics apply to `&&=` and `||=` (right-hand side evaluated only if necessary)

**Array/index assignment**: `arr[i] = value;`

**Field assignment**: `struct.field = value;`

### 5.9 Destructuring Assignment

**Syntax**: `var [a, b, c] = array_expr;` or `const [a, b, c] = array_expr;`

**Semantics**:
- Declares multiple variables by unpacking an array
- Desugars to individual indexed assignments: `var a = array_expr[0]; var b = array_expr[1]; ...`
- Underscore `_` as a placeholder skips an element: `var [a, _, c] = arr;` (ignores `arr[1]`)

**Example**:
```omscript
var data: int[] = [10, 20, 30];
var [x, y, z] = data;
// x = 10, y = 20, z = 30

const [first, _, third] = [100, 200, 300];
// first = 100, third = 300 (200 ignored)
```

**Type propagation**: All destructured variables inherit the element type of the array.

**Limitations**:
- Array length must match the number of variables (no partial destructuring or rest syntax)
- Only single-level destructuring supported (no nested destructuring of nested arrays)

### 5.10 Scope Rules

**Block scope**: Variables declared in a block `{ ... }` are visible only within that block and nested blocks.

```omscript
{
    var x: int = 10;
    {
        var y: int = 20;
        println(x);  // OK: x is visible
        println(y);  // OK: y is visible
    }
    println(x);  // OK
    // println(y);  // Error: y is out of scope
}
```

**Function scope**: Parameters and local variables are scoped to the function body.

**Global scope**: Global variables (`global var`/`global const`) are visible to all functions in the module.

**Shadowing**: Inner scopes may declare variables with the same name as outer scopes (shadowing). The inner declaration hides the outer one:

```omscript
var x: int = 10;
{
    var x: int = 20;  // Shadows outer x
    println(x);  // 20
}
println(x);  // 10
```

**Loop iteration variables**: For-loop iteration variables (`for (i in ...)`) are scoped to the loop body.

---

## 6. Functions

### 6.1 `fn` Declaration Syntax

**Syntax**:
```omscript
fn name(param1: type1, param2: type2, ...) -> return_type {
    // body
}
```

**Components**:
- `fn` keyword
- Function name (identifier)
- Parameter list (comma-separated, each with type annotation)
- Return type (optional; defaults to `void` if omitted)
- Body (block statement)

**Example**:
```omscript
fn add(a: int, b: int) -> int {
    return a + b;
}

fn greet(name: string) {
    println("Hello, ", name);
}
```

### 6.2 Parameters and Return Types

**Parameter type annotations**: Each parameter must have an explicit type annotation.

```omscript
fn multiply(x: float, y: float) -> float {
    return x * y;
}
```

**Return type annotation**: Arrow `->` precedes the return type. If omitted, the return type defaults to `void`.

```omscript
fn print_hello() {
    println("Hello");
}
// Equivalent to: fn print_hello() -> void { ... }
```

**Multiple return statements**: A function may have multiple `return` statements (in different branches). All must return values of the same type (or no value for `void` functions).

```omscript
fn absolute(x: int) -> int {
    if (x < 0) {
        return -x;
    }
    return x;
}
```

**Implicit return**: If a function body ends without a `return` statement and the return type is `void`, the function returns implicitly. For non-`void` functions, an explicit `return` is required (or a compile-time error occurs if control reaches the end).

### 6.3 Default Parameter Values

**Syntax**: `param: type = default_value`

**Semantics**:
- Parameters may have default values
- When calling the function, arguments for parameters with defaults may be omitted
- Default values are evaluated at call time (not compile time)

**Example**:
```omscript
fn greet(name: string = "World") {
    println("Hello, ", name);
}

fn main() {
    greet();           // "Hello, World"
    greet("Alice");    // "Hello, Alice"
}
```

**Limitations**:
- All parameters with defaults must appear after non-default parameters
- No named argument syntax (call with positional arguments only)

### 6.4 Expression-Body Functions

**Syntax**: `fn name(params) -> type = expression;`

**Semantics**:
- Shorthand for a function that returns a single expression
- Equivalent to `fn name(params) -> type { return expression; }`

**Example**:
```omscript
fn square(x: int) -> int = x * x;

fn main() {
    var result: int = square(5);  // 25
}
```

### 6.5 Generic / Type-Parameterized Functions

**Status**: Generic functions (parameterized by type) are **not currently implemented** in the parser or type system. This is a future feature.

**Anticipated syntax**: `fn name<T>(param: T) -> T { ... }`

### 6.6 Function Annotations

Functions may be preceded by **annotation attributes** (introduced by `@`) that modify compilation and optimization behavior.

**Annotation syntax**: `@annotation` appears on the line before `fn`:
```omscript
@inline
fn fast_add(a: int, b: int) -> int {
    return a + b;
}
```

**Multiple annotations**: Stack annotations on separate lines:
```omscript
@vectorize
@inline
fn compute(x: int) -> int {
    return x * 2;
}
```

**Recognized function annotations**:

#### `@inline` — Force Inlining
Requests that the function be inlined at all call sites (LLVM `alwaysinline` attribute).

```omscript
@inline
fn add(a: int, b: int) -> int {
    return a + b;
}
```

**Effect**: Function body is substituted directly at call sites, eliminating call overhead. Use for small, hot functions.

#### `@noinline` — Prevent Inlining
Requests that the function never be inlined (LLVM `noinline` attribute).

```omscript
@noinline
fn large_computation(x: int) -> int {
    // Complex logic that should remain a separate function
    return x * x * x;
}
```

**Effect**: Function always emits a call instruction. Use for debugging or code size control.

#### `@hot` — Hot Function Hint
Marks the function as frequently executed (LLVM `hot` attribute).

```omscript
@hot
fn inner_loop(n: int) -> int {
    var sum: int = 0;
    for (i: int in 0...n) {
        sum = sum + i;
    }
    return sum;
}
```

**Effect**: Compiler prioritizes optimizations for this function (e.g., aggressive inlining, vectorization).

#### `@cold` — Cold Function Hint
Marks the function as rarely executed (LLVM `cold` attribute).

```omscript
@cold
fn error_handler(code: int) {
    println("Error: ", code);
}
```

**Effect**: Compiler deprioritizes this function (may move it out-of-line, reduce optimization effort).

#### `@pure` — Pure Function (No Side Effects)
Indicates the function has no side effects and returns the same result for the same inputs (LLVM `readonly` or `readnone` attribute).

```omscript
@pure
fn square(x: int) -> int {
    return x * x;
}
```

**Effect**: Enables memoization, dead-code elimination, and reordering. Use only if the function truly has no side effects (no I/O, no global state mutation).

#### `@static` — Static Function (Internal Linkage)
Marks the function as having internal linkage (not exported from the module).

```omscript
@static
fn helper(x: int) -> int {
    return x + 1;
}
```

**Effect**: Function is not visible outside the compilation unit. May enable additional interprocedural optimizations.

#### `@vectorize` — Request Vectorization
Hints that the function should be optimized for SIMD vectorization (applies to loops within the function).

```omscript
@vectorize
fn sum_array(arr: int[], n: int) -> int {
    var total: int = 0;
    for (i: int in 0...n) {
        total = total + arr[i];
    }
    return total;
}
```

**Effect**: Compiler attempts to generate SIMD instructions for loops. May combine with loop annotations (see §8.16).

#### `@flatten` — Flatten Control Flow
Requests aggressive loop unrolling and branch flattening to reduce control flow overhead.

```omscript
@flatten
fn compute_polynomial(x: int) -> int {
    var result: int = 0;
    for (i: int in 0...5) {
        result = result + i * x;
    }
    return result;
}
```

**Effect**: Loops may be fully unrolled, branches may be converted to selects. Increases code size but reduces branch mispredictions.

#### `@allocator` — Allocator Function Hint
Marks the function as a memory allocator (LLVM `noalias` return and specific allocator attributes).

```omscript
@allocator
fn my_malloc(size: int) -> ptr {
    // Allocation logic
    return null;  // Placeholder
}
```

**Effect**: Return pointer is guaranteed not to alias any existing pointer. Enables optimizations around memory operations.

#### `@optmax` — Maximum Optimization (OPTMAX)
Marks a function for aggressive optimization (equivalent to wrapping in `OPTMAX=:` / `OPTMAX!:` markers).

```omscript
@optmax
fn optimize_me(n: int) -> int {
    var sum: int = 0;
    for (i: int in 0...n) {
        sum = sum + i;
    }
    return sum;
}
```

**Effect**: Applies OPTMAX optimization profile (see §6 for OPTMAX details).

#### `@prefetch` — Memory Prefetch Hint
Requests that pointer parameters be prefetched at function entry.

```omscript
@prefetch
fn process_array(arr: ptr<int>, n: int) {
    // Array processing
}
```

**Effect**: Emits `llvm.prefetch` intrinsics at function entry for pointer parameters. Reduces memory latency in pointer-heavy code.

#### `@musttail` — Mandatory Tail Call
Guarantees that all `return call(...)` statements in the function are compiled as tail calls (no stack frame growth).

```omscript
@musttail
fn factorial_tail(n: int, acc: int) -> int {
    if (n == 0) {
        return acc;
    }
    return factorial_tail(n - 1, acc * n);
}
```

**Effect**: Recursive calls do not consume stack space (equivalent to iteration). Compilation fails if tail-call optimization is not possible.

#### Additional Annotations

**`@novectorize`**: Opposite of `@vectorize` — disables auto-vectorization for this function.

**`@unroll`**: Requests loop unrolling (may combine with `@vectorize`). Also available as a per-loop annotation (see §8.16).

### 6.7 Method-Call Syntax — Universal Function Call Syntax (UFCS)

**Syntax**: `receiver.function(args)` desugars to `function(receiver, args)`.

**Semantics**:
- Any function whose first parameter matches the type of `receiver` can be called with method syntax
- This is **syntactic sugar** (UFCS: Unified Function Call Syntax)
- No distinction between "methods" and "functions"; all functions are free-standing

**Example**:
```omscript
fn double(x: int) -> int {
    return x * 2;
}

fn main() {
    var a: int = 10;
    var b: int = a.double();  // Desugars to: double(a)
    println(b);  // 20
}
```

**Type-namespace dispatch**: Built-in types may have associated functions (e.g., `i32.parse(s)`). This is implemented internally via namespace resolution (see §9.14).

### 6.8 Tail Calls

**Explicit tail calls**: Use `return` immediately followed by a function call to request tail-call optimization:

```omscript
fn factorial(n: int, acc: int) -> int {
    if (n == 0) {
        return acc;
    }
    return factorial(n - 1, acc * n);  // Tail call
}
```

**Mandatory tail calls**: Annotate the function with `@musttail` to **require** tail-call optimization. Compilation fails if the call cannot be optimized to a tail call.

```omscript
@musttail
fn loop_with_tail_call(n: int) -> int {
    if (n == 0) {
        return 0;
    }
    return loop_with_tail_call(n - 1);  // Must be a tail call
}
```

**Limitations**:
- Tail calls require the calling convention to support it (default conventions do)
- Complex stack frames (e.g., exception handlers, destructors) may prevent tail-call optimization

### 6.9 First-Class Functions / Function Pointers

**Status**: First-class functions (storing function pointers in variables, passing functions as arguments) are **not fully formalized** in the current type system. This is a future feature.

**Current capability**: Functions can be passed by name to higher-order functions (e.g., `array_map(arr, lambda)` where `lambda` is a lambda expression or function reference).

### 6.10 Lambdas — Anonymous Functions

**Syntax**: `|param1, param2, ...| expression` or `|param1, param2, ...| { statements }`

**Semantics**:
- Defines an anonymous function (lambda)
- Parameters are comma-separated (type annotations optional; inferred from context)
- Body is either a single expression (implicit return) or a block with explicit `return`
- Lambdas are converted to named functions internally (compiler-generated unique names)

**Example (expression-body lambda)**:
```omscript
var arr: int[] = [1, 2, 3, 4, 5];
var doubled: int[] = array_map(arr, |x| x * 2);
// doubled = [2, 4, 6, 8, 10]
```

**Example (block-body lambda)**:
```omscript
var total: int = array_reduce(arr, |acc, x| {
    return acc + x;
}, 0);
// total = 15
```

**Captures**: Lambdas **do not capture** variables from the enclosing scope. All data must be passed explicitly as parameters. (True closures are not implemented.)

**Use cases**:
- Higher-order functions: `array_map`, `array_filter`, `array_reduce`
- Inline predicates and transformations
- Pipe chains (see §9.11)

**Internal representation**: Each lambda is desugared to a top-level named function with a compiler-generated name (e.g., `__lambda_0`, `__lambda_1`).

---

## 7. Control Flow

### 7.1 `if` / `elif` / `else`

**Syntax**:
```omscript
if (condition) {
    // then-branch
} elif (condition2) {
    // elif-branch
} else {
    // else-branch
}
```

**Semantics**:
- Evaluates `condition`; if true, executes then-branch; otherwise checks `elif` conditions in order; if all false, executes `else`-branch
- Conditions are evaluated lazily (short-circuit)
- Braces `{ }` are **required** for all branches (no braceless single-statement forms)

**Example**:
```omscript
var x: int = 10;
if (x > 0) {
    println("Positive");
} elif (x < 0) {
    println("Negative");
} else {
    println("Zero");
}
```

### 7.2 `unless`

**Syntax**: `unless (condition) { ... } else { ... }`

**Semantics**:
- Syntactic sugar for `if (!condition) { ... } else { ... }`
- Executes body when condition is **false**

**Example**:
```omscript
unless (x > 10) {
    println("x is not greater than 10");
}
```

**Equivalent to**:
```omscript
if (!(x > 10)) {
    println("x is not greater than 10");
}
```

### 7.3 `guard`

**Syntax**: `guard (condition) else { early_exit }`

**Semantics**:
- Early-exit pattern: if `condition` is **false**, executes `early_exit` block (which must exit the function, e.g., via `return`, `throw`, `break`)
- If `condition` is **true**, execution continues normally after the `guard` statement

**Example**:
```omscript
fn validate(x: int) -> int {
    guard (x > 0) else {
        return -1;
    }
    return x * 2;
}
```

**Use case**: Precondition checks, argument validation (reduces nesting).

### 7.4 `switch`

**Syntax**:
```omscript
switch (expression) {
    case value1, value2, ...:
        // body
        break;
    case value3:
        // body
        break;
    default:
        // body
}
```

**Semantics**:
- Evaluates `expression` once and compares it to each `case` value
- Executes the first matching case body
- **No implicit fallthrough**: `break` is **required** at the end of each case (or explicit fallthrough via omitting `break`)
- `default` is optional and matches any value not covered by cases

**Example**:
```omscript
fn classify(x: int) -> string {
    switch (x) {
        case 1:
            return "one";
        case 2:
            return "two";
        default:
            return "other";
    }
}
```

**Multiple values per case**: Comma-separated list:
```omscript
switch (x) {
    case 1, 2, 3:
        println("Small");
        break;
    case 10, 20, 30:
        println("Large");
        break;
}
```

**Fallthrough**: Omit `break` to continue to the next case (explicit fallthrough):
```omscript
switch (x) {
    case 1:
        println("One");
        // Falls through to case 2
    case 2:
        println("One or Two");
        break;
}
```

### 7.5 `when` — Pattern-Matching Variant

**Syntax**:
```omscript
when (expression) {
    value1, value2, ... => { body }
    value3 => { body }
    _ => { body }
}
```

**Semantics**:
- Similar to `switch` but with pattern-matching syntax
- `=>` separates pattern from body
- `_` is a wildcard pattern (matches any value, equivalent to `default`)
- **No fallthrough**: each arm is independent
- **No `break` required**: arms are non-fallthrough by design

**Example**:
```omscript
fn day_name(day: int) -> string {
    var result: string = "";
    when (day) {
        1, 2, 3, 4, 5 => { result = "Weekday"; }
        6, 7 => { result = "Weekend"; }
        _ => { result = "Invalid"; }
    }
    return result;
}
```

**Difference from `switch`**:
- `when` does not require `break` statements (arms are mutually exclusive)
- `when` uses `=>` instead of `:` for case labels
- Wildcard `_` instead of `default`

### 7.6 `defer`

**Syntax**: `defer statement;`

**Semantics**:
- Defers execution of `statement` until the end of the current scope (block exit)
- Multiple `defer` statements execute in **LIFO** order (last-in, first-out)
- Useful for cleanup, resource release, logging

**Example**:
```omscript
fn example() {
    var x: int = 10;
    defer println("End of function");
    defer println("This runs before the above");
    println("Function body");
}
// Output:
// Function body
// This runs before the above
// End of function
```

**Scope**: `defer` applies to the innermost enclosing block:
```omscript
{
    defer println("Outer defer");
    {
        defer println("Inner defer");
    }
    // Inner defer executes here (end of inner block)
}
// Outer defer executes here (end of outer block)
```

### 7.7 `with`

**Syntax**: `with (var x = expr) { body }` or `with (var x = e1, var y = e2) { body }`

**Semantics**:
- Scoped variable binding: declares variables visible only within the `with` block
- Equivalent to wrapping `body` in a nested block with variable declarations

**Example**:
```omscript
with (var temp: int = compute()) {
    println(temp);
}
// temp is not accessible here
```

**Multiple variables**:
```omscript
with (var a: int = 5, var b: int = 10) {
    println(a + b);
}
```

**Use case**: Limiting variable scope, avoiding pollution of outer scope.

### 7.8 Branch Prediction Hints

**Syntax**:
- `likely if (condition) { ... }` — Hint that condition is **likely true**
- `unlikely if (condition) { ... }` — Hint that condition is **likely false**

**Semantics**:
- Provides branch prediction hints to the compiler (LLVM metadata)
- Optimizes code layout (places likely branch inline, unlikely branch out-of-line)

**Example**:
```omscript
likely if (x > 0) {
    // Common case
} else {
    // Rare case
}

unlikely if (error_occurred) {
    handle_error();
}
```

**Effect**: Emits LLVM branch weight metadata (`!prof`). No semantic difference, only optimization hints.

### 7.9 Ternary Operator

**Syntax**: `condition ? true_expr : false_expr`

**Semantics**:
- Evaluates `condition`; if true, evaluates and returns `true_expr`; otherwise evaluates and returns `false_expr`
- Short-circuit: only one of the two branches is evaluated

**Example**:
```omscript
var max: int = (a > b) ? a : b;
var sign: string = (x >= 0) ? "positive" : "negative";
```

**Type constraint**: Both branches must have the same type (or compatible types).

### 7.10 `assume` / `unreachable` / `expect` Statements

These three constructs feed information to the optimizer. They emit **no runtime code** in the success path — they are purely advisory.

#### `assume(cond)` and `assume(cond) else deopt { stmt }`

`assume` is **both a statement and a built-in expression** with two forms:

**Bare form — statement or expression:**
```omscript
assume(b != 0);          // statement
let q = a / assume(b != 0); // expression position also accepted
```
Tells the optimizer to treat `cond` as true — implemented by lowering to `llvm.assume(cond)`. **No runtime check is emitted.** If `cond` is actually false at runtime, the program has undefined behaviour (the optimizer may have deleted code, mis-folded values, etc.). Use only for invariants you can prove.

**`else deopt` form — statement only:**
```omscript
assume(x > 0) else deopt {
    // deopt path: runs when the assumed condition does not hold
    println("slow path");
    return -1;
}
```
This form **does** emit a runtime check. If the condition is true the body is skipped (just like the bare form); if it's false the deopt block runs. Conceptually a guarded fast-path / slow-path split — the optimizer is told to bias and lay out the deopt block as cold. The `deopt` keyword after `else` is optional in the grammar; both `assume(c) else deopt { ... }` and `assume(c) else { ... }` parse to the same AST (`AssumeStmt` with a non-null deopt body).

#### `unreachable()`

```omscript
unreachable();
```
Marks a code path as never executed. Lowers to LLVM's `unreachable` instruction — reaching it at runtime is **undefined behaviour** (the compiler is free to delete preceding code that would lead here). Use at the bottom of `switch` defaults that should be impossible, or after `exit`/`abort` calls the optimizer doesn't recognize.

#### `expect(value, expected)`

```omscript
if (expect(x == 0, false)) { /* cold */ }
```
A pure branch-prediction hint. Returns `value` unchanged but tags the comparison with metadata steering LLVM's branch-weighting. No runtime cost, no UB on misprediction — this is purely a layout hint.

#### Comparison

| Construct | Runtime check? | UB if violated? | Effect |
|-----------|---------------|----------------|--------|
| `assert(cond)` (§16.3) | Yes | No (abort) | Real check, prints + aborts on failure |
| `assume(cond)` | **No** | **Yes** | Optimizer hint via `llvm.assume` |
| `assume(cond) else deopt { ... }` | Yes | No (runs deopt body) | Guarded fast-path / cold deopt path |
| `unreachable()` | No | **Yes** | Optimizer assumes path is dead |
| `expect(v, e)` | No | No | Branch-prediction hint only |

**Example:**
```omscript
fn divide(a: int, b: int) -> int {
    assume(b != 0);  // optimizer assumes b is never 0; division check elided
    return a / b;
}

fn handle_case(x: int) {
    if (x == 1)      { return; }
    elif (x == 2)    { return; }
    else             { unreachable(); }  // x is always 1 or 2 by construction
}
```

**Warning:** Misuse of `assume` or `unreachable` produces silent miscompilation, not a crash. Reserve them for invariants you can prove. Prefer `assert` when in doubt.

---

## 8. Loops

### 8.1 `while`

**Syntax**: `while (condition) { body }`

**Semantics**:
- Pre-test loop: evaluates `condition`; if true, executes `body`; repeats
- If `condition` is false initially, `body` never executes

**Example**:
```omscript
var i: int = 0;
while (i < 10) {
    println(i);
    i = i + 1;
}
```

### 8.2 `do { } while (...)` and `do { } until (...)`

**Syntax**:
- `do { body } while (condition);`
- `do { body } until (condition);`

**Semantics**:
- Post-test loop: executes `body` first, then evaluates `condition`
- `while` form: repeat if `condition` is true
- `until` form: repeat if `condition` is false (sugar for `while (!condition)`)

**Example (`do while`)**:
```omscript
var i: int = 0;
do {
    println(i);
    i = i + 1;
} while (i < 10);
```

**Example (`do until`)**:
```omscript
var j: int = 0;
do {
    println(j);
    j = j + 1;
} until (j == 10);  // Repeat until j equals 10
```

### 8.3 `until`

**Syntax**: `until (condition) { body }`

**Semantics**:
- Syntactic sugar for `while (!condition) { body }`
- Pre-test loop that repeats **while condition is false**

**Example**:
```omscript
var i: int = 0;
until (i == 10) {
    println(i);
    i = i + 1;
}
```

### 8.4 `for (i in start...end)` — Range Loops

**Syntax**:
- Inclusive range: `for (var in start...end) { body }`
- Exclusive range: `for (var in start..end) { body }`

**Semantics**:
- Iterates `var` from `start` to `end` (inclusive or exclusive)
- Step is implicitly 1
- `var` is scoped to the loop body

**Example (inclusive)**:
```omscript
for (i: int in 0...5) {
    println(i);  // Prints: 0, 1, 2, 3, 4, 5
}
```

**Example (exclusive)**:
```omscript
for (i: int in 0..5) {
    println(i);  // Prints: 0, 1, 2, 3, 4
}
```

### 8.5 `for (i in start downto end)` / Step Variant

**Syntax**:
- `for (var in start downto end) { body }` — Descending loop
- **No explicit step syntax** in basic `for`; use `for` with custom increment in body

**`downto` semantics**: Decrements iterator from `start` to `end` (inclusive).

**Example**:
```omscript
for (i: int in 10 downto 0) {
    println(i);  // Prints: 10, 9, 8, ..., 1, 0
}
```

**Custom step**: Use `range_step` function (see §10):
```omscript
var steps: int[] = range_step(0, 10, 2);  // [0, 2, 4, 6, 8]
for (x in steps) {
    println(x);
}
```

### 8.6 `for (x in collection)` — For-Each

**Syntax**: `for (var in collection) { body }`

**Semantics**:
- Iterates over elements of `collection` (array, string, etc.)
- `var` takes the value of each element in turn

**Example (array)**:
```omscript
var arr: int[] = [10, 20, 30];
for (x in arr) {
    println(x);  // Prints: 10, 20, 30
}
```

**Example (string)**:
```omscript
var s: string = "abc";
for (c in s) {
    println(c);  // Prints byte values: 97, 98, 99 (ASCII codes)
}
```

### 8.7 `foreach`

**Syntax**: `foreach (var in collection) { body }`

**Semantics**:
- Alias for `for (var in collection)` (identical behavior)

**Example**:
```omscript
var nums: int[] = [1, 2, 3];
foreach (n in nums) {
    println(n);
}
```

### 8.8 `loop { }`

**Syntax**:
- Infinite loop: `loop { body }`
- Counted loop: `loop N { body }` or `loop (N) { body }`

**Infinite loop semantics**: Equivalent to `while (true) { body }`

**Example**:
```omscript
var i: int = 0;
loop {
    println(i);
    i = i + 1;
    if (i == 10) {
        break;
    }
}
```

**Counted loop semantics**: Executes `body` exactly `N` times (desugars to `for (__loop_i in 0...N) { body }`).

**Example**:
```omscript
var sum: int = 0;
loop 10 {
    sum = sum + 1;
}
// sum = 10
```

### 8.9 `repeat { } until (...)`

**Syntax**: `repeat { body } until (condition);`

**Semantics**:
- Post-test loop: executes `body`, then evaluates `condition`
- Repeats **while condition is false** (until condition becomes true)
- Equivalent to `do { body } until (condition);`

**Example**:
```omscript
var i: int = 0;
repeat {
    println(i);
    i = i + 1;
} until (i == 10);
```

### 8.10 `forever { }`

**Syntax**: `forever { body }`

**Semantics**:
- Infinite loop (alias for `loop { body }`)

**Example**:
```omscript
forever {
    println("Looping...");
    // Must use break to exit
}
```

### 8.11 `times N { }`

**Syntax**: `times N { body }` or `times (N) { body }`

**Semantics**:
- Executes `body` exactly `N` times
- `N` is evaluated once at loop entry
- Desugars to: `for (__times_i in 0...N) { body }`

**Example**:
```omscript
var counter: int = 0;
times 5 {
    counter = counter + 1;
}
// counter = 5
```

**With variable**:
```omscript
var n: int = 3;
times n {
    println("Repeat");
}
```

### 8.12 `parallel for (...)`

**Syntax**: `parallel for (var in range) { body }`

**Semantics**:
- Parallelization hint: suggests that loop iterations may execute in parallel
- No ordering guarantees between iterations
- Data races (shared mutable state) are **undefined behavior**

**Example**:
```omscript
var sum: int = 0;
parallel for (i: int in 0...10) {
    // Caution: accessing `sum` from multiple threads is a data race
    // This example is unsafe unless atomic operations are used
    sum = sum + i;  // UNSAFE (for illustration only)
}
```

**Safe use**: Each iteration must be independent (no shared mutable state, or use atomic operations).

**Implementation**: Code generator may emit OpenMP pragmas or thread pool calls. Exact parallelization strategy is implementation-defined.

### 8.13 `pipeline { stage s { } stage t { } }`

**Syntax**:
- Counted form: `pipeline N { stage name { body } ... }`
- One-shot form: `pipeline { stage name { body } ... }`

**Semantics**:
- Staged execution pipeline: divides computation into named stages
- **Counted form**: Each stage executes `N` times with a loop iterator `__pipeline_i` (0 to N-1)
- **One-shot form**: Each stage executes exactly once (no loop)

**Example (counted)**:
```omscript
var sum: int = 0;
pipeline 5 {
    stage accumulate {
        sum = sum + __pipeline_i;
    }
}
// sum = 0 + 1 + 2 + 3 + 4 = 10
```

**Example (multi-stage)**:
```omscript
var x: int = 0;
var y: int = 0;
var result: int = 0;
pipeline 4 {
    stage load {
        x = __pipeline_i;
    }
    stage compute {
        y = x * 2;
    }
    stage store {
        result = result + y;
    }
}
// result = (0*2) + (1*2) + (2*2) + (3*2) = 0 + 2 + 4 + 6 = 12
```

**Example (one-shot)**:
```omscript
var a: int = 1;
var b: int = 0;
pipeline {
    stage double {
        a = a * 2;
    }
    stage add5 {
        b = a + 5;
    }
}
// a = 2, b = 7
```

**Implementation**: May emit software prefetching, pipelined execution, or other stage-based optimizations. Exact semantics are implementation-defined (sequential execution is a valid interpretation).

### 8.14 `swap a, b;`

**Syntax**: `swap a, b;`

**Semantics**:
- Swaps the values of variables `a` and `b`
- Desugars to: `{ var tmp = a; a = b; b = tmp; }`

**Example**:
```omscript
var x: int = 10;
var y: int = 20;
swap x, y;
// x = 20, y = 10
```

### 8.15 `break` / `continue`

**`break` statement**: Exits the innermost enclosing loop or `switch`.

**Example**:
```omscript
for (i: int in 0...10) {
    if (i == 5) {
        break;  // Exit loop
    }
    println(i);  // Prints: 0, 1, 2, 3, 4
}
```

**`continue` statement**: Skips the remainder of the current loop iteration and proceeds to the next iteration.

**Example**:
```omscript
for (i: int in 0...10) {
    if (i % 2 == 0) {
        continue;  // Skip even numbers
    }
    println(i);  // Prints: 1, 3, 5, 7, 9
}
```

**`break` in `switch`**: Required at the end of each case to prevent fallthrough (unless explicit fallthrough is intended).

### 8.16 Loop Annotations

**Syntax**: `for (...) @loop(annotation) { body }`

**Semantics**:
- Loop-specific optimization hints
- Placed after the loop header, before the body

**Recognized loop annotations**:

#### `@loop(unroll=N)` — Loop Unrolling

Requests that the loop be unrolled by a factor of `N`.

**Example**:
```omscript
for (i: int in 0...16) @loop(unroll=4) {
    sum = sum + i;
}
```

**Effect**: Loop body is duplicated 4 times per iteration, reducing loop overhead (at the cost of code size).

#### `@loop(vectorize)` — Vectorization Hint

Requests that the loop be vectorized (SIMD).

**Example**:
```omscript
for (i: int in 0...100) @loop(vectorize) {
    arr[i] = arr[i] * 2;
}
```

**Effect**: Compiler attempts to generate SIMD instructions (e.g., AVX, NEON).

#### Additional Loop Annotations

**`@loop(unroll)` (no argument)**: Auto-unroll (compiler chooses factor).

**`@independent`**: Asserts that iterations have no cross-iteration dependencies (enables aggressive parallelization and reordering).

**`@fuse`**: Suggests merging this loop with an adjacent compatible loop (loop fusion).

**Example (multiple annotations)**:
```omscript
for (i: int in 0...100) @loop(vectorize) @loop(unroll=8) {
    arr[i] = compute(i);
}
```

---

## 9. Operators and Expressions

### 9.1 Arithmetic Operators

| Operator | Syntax | Meaning | Example |
|----------|--------|---------|---------|
| `+` | `a + b` | Addition | `3 + 4` → `7` |
| `-` | `a - b` | Subtraction | `10 - 3` → `7` |
| `*` | `a * b` | Multiplication | `4 * 5` → `20` |
| `/` | `a / b` | Division | `20 / 4` → `5` |
| `%` | `a % b` | Modulus (remainder) | `10 % 3` → `1` |
| `**` | `a ** b` | Exponentiation | `2 ** 8` → `256` |
| `-` | `-a` | Unary negation | `-5` → `-5` |
| `+` | `+a` | Unary plus (identity) | `+5` → `5` |

**Type rules**:
- Integer + integer → integer (widened to larger type if mixed sizes)
- Float + float → float
- Integer + float → float (integer promoted to float)
- Division by zero: undefined behavior (may trap, wrap, or produce garbage)

**Integer division**: Truncates toward zero (not floor division).

**Exponentiation `**`**: Implemented via `llvm.powi` (integer exponent) or `pow` (float exponent). Integer base with negative exponent causes undefined behavior.

### 9.2 Comparison Operators

| Operator | Syntax | Meaning | Example |
|----------|--------|---------|---------|
| `==` | `a == b` | Equality | `5 == 5` → `true` |
| `!=` | `a != b` | Inequality | `5 != 3` → `true` |
| `<` | `a < b` | Less than | `3 < 5` → `true` |
| `<=` | `a <= b` | Less or equal | `5 <= 5` → `true` |
| `>` | `a > b` | Greater than | `5 > 3` → `true` |
| `>=` | `a >= b` | Greater or equal | `5 >= 3` → `true` |

**Result type**: `bool`

**Chained comparison**: **Not currently supported** (e.g., `a < b < c` parses as `(a < b) < c`, not `a < b && b < c`).

**String comparison**: Strings use lexicographic (byte-wise) comparison. Use `str_eq(a, b)` for equality check or `==` operator.

### 9.3 Logical Operators

| Operator | Syntax | Meaning | Short-circuit |
|----------|--------|---------|---------------|
| `&&` | `a && b` | Logical AND | Yes (if `a` is false, `b` not evaluated) |
| `||` | `a || b` | Logical OR | Yes (if `a` is true, `b` not evaluated) |
| `!` | `!a` | Logical NOT | N/A (unary) |

**Result type**: `bool`

**Short-circuit semantics**: `&&` and `||` evaluate the right operand **only if necessary** to determine the result.

**Example**:
```omscript
if (ptr != null && ptr.value > 0) {
    // ptr.value is only accessed if ptr is non-null
}
```

### 9.4 Bitwise Operators

| Operator | Syntax | Meaning |
|----------|--------|---------|
| `&` | `a & b` | Bitwise AND |
| `|` | `a | b` | Bitwise OR |
| `^` | `a ^ b` | Bitwise XOR |
| `~` | `~a` | Bitwise NOT (one's complement) |
| `<<` | `a << b` | Left shift |
| `>>` | `a >> b` | Right shift |

**Type rules**: Operands must be integers. Result is integer of the same width.

**Shift semantics**:
- **Left shift `<<`**: Fills vacated bits with zeros
- **Right shift `>>`**:
  - **Arithmetic shift** (signed integers): Fills vacated bits with sign bit (preserves sign)
  - **Logical shift** (unsigned integers): Fills vacated bits with zeros

**Shift amount**: Must be non-negative and less than the bit width of the left operand (undefined behavior otherwise).

**Example**:
```omscript
var x: int = 0b1010;  // 10
var y: int = x << 2;  // 0b101000 = 40
var z: int = x >> 1;  // 0b0101 = 5
var mask: int = x & 0b1111;  // 0b1010 = 10
```

### 9.5 Null-Coalescing (`??`) and Elvis (`?:`)

**Null-coalescing `??`**: `a ?? b` returns `a` if non-null, otherwise `b`.

**Semantics**: Short-circuit evaluation — if `a` is non-null, `b` is not evaluated.

**Example**:
```omscript
var result: int = getValue() ?? 0;  // Use 0 if getValue() returns null
```

**Elvis operator `?:`**: **Not currently implemented** as a distinct operator. Use ternary `cond ? a : b` instead.

### 9.6 `in` Operator (Membership)

**Syntax**: `element in collection`

**Semantics**: Tests if `element` is a member of `collection` (array, dict keys, string substring, etc.).

**Example (array)**:
```omscript
var arr: int[] = [1, 2, 3];
var found: bool = 2 in arr;  // true
```

**Example (dict)**:
```omscript
var d: dict = { "key": 42 };
var has_key: bool = "key" in d;  // true
```

**Example (string)**:
```omscript
var s: string = "hello";
var has_char: bool = "e" in s;  // true (substring search)
```

**Result type**: `bool`

### 9.7 Range `..` and `...`

**Exclusive range `..`**: `start..end` creates a half-open range `[start, end)`.

**Inclusive range `...`**: `start...end` creates a closed range `[start, end]`.

**Usage**: Primarily in `for` loops (see §8.4):
```omscript
for (i: int in 0..5) {
    // i = 0, 1, 2, 3, 4
}
for (i: int in 0...5) {
    // i = 0, 1, 2, 3, 4, 5
}
```

**Not first-class values**: Ranges are not directly stored in variables; they are syntactic constructs for loops.

### 9.8 Spread `...arr`

**Syntax**: `...expr`

**Semantics**: Expands an array into individual elements (used in array literals or function calls).

**Example (array literal)**:
```omscript
var a: int[] = [1, 2, 3];
var b: int[] = [0, ...a, 4];  // [0, 1, 2, 3, 4]
```

**Example (function call)**: **Not currently implemented** (future feature).

### 9.9 Increment / Decrement (`++` / `--`)

**Prefix increment**: `++x` increments `x` and returns the new value.

**Postfix increment**: `x++` increments `x` and returns the old value.

**Prefix decrement**: `--x` decrements `x` and returns the new value.

**Postfix decrement**: `x--` decrements `x` and returns the old value.

**Example**:
```omscript
var i: int = 5;
var a: int = ++i;  // i = 6, a = 6
var b: int = i++;  // i = 7, b = 6
var c: int = --i;  // i = 6, c = 6
var d: int = i--;  // i = 5, d = 6
```

**Semantics**: Equivalent to `x = x + 1` or `x = x - 1`, but as a single operation (may optimize better).

### 9.10 Address-Of `&x`

**Syntax**: `&x`

**Semantics**: Returns a pointer to the variable `x`.

**Type**: If `x` has type `T`, `&x` has type `ptr<T>`.

**Example**:
```omscript
var n: int = 42;
var p: ptr<int> = &n;  // Pointer to n
```

**Element-type inference**: The pointer's element type `T` is inferred from the variable type.

**Bare `ptr` sentinel**: The generic `ptr` type (without element type) is used for type-erased pointers.

### 9.11 Pipe-Forward `|>`

**Syntax**: `expr |> function`

**Semantics**: Desugars to `function(expr)` (passes `expr` as the first argument to `function`).

**Example**:
```omscript
var arr: int[] = [1, 2, 3, 4, 5];
var length: int = arr |> len;  // Equivalent to: len(arr)
```

**Chaining**:
```omscript
var result: int = [1, 2, 3, 4, 5]
    |> array_filter(|x| x % 2 == 0)
    |> len;
// Result: 2 (length of [2, 4])
```

### 9.12 String Concatenation

**Syntax**: `str1 + str2`

**Semantics**: Concatenates two strings.

**Mixed types**: If one operand is a string and the other is not, the non-string is converted to a string via `to_string`.

**Example**:
```omscript
var s1: string = "Hello";
var s2: string = " World";
var s3: string = s1 + s2;  // "Hello World"

var msg: string = "Count: " + 42;  // "Count: 42" (integer auto-converted)
```

### 9.13 Complete Operator Precedence Table

Operators are listed from **highest to lowest** precedence. Operators on the same line have equal precedence and associate left-to-right unless noted.

| Precedence | Operators | Associativity | Description |
|------------|-----------|---------------|-------------|
| 1 (highest) | `::` | Left | Scope resolution |
| 2 | `()` `[]` `.` | Left | Call, index, member access |
| 3 | `++` `--` (postfix) | Left | Postfix increment/decrement |
| 4 | `++` `--` (prefix), `+` `-` (unary), `!` `~` `&` | Right | Prefix inc/dec, unary +/-, logical NOT, bitwise NOT, address-of |
| 5 | `**` | Right | Exponentiation |
| 6 | `*` `/` `%` | Left | Multiplication, division, modulus |
| 7 | `+` `-` | Left | Addition, subtraction |
| 8 | `<<` `>>` | Left | Bitwise shift |
| 9 | `<` `<=` `>` `>=` | Left | Relational comparison |
| 10 | `==` `!=` | Left | Equality comparison |
| 11 | `&` | Left | Bitwise AND |
| 12 | `^` | Left | Bitwise XOR |
| 13 | `|` | Left | Bitwise OR |
| 14 | `&&` | Left | Logical AND (short-circuit) |
| 15 | `||` | Left | Logical OR (short-circuit) |
| 16 | `??` | Left | Null-coalescing |
| 17 | `? :` | Right | Ternary conditional |
| 18 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=` `**=` `??=` `&&=` `||=` | Right | Assignment, compound assignment |
| 19 | `|>` | Left | Pipe-forward |
| 20 (lowest) | `,` | Left | Comma (sequencing) |

**Notes**:
- **Exponentiation `**`** is right-associative: `2 ** 3 ** 2` = `2 ** (3 ** 2)` = `2 ** 9` = `512`
- **Ternary `? :`** is right-associative: `a ? b : c ? d : e` = `a ? b : (c ? d : e)`
- **Assignment** is right-associative: `a = b = c` = `a = (b = c)`

### 9.14 Type-Namespace Method Dispatch

**Syntax**: `Type.method(args)`

**Semantics**: Calls a static function associated with type `Type` (e.g., `i32.parse(s)` parses a string as an `i32`).

**Example**:
```omscript
var num: int = i32.parse("42");  // Parse string to i32
```

**Implementation**: Resolves via namespace lookup (built-in types have associated namespaces). This is syntactic sugar for calling a global function with a qualified name.

### 9.15 Cast-Function Dispatch

**Syntax**: `TargetType(value)`

**Semantics**: Converts `value` to `TargetType`. Implemented as built-in conversion functions.

**Example**:
```omscript
var i: int = 42;
var f: float = float(i);  // Cast int to float

var s: string = "123";
var n: int = int(s);  // Parse string to int (may fail at runtime)
```

**Built-in casts**:
- `int(x)`, `float(x)`, `string(x)`, `bool(x)`: Convert `x` to target type
- Type conversion may involve parsing (e.g., `int("42")`) or formatting (e.g., `string(42)`)

---

## 10. Collection Literals and Indexing

This section covers the **literal syntax** and **basic indexing/slicing** for built-in composite types. Built-in functions for manipulating these types (e.g., `len`, `push`, `pop`) are deferred to Part 2.

### 10.1 Array Literals

**Syntax**:
- `[elem1, elem2, ...]` — Array literal
- `[]type{elem1, elem2, ...}` — Typed array literal (explicit element type)

**Examples**:
```omscript
var arr1: int[] = [1, 2, 3, 4, 5];
var arr2: float[] = [1.0, 2.5, 3.14];
var arr3: string[] = ["hello", "world"];
var empty: int[] = [];
```

**Typed array literal** (explicit type):
```omscript
var arr4: int[] = []int{10, 20, 30};
```

**Array initialization functions** (runtime):
- `array_fill(N, value)` — Creates array of length `N` filled with `value`
- `range(start, end)` — Creates array `[start, start+1, ..., end-1]`
- `range_step(start, end, step)` — Creates array with custom step

**Example**:
```omscript
var zeros: int[] = array_fill(10, 0);  // [0, 0, ..., 0] (10 elements)
var seq: int[] = range(0, 5);          // [0, 1, 2, 3, 4]
var evens: int[] = range_step(0, 10, 2);  // [0, 2, 4, 6, 8]
```

### 10.2 Indexing and Slicing

**Indexing**: `arr[i]` accesses the element at index `i` (zero-based).

```omscript
var arr: int[] = [10, 20, 30];
var first: int = arr[0];  // 10
var second: int = arr[1]; // 20
```

**Out-of-bounds access**: Undefined behavior (may trap, return garbage, or wrap).

**Slicing** (half-open range): `arr[i..j]` extracts elements from index `i` to `j-1`.

```omscript
var slice: int[] = arr[1..3];  // [20, 30]
```

**Slicing** (closed range): `arr[i...j]` extracts elements from index `i` to `j` (inclusive).

```omscript
var slice2: int[] = arr[0...1];  // [10, 20]
```

**Negative indices**: **Not currently supported**. Use positive indices only.

**Assignment**: `arr[i] = value;` updates the element at index `i`.

```omscript
arr[0] = 100;  // arr = [100, 20, 30]
```

### 10.3 String Literals and Indexing

**String literals**: `"text"` (see §2.5.5 for escape sequences and multi-line strings).

**Indexing**: `s[i]` returns the **byte value** at index `i` (as an integer, not a character).

```omscript
var s: string = "hello";
var first_byte: int = s[0];  // 104 (ASCII 'h')
```

**Character extraction**: Use `char_at(s, i)` to get a single-character string:

```omscript
var ch: string = char_at(s, 1);  // "e"
```

**Slicing**: `s[i..j]` and `s[i...j]` extract substrings.

```omscript
var sub: string = s[1..4];  // "ell"
```

**Length**: `len(s)` returns the byte count (not character count for multibyte UTF-8).

**Concatenation**: `s1 + s2` (see §9.12).

### 10.4 Dict Literals

**Syntax**: `{ key1: value1, key2: value2, ... }`

**Empty dict**: `{}`

**Examples**:
```omscript
var d: dict = { "name": "Alice", "age": 30 };
var scores: dict = { 1: 100, 2: 200, 3: 300 };
var empty_map: dict = {};
```

**Type annotation**: `dict` (untyped); typed variants (`dict[K, V]`) not yet fully implemented.

**Access**: Use `map_get(d, key, default)` (see Part 2 for dict functions).

```omscript
var age: int = map_get(d, "age", 0);  // 30
```

**Mutation**: Dicts are **immutable** from the user perspective; operations like `map_set` return a new dict.

```omscript
d = map_set(d, "city", "NYC");
```

### 10.5 Struct Literals

**Syntax**: `StructName { field1: value1, field2: value2, ... }`

**Example**:
```omscript
struct Point { x, y }

var p: Point = Point { x: 10, y: 20 };
```

**Field access**: `p.field`

```omscript
var x_val: int = p.x;  // 10
```

**Field assignment**: `p.field = value;`

```omscript
p.y = 25;  // p.y is now 25
```

### 10.6 Enum Literals and Variant Access

**Enum declaration** (see §4.4.7):
```omscript
enum Color {
    RED,
    GREEN = 10,
    BLUE
}
```

**Variant access**:
- Underscore convention: `Color_RED`, `Color_GREEN`, `Color_BLUE`
- Scope resolution (alternative): `Color::RED` (desugars to `Color_RED`)

**Example**:
```omscript
var c: int = Color_RED;  // 0
var g: int = Color::GREEN;  // 10 (scope resolution syntax)
```

**Enum values are global integer constants**; no distinct enum type (values are plain integers).

---

**End of Part 1 of the OmScript Language Reference.**

*This document is derived exclusively from the OmScript compiler source code and validated test programs. All claims are verified against the implementation.*

## 11. Arrays — Complete API

### 11.1 Array model

**Heap layout:**
```
[length: i64][element₀: i64][element₁: i64]...[element_{n-1}: i64]
```

- **Header:** 8-byte signed integer storing the logical element count.
- **Element storage:** Contiguous i64 slots immediately following the header.
- **Element type:** All array elements are i64 by default (supports integers, floats-as-i64-bits, and string pointers).
- **Zero-initialization:** Array elements are NOT zero-initialized by default (caller must explicitly fill or assign).
- **Pointer representation:** Arrays pass as i64 (ptrtoint of the allocation) across function boundaries; callers convert via `IntToPtr` to access elements.

**Metadata tracking:**
- `arrayLenRangeMD_`: LLVM range metadata `!range [0, 2^63)` attached to length loads, informing optimizations that array lengths are always non-negative.
- `tbaaArrayLen_`, `tbaaArrayData_`: TBAA (Type-Based Alias Analysis) tags distinguish length field from element data, enabling aggressive load/store reordering.

**Address calculations:**
- Element at index `i` resides at byte offset `(i + 1) * 8` from the array base pointer.
- Negative indices are NOT supported by the runtime (indexing with `i < 0` is undefined behavior).

### 11.2 Construction

#### Literal syntax

```omscript
var a = [1, 2, 3, 4];            // 4-element array
var b = [10 * 2, 30, sum(x)];    // expressions allowed
```

**Compile-time evaluation:** When all elements are compile-time constant integers, the parser emits a `LiteralExpr` with `literalType = ARRAY` and `arrayValue = vector<int64_t>`. The code generator replaces this with a single `memcpy` from a global constant array, eliminating per-element stores.

#### `array_fill(n, val)`

**Signature:** `array_fill(i64, any) → array`  
**Semantics:** Allocate an array of length `n` and initialize every element to `val`.  
**Time complexity:** O(n)  
**Implementation:** Allocates `(n+1)*8` bytes via `malloc`, stores `n` in slot 0, then loop-stores `val` into slots 1..n.

**Example:**
```omscript
var a = array_fill(100, 42);   // [42, 42, ..., 42] (100 elements)
println(len(a));                // 100
println(a[0]);                  // 42
```

**Constant folding:** When both `n` and `val` are compile-time constants AND `n*sizeof(i64) <= 4096`, the compiler may emit a stack alloca + unrolled stores instead of a heap allocation.

#### Type-annotated literals (planned feature)

Syntax `[]T{...}` is parsed but not yet implemented in code generation.

### 11.3 Indexing, slicing, negative indices

#### Indexing: `arr[i]`

**Semantics:** Return the i-th element (0-based).  
**Bounds checking:**
- **Runtime check** when optimizations are below O3 or the index is not provably in-bounds.
- **Undefined behavior** on out-of-bounds access (may trap with error message or return garbage).

**Implementation** (`codegen_expr.cpp`):
1. Load length: `len = load i64, ptr arr`
2. Check: `0 <= i < len`
3. If valid: compute offset `(i+1)*8`, load element via `InBoundsGEP`.
4. If invalid: print error and call `abort()` / `llvm.trap`.

**Negative indices:** NOT supported. Passing `i < 0` triggers the out-of-bounds path.

**Example:**
```omscript
var a = [10, 20, 30];
println(a[0]);   // 10
println(a[2]);   // 30
println(a[3]);   // Runtime error: array index out of bounds
```

#### Slicing: `arr[start:end]` and `arr[start:]`

**Semantics:**
- `arr[start:end]` — return a NEW array containing elements `[start, end)`.
- `arr[start:]` — return a NEW array from index `start` to the end of `arr`.

**Bounds:**
- `start` must be in `[0, len(arr)]`.
- `end` must be in `[start, len(arr)]`.
- Out-of-bounds slice indices produce empty array or runtime error (depends on optimization level).

**Implementation:**
1. Compute slice length: `sliceLen = end - start`.
2. Allocate new array: `malloc((sliceLen + 1) * 8)`.
3. Store `sliceLen` in slot 0.
4. `memcpy` elements `arr[start..end-1]` into new array slots `1..sliceLen`.

**Time complexity:** O(sliceLen)  
**Allocation:** Always heap (slice never aliases the source array).

**Example:**
```omscript
var a = [10, 20, 30, 40, 50];
var b = a[1:4];     // [20, 30, 40]
var c = a[2:];      // [30, 40, 50]
println(len(b));    // 3
println(len(c));    // 3
```

### 11.4 Mutation

#### Element assignment: `a[i] = v`

**Semantics:** Store value `v` into element `i` of array `a`.  
**Bounds checking:** Same as indexing (runtime check unless provably safe).  
**Implementation:** Convert index to offset, store via `InBoundsGEP`.

**Example:**
```omscript
var a = [1, 2, 3];
a[1] = 99;
println(a[1]);  // 99
```

#### Compound assignment: `a[i] += v`

**Desugars to:** `a[i] = a[i] + v`  
**Optimized:** Load + add + store with a single bounds check (when optimization level >= O1).

**Supported operators:** `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`

**Example:**
```omscript
var a = [10, 20, 30];
a[0] += 5;       // a[0] = 15
a[1] *= 2;       // a[1] = 40
```

### 11.5 Stack-allocated arrays (escape analysis)

**Trigger conditions:**  
When the optimizer proves an array does NOT escape its allocating function (not returned, not stored into a global/struct, not passed to an escaping callee), the allocation is moved from heap (`malloc`) to stack (`alloca`).

**Threshold:** Arrays larger than 4096 bytes remain heap-allocated even when non-escaping (prevents stack overflow).

**Benefits:**
- Eliminates `malloc`/`free` overhead.
- Improved cache locality.
- Enables SROA (Scalar Replacement of Aggregates) — the optimizer may eliminate the array entirely and promote elements to SSA registers.

**Stats counter:** `optStats_.escapeStackAllocs` tracks how many arrays were stack-promoted.

**Example:**
```omscript
fn local_sum() -> int {
    var a = array_fill(10, 1);  // Stack-allocated (doesn't escape)
    return sum(a);
}
```

### 11.6 Array built-ins — Exhaustive list

#### `len(array) → i64`

**Semantics:** Return the logical element count.  
**Time:** O(1)  
**Implementation:** Load the first i64 slot of the array pointer.

**Example:**
```omscript
var a = [1, 2, 3];
println(len(a));  // 3
```

---

#### `push(array, value) → i64`

**Semantics:** Append `value` to the end of `array` (mutates in-place via `realloc`). Returns 0.  
**Time:** Amortized O(1) (uses geometric growth: doubles capacity when full).  
**Side effects:** Mutates `array` pointer (may change the base address).

**Implementation:**
1. Load current length `n`.
2. Compute new size: `(n+2)*8` bytes.
3. Call `realloc(array, newSize)` with power-of-2 growth.
4. Store `value` at offset `(n+1)*8`.
5. Store `n+1` as new length.

**Example:**
```omscript
var a = [10, 20];
push(a, 30);
println(len(a));  // 3
println(a[2]);    // 30
```

---

#### `pop(array) → i64`

**Semantics:** Remove and return the last element of `array`. Decrements length by 1. Runtime error if array is empty.  
**Time:** O(1)  
**Side effects:** Mutates `array` (decrements length in-place).

**Example:**
```omscript
var a = [10, 20, 30];
var x = pop(a);
println(x);        // 30
println(len(a));   // 2
```

---

#### `shift(array) → i64`

**Signature:** `shift(array) → i64`
**Semantics:** Remove and return the first element; shift all remaining
elements left by one slot. Mutates the array in place: length decreases by 1.
Aborts with a runtime error when called on an empty array.
**Time:** O(n)

```omscript
var a:i64[] = [10, 20, 30];
var x:i64 = shift(a);
println(x);        // 10
println(len(a));   // 2
println(a[0]);     // 20
```

---

#### `unshift(array, value) → i64`

**Signature:** `unshift(array, i64) → array`
**Semantics:** Insert `value` at index 0; shift existing elements right by one
slot. Returns the (possibly reallocated) array pointer — assign it back to a
variable to track the new buffer when growth occurs. Reuses the same
power-of-two growth policy as `push`.
**Time:** O(n)

```omscript
var a:i64[] = [20, 30];
a = unshift(a, 10);
println(a[0]);     // 10
println(len(a));   // 3
```

---

#### `array_insert(array, index, value) → i64`

**Signature:** `array_insert(array, i64, i64) → i64`  
**Semantics:** Insert `value` at position `index`; shift elements `[index..len-1]` right by 1. Returns 0.  
**Time:** O(n - index)  
**Bounds:** `index` must be in `[0, len(array)]`.

**Implementation:**
1. Reallocate array to accommodate one more element.
2. `memmove` elements `[index..len-1]` to `[index+1..len]`.
3. Store `value` at `index`.

**Example:**
```omscript
var a = [10, 30, 40];
array_insert(a, 1, 20);  // [10, 20, 30, 40]
```

---

#### `array_remove(array, index) → i64`

**Signature:** `array_remove(array, i64) → i64`  
**Semantics:** Remove element at `index`; shift elements `[index+1..len-1]` left by 1. Returns the removed value.  
**Time:** O(n - index)  
**Bounds:** `index` must be in `[0, len(array))`.

**Example:**
```omscript
var a = [10, 20, 30, 40];
var x = array_remove(a, 2);  // x = 30, a = [10, 20, 40]
```

---

#### `array_slice(array, start, end) → array`

**Signature:** `array_slice(array, i64, i64) → array`  
**Semantics:** Return a NEW array containing elements `[start, end)`.  
**Time:** O(end - start)  
**Alias:** This is the implementation of the slice operator `arr[start:end]`.

**Example:**
```omscript
var a = [10, 20, 30, 40];
var b = array_slice(a, 1, 3);  // [20, 30]
```

---

#### `array_concat(array, array) → array`

**Signature:** `array_concat(array, array) → array`  
**Semantics:** Return a NEW array that is the concatenation of the two input arrays.  
**Time:** O(len(a) + len(b))

**Example:**
```omscript
var a = [1, 2];
var b = [3, 4];
var c = array_concat(a, b);  // [1, 2, 3, 4]
```

---

#### `reverse(array) → array`

**Signature:** `reverse(array) → array`  
**Semantics:** Reverse the array IN-PLACE. Returns the same array pointer.  
**Time:** O(n)  
**Side effects:** Mutates the array.

**Implementation:** Two-pointer swap loop from `i=0`, `j=len-1` until `i >= j`.

**Example:**
```omscript
var a = [1, 2, 3, 4];
reverse(a);
println(a[0]);  // 4
```

---

#### `sort(array) → array`

**Signature:** `sort(array) → array`  
**Semantics:** Sort the array IN-PLACE in ascending order. Returns the array pointer.  
**Time:** O(n log n) (uses `qsort` from C stdlib).  
**Side effects:** Mutates the array.

**Example:**
```omscript
var a = [30, 10, 40, 20];
sort(a);
println(a[0]);  // 10
```

---

#### `array_fill(n, value) → array`

See section 11.2.

---

#### `array_copy(array) → array`

**Signature:** `array_copy(array) → array`  
**Semantics:** Return a NEW array that is a shallow copy of the input.  
**Time:** O(n)

**Example:**
```omscript
var a = [10, 20, 30];
var b = array_copy(a);
b[0] = 99;
println(a[0]);  // 10 (unchanged)
println(b[0]);  // 99
```

---

#### `index_of(array, value) → i64`

**Signature:** `index_of(array, i64) → i64`  
**Semantics:** Return the index of the first occurrence of `value`, or `-1` if not found.  
**Time:** O(n)

**Example:**
```omscript
var a = [10, 20, 30, 20];
println(index_of(a, 20));   // 1
println(index_of(a, 99));   // -1
```

---

#### `array_contains(array, value) → bool`

**Signature:** `array_contains(array, i64) → i64`  
**Semantics:** Return 1 if `value` is in the array, 0 otherwise.  
**Time:** O(n)

**Example:**
```omscript
var a = [10, 20, 30];
println(array_contains(a, 20));  // 1
println(array_contains(a, 99));  // 0
```

---

#### `array_count(array, value) → i64`

**Signature:** `array_count(array, i64) → i64`  
**Semantics:** Return the number of times `value` appears in the array.  
**Time:** O(n)

**Example:**
```omscript
var a = [10, 20, 10, 30, 10];
println(array_count(a, 10));  // 3
```

---

#### `sum(array) → i64`

**Signature:** `sum(array) → i64`  
**Semantics:** Return the sum of all elements.  
**Time:** O(n)  
**Optimizations:**
- Constant-folding when array is a compile-time literal.
- Special case: `sum(array_fill(n, v))` → `n * v` when both constant.
- Special case: `sum(range(a, b))` → arithmetic series formula `(b-a)*(a+b-1)/2`.

**Example:**
```omscript
var a = [10, 20, 30];
println(sum(a));  // 60
```

---

#### `array_product(array) → i64`

**Signature:** `array_product(array) → i64`  
**Semantics:** Return the product of all elements.  
**Time:** O(n)

**Example:**
```omscript
var a = [2, 3, 4];
println(array_product(a));  // 24
```

---

#### `array_mean(array) → i64` / `array_avg(array) → i64` (alias)

**Signature:** `array_mean(array) → i64`  
**Semantics:** Return the integer mean (truncated division: `sum(array) / len(array)`).  
**Time:** O(n)

**Example:**
```omscript
var a = [10, 20, 30];
println(array_mean(a));  // 20
```

---

#### `array_min(array) → i64`

**Signature:** `array_min(array) → i64`  
**Semantics:** Return the minimum element. Runtime error if array is empty.  
**Time:** O(n)

**Example:**
```omscript
var a = [30, 10, 20];
println(array_min(a));  // 10
```

---

#### `array_max(array) → i64`

**Signature:** `array_max(array) → i64`  
**Semantics:** Return the maximum element. Runtime error if array is empty.  
**Time:** O(n)

**Example:**
```omscript
var a = [30, 10, 20];
println(array_max(a));  // 30
```

---

#### `array_last(array) → i64`

**Signature:** `array_last(array) → i64`  
**Semantics:** Return the last element. Runtime error if array is empty.  
**Time:** O(1)

**Example:**
```omscript
var a = [10, 20, 30];
println(array_last(a));  // 30
```

---

#### `array_take(array, n) → array`

**Signature:** `array_take(array, i64) → array`  
**Semantics:** Return a NEW array containing the first `n` elements. If `n > len(array)`, return a copy of the entire array.  
**Time:** O(min(n, len(array)))

**Example:**
```omscript
var a = [10, 20, 30, 40];
var b = array_take(a, 2);  // [10, 20]
```

---

#### `array_drop(array, n) → array`

**Signature:** `array_drop(array, i64) → array`  
**Semantics:** Return a NEW array with the first `n` elements removed. If `n >= len(array)`, return an empty array.  
**Time:** O(len(array) - n)

**Example:**
```omscript
var a = [10, 20, 30, 40];
var b = array_drop(a, 2);  // [30, 40]
```

---

#### `array_unique(array) → array`

**Signature:** `array_unique(array) → array`  
**Semantics:** Return a NEW array with consecutive duplicate elements removed.  
**Time:** O(n)

**Example:**
```omscript
var a = [10, 10, 20, 20, 30];
var b = array_unique(a);  // [10, 20, 30]
```

---

#### `array_rotate(array, n) → array`

**Signature:** `array_rotate(array, i64) → array`  
**Semantics:** Rotate the array IN-PLACE left by `n` positions (positive `n` → left, negative `n` → right).  
**Time:** O(len(array))

**Example:**
```omscript
var a = [10, 20, 30, 40];
array_rotate(a, 1);  // [20, 30, 40, 10]
```

---

#### `array_zip(array, array) → array` (interleave)

**Signature:** `array_zip(array, array) → array`  
**Semantics:** Return a NEW array interleaving elements: `[a[0], b[0], a[1], b[1], ...]`. Length = `2 * min(len(a), len(b))`.  
**Time:** O(min(len(a), len(b)))

**Example:**
```omscript
var a = [1, 2, 3];
var b = [10, 20, 30];
var c = array_zip(a, b);  // [1, 10, 2, 20, 3, 30]
```

---

### 11.7 Higher-order array operations and lambda interaction

#### `array_map(array, fn) → array`

**Signature:** `array_map(array, function) → array`  
**Semantics:** Return a NEW array where each element is `fn(element)`.  
**Time:** O(n * cost(fn))

**Example:**
```omscript
fn double(x) { return x * 2; }

var a = [1, 2, 3];
var b = array_map(a, double);  // [2, 4, 6]
```

---

#### `array_filter(array, fn) → array`

**Signature:** `array_filter(array, function) → array`  
**Semantics:** Return a NEW array containing only elements where `fn(element)` is truthy.  
**Time:** O(n * cost(fn))

**Example:**
```omscript
fn is_even(x) { return x % 2 == 0; }

var a = [1, 2, 3, 4];
var b = array_filter(a, is_even);  // [2, 4]
```

---

#### `array_reduce(array, fn, init) → any`

**Signature:** `array_reduce(array, function, any) → any`  
**Semantics:** Fold the array left-to-right: `result = fn(fn(fn(init, a[0]), a[1]), ...)`.  
**Time:** O(n * cost(fn))

**Example:**
```omscript
fn add(acc, x) { return acc + x; }

var a = [1, 2, 3, 4];
var s = array_reduce(a, add, 0);  // 10
```

---

#### `array_find(array, fn) → i64`

**Signature:** `array_find(array, function) → i64`  
**Semantics:** Return the first element where `fn(element)` is truthy, or `-1` if none.  
**Time:** O(n * cost(fn)) (short-circuits on first match)

**Example:**
```omscript
fn gt_10(x) { return x > 10; }

var a = [5, 15, 25];
var x = array_find(a, gt_10);  // 15
```

---

#### `array_any(array, fn) → bool`

**Signature:** `array_any(array, function) → i64`  
**Semantics:** Return 1 if at least one element satisfies `fn`, 0 otherwise. Short-circuits on first match.  
**Time:** O(n * cost(fn))

**Example:**
```omscript
fn is_neg(x) { return x < 0; }

var a = [1, 2, -3];
println(array_any(a, is_neg));  // 1
```

---

#### `array_every(array, fn) → bool` (alias: `array_all`)

**Signature:** `array_every(array, function) → i64`  
**Semantics:** Return 1 if ALL elements satisfy `fn`, 0 otherwise. Short-circuits on first failure.  
**Time:** O(n * cost(fn))

**Example:**
```omscript
fn is_pos(x) { return x > 0; }

var a = [1, 2, 3];
println(array_every(a, is_pos));  // 1
```

---

### 11.8 Array-of-string special handling

**String-array tracking:** When an array is known to contain string pointers (i.e., every element is an i64 holding a `char*`), the compiler tracks it in `stringArrays_` set. This enables:
- Type-aware code generation for `join`, `split`, etc.
- Automatic memory management hints (each string element may need freeing).

**Detection heuristics:**
- Array literal where all elements are string literals → marked as string-array.
- Result of `str_split()` → marked as string-array.
- Result of `str_chars()` → marked as string-array.

**Example:**
```omscript
var words = str_split("hello world", " ");  // words is a string-array
var joined = str_join(words, "-");          // "hello-world"
```

---

## 12. Strings — Complete API

### 12.1 String model

**Heap allocation:** All OmScript strings are heap-allocated C-style null-terminated strings (`char*`).

**Length-prefixed:** NOT stored explicitly in a header (unlike arrays). String lengths are computed via `strlen()` on demand unless cached (see string-length cache optimization in `str_concat`).

**Immutability:** Strings are logically immutable from the user's perspective. Modifying operations (`str_upper`, `str_replace`, etc.) return NEW strings. However, `string_index_assign` (if implemented) may allow in-place mutation.

**Representation:**
- **Local/literal:** Direct pointer (LLVM `i8*`).
- **Cross-function:** Passed as i64 (ptrtoint) to avoid pointer-type mismatches in generic contexts; callers convert back via `IntToPtr`.

**Metadata:**
- `tbaaStringData_`: TBAA tag for string data loads/stores (orthogonal to array TBAA).
- `stringReturningFunctions_`: Set tracking which functions return strings (enables downstream tracking).

---

### 12.2 String literals & escape sequences

**Syntax:**
```omscript
var s1 = "hello";
var s2 = "line1\nline2";
var s3 = "tab\tseparated";
```

**Supported escape sequences:**
- `\n` — newline (0x0A)
- `\t` — tab (0x09)
- `\r` — carriage return (0x0D)
- `\\` — backslash
- `\"` — double quote
- `\xHH` — hex escape (two hex digits)
- `\0` — null (0x00)

**Storage:** String literals are stored as LLVM global constants (`.rodata` section) and referenced via `GlobalString`.

---

### 12.3 String interpolation full rules

**Syntax:** Embedded expressions within string literals using `${...}`:
```omscript
var x = 42;
var s = "The answer is ${x}";  // "The answer is 42"
```

**Implementation:**
1. Parser rewrites `"...${expr}..."` into `str_concat("...", to_string(expr), ...)`.
2. Multiple interpolations are chained: `str_concat(str_concat(a, b), c)`.
3. Nested expressions are evaluated left-to-right.

**Supported expression types:**
- Integers → converted via `snprintf("%lld", ...)`
- Floats → converted via `snprintf("%g", ...)`
- Strings → used directly
- Other → calls `to_string()` if available

**Example:**
```omscript
var x = 10;
var y = 20;
println("x=${x}, y=${y}, sum=${x+y}");  // "x=10, y=20, sum=30"
```

---

### 12.4 Indexing and slicing

#### Indexing: `s[i]`

**Semantics:** Return the ASCII value (as i64) of the character at index `i`.  
**Bounds checking:** Runtime check that `0 <= i < strlen(s)`. Out-of-bounds triggers runtime error.

**Example:**
```omscript
var s = "hello";
println(s[0]);  // 104 ('h')
println(s[1]);  // 101 ('e')
```

#### Slicing: `s[start:end]`

**Semantics:** Return a NEW string containing characters `[start, end)`.  
**Allocation:** Always heap (via `malloc` + `memcpy` + null terminator).

**Example:**
```omscript
var s = "hello";
var t = s[1:4];  // "ell"
```

---

### 12.5 Concatenation rules

#### Operator `+`

**Syntax:** `s1 + s2`  
**Semantics:** Return a NEW string that is the concatenation of `s1` and `s2`.  
**Implementation:** Calls `str_concat(s1, s2)`.

**Example:**
```omscript
var s = "hello" + " " + "world";  // "hello world"
```

#### Operator `*` (repeat)

**Syntax:** `s * n`  
**Semantics:** Return a NEW string that is `s` repeated `n` times.  
**Implementation:** Calls `str_repeat(s, n)`.

**Example:**
```omscript
var s = "ab" * 3;  // "ababab"
```

---

### 12.6 Comparison

**Operators:** `==`, `!=`, `<`, `<=`, `>`, `>=`

**Semantics:**
- `==` / `!=` — lexicographic equality (uses C `strcmp() == 0`).
- `<` / `<=` / `>` / `>=` — lexicographic ordering (uses C `strcmp()` with `< 0`, `<= 0`, etc.).

**Example:**
```omscript
var a = "apple";
var b = "banana";
println(a < b);      // 1 (true)
println(a == "apple"); // 1 (true)
```

---

### 12.7 String built-ins — Exhaustive list

#### `str_len(string) → i64` / `len(string) → i64` (when applied to string)

**Semantics:** Return the number of characters (bytes) in the string, excluding the null terminator.  
**Time:** O(n) (calls C `strlen()`).

**Example:**
```omscript
println(str_len("hello"));  // 5
```

---

#### `char_at(string, i64) → i64`

**Semantics:** Return the ASCII value of the character at index `i`. Runtime error if out-of-bounds.  
**Time:** O(1)

**Example:**
```omscript
var s = "hello";
println(char_at(s, 0));  // 104 ('h')
```

---

#### `str_substr(string, i64, i64) → string` (alias: `substr`)

**Signature:** `str_substr(string, start, length) → string`  
**Semantics:** Return a NEW string of `length` characters starting at `start`.  
**Time:** O(length)

**Example:**
```omscript
var s = "hello";
var t = str_substr(s, 1, 3);  // "ell"
```

---

#### `str_contains(string, string) → bool`

**Semantics:** Return 1 if the first string contains the second as a substring, 0 otherwise.  
**Time:** O(m * n) (uses C `strstr()`).

**Example:**
```omscript
println(str_contains("hello", "ell"));  // 1
println(str_contains("hello", "xyz"));  // 0
```

---

#### `str_starts_with(string, string) → bool`

**Semantics:** Return 1 if the first string starts with the second, 0 otherwise.  
**Time:** O(len(prefix))

**Example:**
```omscript
println(str_starts_with("hello", "hel"));  // 1
```

---

#### `str_ends_with(string, string) → bool`

**Semantics:** Return 1 if the first string ends with the second, 0 otherwise.  
**Time:** O(len(suffix))

**Example:**
```omscript
println(str_ends_with("hello", "llo"));  // 1
```

---

#### `str_index_of(string, string) → i64` (alias: `str_find`)

**Semantics:** Return the index of the first occurrence of the second string, or `-1` if not found.  
**Time:** O(m * n)

**Example:**
```omscript
println(str_index_of("hello", "ll"));  // 2
println(str_index_of("hello", "z"));   // -1
```

---

#### `str_replace(string, string, string) → string`

**Signature:** `str_replace(haystack, needle, replacement) → string`  
**Semantics:** Return a NEW string with ALL occurrences of `needle` replaced by `replacement`.  
**Time:** O(len(haystack) * len(needle))

**Example:**
```omscript
var s = str_replace("hello world", "world", "OmScript");
println(s);  // "hello OmScript"
```

---

#### `str_trim(string) → string`

**Semantics:** Return a NEW string with leading and trailing whitespace removed.  
**Time:** O(n)

**Example:**
```omscript
var s = str_trim("  hello  ");
println(s);  // "hello"
```

---

#### `str_lstrip(string) → string`

**Semantics:** Return a NEW string with leading whitespace removed.  
**Time:** O(n)

**Example:**
```omscript
var s = str_lstrip("  hello");
println(s);  // "hello"
```

---

#### `str_rstrip(string) → string`

**Semantics:** Return a NEW string with trailing whitespace removed.  
**Time:** O(n)

**Example:**
```omscript
var s = str_rstrip("hello  ");
println(s);  // "hello"
```

---

#### `str_upper(string) → string`

**Semantics:** Return a NEW string with all lowercase letters converted to uppercase.  
**Time:** O(n)

**Example:**
```omscript
println(str_upper("hello"));  // "HELLO"
```

---

#### `str_lower(string) → string`

**Semantics:** Return a NEW string with all uppercase letters converted to lowercase.  
**Time:** O(n)

**Example:**
```omscript
println(str_lower("HELLO"));  // "hello"
```

---

#### `str_repeat(string, i64) → string`

**Semantics:** Return a NEW string that is the input repeated `n` times.  
**Time:** O(n * len(string))

**Example:**
```omscript
println(str_repeat("ab", 3));  // "ababab"
```

---

#### `str_reverse(string) → string`

**Semantics:** Return a NEW string with characters in reverse order.  
**Time:** O(n)

**Example:**
```omscript
println(str_reverse("hello"));  // "olleh"
```

---

#### `str_split(string, string) → array`

**Signature:** `str_split(string, delimiter) → array`  
**Semantics:** Return an array of strings by splitting the input at each occurrence of `delimiter`.  
**Time:** O(n)

**Example:**
```omscript
var words = str_split("a,b,c", ",");
println(len(words));  // 3
println(words[0]);    // "a"
```

---

#### `str_join(array, string) → string`

**Signature:** `str_join(array_of_strings, separator) → string`  
**Semantics:** Concatenate all strings in the array with `separator` between them.  
**Time:** O(total_length)

**Example:**
```omscript
var words = ["a", "b", "c"];
var s = str_join(words, ",");
println(s);  // "a,b,c"
```

---

#### `str_count(string, string) → i64`

**Semantics:** Count non-overlapping occurrences of the second string in the first.  
**Time:** O(m * n)

**Example:**
```omscript
println(str_count("ababab", "ab"));  // 3
```

---

#### `str_remove(string, string) → string`

**Semantics:** Return a NEW string with ALL occurrences of the second string removed.  
**Time:** O(m * n)

**Example:**
```omscript
var s = str_remove("hello world", "o");
println(s);  // "hell wrld"
```

---

#### `str_pad_left(string, i64, string) → string`

**Signature:** `str_pad_left(string, width, pad_char) → string`  
**Semantics:** Return a NEW string padded on the left with `pad_char` to reach `width` total characters. If already >= `width`, return unchanged.  
**Time:** O(width)

**Example:**
```omscript
println(str_pad_left("42", 5, "0"));  // "00042"
```

---

#### `str_pad_right(string, i64, string) → string`

**Signature:** `str_pad_right(string, width, pad_char) → string`  
**Semantics:** Return a NEW string padded on the right with `pad_char` to reach `width` total characters.  
**Time:** O(width)

**Example:**
```omscript
println(str_pad_right("42", 5, "0"));  // "42000"
```

---

#### `str_chars(string) → array`

**Semantics:** Return an array of one-character strings (one per character in the input).  
**Time:** O(n)

**Example:**
```omscript
var chars = str_chars("abc");
println(len(chars));  // 3
println(chars[0]);    // "a"
```

---

#### `str_filter(string, fn) → string`

**Signature:** `str_filter(string, function) → string`  
**Semantics:** Return a NEW string containing only characters where `fn(char_code)` is truthy.  
**Time:** O(n * cost(fn))

**Example:**
```omscript
fn is_vowel(c) { return c == 97 || c == 101 || c == 105 || c == 111 || c == 117; }
var s = str_filter("hello", is_vowel);
println(s);  // "eo"
```

---

### 12.8 String formatting

#### `str_format(string, ...) → string`

**Signature:** `str_format(format_string, arg1, arg2, ...) → string`  
**Semantics:** Format a string using C `snprintf`-style format specifiers. Returns a NEW heap-allocated string.  
**Supported specifiers:** `%d`, `%lld`, `%s`, `%f`, `%g`, `%x`, etc. (full C printf subset).

**Example:**
```omscript
var s = str_format("x=%d, y=%f", 10, 3.14);
println(s);  // "x=10, y=3.140000"
```

---

### 12.9 String interning (when applies)

**Not explicitly implemented** in the current code generation. String literals are global constants, but runtime string deduplication is not performed. Future optimization may add interning for frequently used strings.

---

## 13. Dictionaries / Maps — Complete API

### 13.1 Dict model

**Representation:** Opaque pointer to a C++ `std::unordered_map<int64_t, int64_t>` allocated on the heap.

**Key/value types:**
- Keys: i64 (integers or string pointers cast to i64).
- Values: i64 (integers, floats-as-bits, or string pointers).

**Hash table:** Backed by C++ STL implementation (chaining with linked lists; amortized O(1) operations).

**Ownership:** Dictionaries are heap-allocated and NOT tracked by the ownership system (manual memory management via `map_new`, no automatic invalidation).

---

### 13.2 Literals & construction

#### `map_new() → dict`

**Semantics:** Allocate and return a new empty dictionary.  
**Time:** O(1)

**Example:**
```omscript
var m = map_new();
```

#### Literal syntax (planned feature)

**Syntax:** `dict { key1: val1, key2: val2 }`  
**Status:** Parsed but not yet implemented in code generation.

---

### 13.3 Access, insert, update, delete

#### `map_set(dict, key, value) → dict`

**Semantics:** Insert or update `key` with `value`. Returns the dict pointer.  
**Time:** Amortized O(1)

**Example:**
```omscript
var m = map_new();
map_set(m, 10, 100);
map_set(m, 20, 200);
```

---

#### `map_get(dict, key) → value`

**Semantics:** Return the value associated with `key`, or 0 if not present.  
**Time:** Amortized O(1)

**Example:**
```omscript
var m = map_new();
map_set(m, 10, 100);
println(map_get(m, 10));  // 100
println(map_get(m, 99));  // 0
```

---

#### `map_has(dict, key) → bool`

**Semantics:** Return 1 if `key` is in the dictionary, 0 otherwise.  
**Time:** Amortized O(1)

**Example:**
```omscript
var m = map_new();
map_set(m, 10, 100);
println(map_has(m, 10));  // 1
println(map_has(m, 99));  // 0
```

---

#### `map_remove(dict, key) → dict`

**Semantics:** Remove `key` from the dictionary. Returns the dict pointer.  
**Time:** Amortized O(1)

**Example:**
```omscript
var m = map_new();
map_set(m, 10, 100);
map_remove(m, 10);
println(map_has(m, 10));  // 0
```

---

### 13.4 Built-ins

#### `map_keys(dict) → array`

**Semantics:** Return an array of all keys in the dictionary (order is arbitrary).  
**Time:** O(n)

**Example:**
```omscript
var m = map_new();
map_set(m, 10, 100);
map_set(m, 20, 200);
var keys = map_keys(m);
println(len(keys));  // 2
```

---

#### `map_values(dict) → array`

**Semantics:** Return an array of all values in the dictionary (order is arbitrary).  
**Time:** O(n)

**Example:**
```omscript
var m = map_new();
map_set(m, 10, 100);
map_set(m, 20, 200);
var vals = map_values(m);
println(len(vals));  // 2
```

---

#### `map_size(dict) → i64` / `len(dict) → i64`

**Semantics:** Return the number of key-value pairs.  
**Time:** O(1)

**Example:**
```omscript
var m = map_new();
map_set(m, 10, 100);
println(map_size(m));  // 1
```

---

#### `map_merge(dict, dict) → dict`

**Semantics:** Return a NEW dictionary that is the union of the two inputs. If a key exists in both, the second dict's value wins.  
**Time:** O(n + m)

**Example:**
```omscript
var m1 = map_new();
map_set(m1, 10, 100);
var m2 = map_new();
map_set(m2, 10, 999);
map_set(m2, 20, 200);
var m3 = map_merge(m1, m2);
println(map_get(m3, 10));  // 999
println(map_get(m3, 20));  // 200
```

---

#### `map_invert(dict) → dict`

**Semantics:** Return a NEW dictionary with keys and values swapped. If multiple keys have the same value, the last one wins.  
**Time:** O(n)

**Example:**
```omscript
var m = map_new();
map_set(m, 10, 100);
map_set(m, 20, 200);
var inv = map_invert(m);
println(map_get(inv, 100));  // 10
```

---

#### `map_filter(dict, fn) → dict`

**Signature:** `map_filter(dict, function) → dict`  
**Semantics:** Return a NEW dictionary containing only entries where `fn(key, value)` is truthy.  
**Time:** O(n * cost(fn))

**Example:**
```omscript
fn keep_large_vals(k, v) { return v > 100; }
var m = map_new();
map_set(m, 10, 50);
map_set(m, 20, 200);
var m2 = map_filter(m, keep_large_vals);
println(map_size(m2));  // 1
```

---

### 13.5 Iteration order guarantees

**No guarantee.** Iteration order (in `map_keys`, `map_values`, `map_filter`) depends on the internal hash table bucket order and is NOT stable across insertions/deletions.

---

### 13.6 Method-call chaining

**Supported:** All `map_*` functions return the dict pointer, enabling chaining:
```omscript
var m = map_new();
map_set(map_set(m, 10, 100), 20, 200);
```

---

## 14. Structs

### 14.1 Definition syntax

**Syntax:**
```omscript
struct Name { field1, field2, field3 }
```

**Fields:** Comma-separated identifiers (no type annotations in the current syntax).

**Example:**
```omscript
struct Point { x, y }
struct Person { name, age }
```

**Global scope:** Struct declarations must appear at the top level (not inside functions).

---

### 14.2 Field types, attributes

**Field types:** All fields are i64 (integers, floats-as-bits, or pointers).

**Attributes:** `@packed` is parsed but not yet implemented. Future feature for controlling memory layout.

**Alignment:** Fields are 8-byte aligned (standard i64 alignment).

---

### 14.3 Struct literal syntax

**Syntax:**
```omscript
var p = StructName { field1: expr1, field2: expr2 }
```

**Field order:** Must match the declaration order.

**Example:**
```omscript
struct Point { x, y }

var p = Point { x: 10, y: 20 };
```

---

### 14.4 Field access & mutation

#### Access: `s.field`

**Semantics:** Return the value of `field` in struct `s`.  
**Implementation:** Load via `GEP` (GetElementPtr) with offset calculated from field index.

**Example:**
```omscript
struct Point { x, y }
var p = Point { x: 10, y: 20 };
println(p.x);  // 10
```

#### Mutation: `s.field = value`

**Semantics:** Store `value` into `field` of struct `s`.  
**Implementation:** Store via `GEP`.

**Example:**
```omscript
p.x = 30;
println(p.x);  // 30
```

---

### 14.5 Methods

**Syntax:** Methods are parsed as free functions with the struct name prefix:
```omscript
fn StructName::method(self, args...) {
    // body
}
```

**Status:** Parsed but method-call syntax (`obj.method()`) is NOT yet implemented. Workaround: call as `StructName::method(obj, args)`.

---

### 14.6 Operator overloading

**Not supported** in the current implementation. Future feature.

---

### 14.7 Memory layout & SROA interaction

**Layout:** Contiguous i64 fields in declaration order.

**SROA (Scalar Replacement of Aggregates):** When a struct does not escape its allocating function, LLVM's SROA pass may promote individual fields to SSA registers, eliminating the struct allocation entirely.

**Example:**
```omscript
fn local_point() {
    var p = Point { x: 10, y: 20 };
    return p.x + p.y;  // SROA → no heap allocation, x and y are registers
}
```

---

### 14.8 Heap-allocated representation (opaque pointer)

**Opaque pointer:** Structs are represented as i64 (ptrtoint of the heap allocation) when crossing function boundaries.

**Allocation:** Always heap (via `malloc` with size = `num_fields * 8`).

---

### 14.9 `invalidate` on struct

**Semantics:** `invalidate s;` frees the heap allocation immediately.  
**Effect:** Struct pointer becomes invalid; subsequent access is undefined behavior.

**Example:**
```omscript
struct Point { x, y }
var p = Point { x: 10, y: 20 };
invalidate p;
// p is now dead
```

---

## 15. Enums

### 15.1 Definition syntax

**Syntax:**
```omscript
enum Name {
    VARIANT1,
    VARIANT2 = value,
    VARIANT3
}
```

**Auto-increment:** If a variant has no explicit value, it is `prev_value + 1`. First variant defaults to 0.

**Example:**
```omscript
enum Color {
    RED,         // 0
    GREEN = 10,  // 10
    BLUE         // 11
}
```

---

### 15.2 Variant access and discriminant

**Access:** Enum variants are exposed as global constants with the naming convention `EnumName_VariantName`.

**Example:**
```omscript
enum Status { OK, ERROR }

var s = Status_OK;   // 0
println(s);          // 0
```

**Discriminant:** The integer value assigned to each variant.

---

### 15.3 Pattern matching with enums

**Switch statements:**
```omscript
enum Color { RED, GREEN, BLUE }

var c = Color_GREEN;
switch (c) {
    case 0: println("red"); break;
    case 1: println("green"); break;
    case 2: println("blue"); break;
}
```

**When expressions:** (if implemented)
```omscript
when (c) {
    Color_RED => println("red"),
    Color_GREEN => println("green"),
    Color_BLUE => println("blue")
}
```

---

### 15.4 Underlying integer representation

**Type:** All enum variants are i64 constants.

**Scope:** Enum names are tracked in `enumNames_` set; variant names are global constants.

---

## 16. Error Handling

OmScript's error-handling model is **deliberately minimal** and very different from C++/Java/Python exceptions:

- There is **no `try { ... }` block.** The `try` keyword is reserved by the lexer but unused by the parser.
- A `catch(N) { ... }` block is a **top-level statement inside a function body**, not a clause attached to a `try`.
- `throw expr;` evaluates `expr` to an `i64` "error code" and dispatches via a compile-time **switch table** to the matching `catch(N)` block in the same function. There is no exception variable, no message payload, no stack object, and no inheritance hierarchy.
- There is **no stack unwinding.** Destructors / `defer` blocks (§7.6) registered in stack frames between the throw and the matching catch are **not run**. Heap allocations made between the two are leaked.
- Compilation lowers `throw` directly to an LLVM `switch` instruction — there is no `setjmp`/`longjmp`, no DWARF unwind tables, and no runtime exception object.

This design makes errors essentially zero-cost when not raised (a tracked basic block per `catch` and one `switch` per `throw`) and gives the optimizer full visibility into the control flow.

### 16.1 `throw expr;`

**Syntax:**
```ebnf
throw_stmt ::= 'throw' expression ';'
```

**Semantics:**
1. `expression` is evaluated and coerced to `i64`. (Any non-integer value is implicitly normalized.)
2. The compiler emits an LLVM `switch` against the value, with a case arm for every `catch(N)` block in the **enclosing function only**.
3. If a case key matches, control jumps directly to that handler block — execution does not return.
4. If no key matches (or the function has no `catch` blocks at all), the program prints `Runtime error: unmatched throw` (or `unhandled throw at line N` when there are no handlers at all) and calls `abort()`.

**Important constraints:**
- `throw` only dispatches to handlers in the **current function**. Throws **do not propagate up the call stack** — there is no unwinding. A `throw` in a callee with no matching local `catch` aborts the program even if the caller has a matching `catch`.
- Because dispatch is a `switch` over the integer value, only **integer literals** (or values equal to one) can match. The catch key must be an integer or string literal — see §16.2.
- String catch keys are mapped to internal integer IDs at compile time. This means **string throws practically only match if you `throw <same-string-literal>;` from the same translation unit** and the codegen routes that literal through the same id-assigning path. For runtime-computed strings this will not match — prefer integer codes.

**Example:**
```omscript
fn check(x) {
    if (x < 0) {
        throw 1;        // "negative" error
    }
    if (x == 0) {
        throw 2;        // "zero" error
    }
    return 100 / x;
}

fn main() {
    var y = check(-5);  // throws 1
    println(y);         // never reached
    return 0;

    catch(1) {
        println("negative input");
        return 1;
    }
    catch(2) {
        println("zero input");
        return 2;
    }
}
```

---

### 16.2 `catch(N) { ... }`

**Syntax:**
```ebnf
catch_stmt ::= 'catch' '(' (integer_literal | string_literal) ')' block
```

**Placement:** A `catch` block is a **top-level statement of a function body** — it sits alongside other statements, *not* immediately after a `try`. Conceptually a function declares a fixed table of (key → handler) pairs and any `throw` in that function dispatches into the table.

**Restrictions:**
- The catch key **must be a literal** — an integer (`catch(42)`) or a string (`catch("io_error")`). Variables, expressions, and constants are not allowed.
- **Duplicate keys in the same function are a compile error** (`Duplicate catch(N) block in the same function`).
- Catch blocks **cannot be nested inside other blocks** — only top-level statements of the function body are scanned. A `catch` placed inside an `if` or `while` body is effectively dead code.
- There is **no exception variable** — the `catch (e) { ... }` form does *not* exist. The thrown value is consumed by the `switch` dispatch and is not exposed inside the handler.

**Control flow:**
- Normal (non-throwing) execution falls through `catch` blocks as if they were not there — the compiler emits a branch around each handler's basic block.
- After a handler runs, control flows through to whatever statement follows the `catch` in the source (a "merge" block). To stop processing, end the handler with `return` (as in §16.1's example).

**Example — multi-handler function:**
```omscript
fn parse_and_use(s: string) {
    if (str_len(s) == 0) { throw 10; }
    var n = str_to_int(s);
    if (n < 0) { throw 20; }
    return n * 2;

    catch(10) {
        println("empty input");
        return -1;
    }
    catch(20) {
        println("negative number");
        return -2;
    }
}
```

---

### 16.3 `assert(cond)`

**Syntax:** Built-in function, **single argument only**:
```ebnf
assert_call ::= 'assert' '(' expression ')'
```

> **Note:** OmScript's `assert` does **not** accept a custom message argument. `assert(cond, "msg")` is a compile error. The runtime always prints `Runtime error: assertion failed at line N`.

**Semantics:**
1. Evaluates `cond` and coerces to a 1-bit boolean.
2. If true: returns `1` (the call evaluates as an expression to `1`).
3. If false: prints `Runtime error: assertion failed at line N` to stdout and calls `abort()`.

**Optimizer hint:** The compiler tags the success branch with a 1000:1 branch-weight, telling LLVM to lay out the failure path cold.

**`assert(cond)` vs `assume(cond)`:**

| Built-in | Failure behaviour | Compiler treatment |
| --- | --- | --- |
| `assert(cond)` | Aborts the program at runtime | Emits a real check |
| `assume(cond)` | **Undefined behaviour** if violated | Lowers to `llvm.assume` — no runtime check, used as an optimization hint (§7.10) |

Use `assert` for safety-critical invariants you want enforced. Use `assume` only when you can prove the predicate holds and want the optimizer to exploit it.

**Example:**
```omscript
fn divide(a: int, b: int) {
    assert(b != 0);   // aborts on b == 0 with line number
    return a / b;
}
```

---

### 16.4 Runtime error model

Runtime errors in OmScript fall into three categories, all of which **terminate the program immediately** via `abort()` (or LLVM's `llvm.trap` for some classes). There is **no stack unwinding, no destructor invocation, and no opportunity to handle the error** — they are program-fatal.

| Error class | Trigger | Message form |
| --- | --- | --- |
| Bounds check | Out-of-range array / string index | `Runtime error: index out of bounds` |
| Division by zero | Integer `/` or `%` with zero divisor | `Runtime error: division by zero` |
| Zero-step loop | `for (i in 0...10...0)` | `Runtime error: for loop step is zero` |
| Assertion failure | `assert(false)` | `Runtime error: assertion failed at line N` |
| Unmatched throw | `throw N;` with no `catch(N)` in the function | `Runtime error: unmatched throw` |
| Unhandled throw | `throw N;` in a function with **no** catch blocks at all | `Runtime error: unhandled throw at line N` |

Heap allocations live until program exit (or until the OS reclaims them on `abort`). User-visible side effects (file I/O, stdout buffers) occur in whatever state they were in when the trap fired — buffered output may be lost.

---

### 16.5 Compile-time vs runtime errors

Errors are reported in two tiers:

**Compile-time errors** — produced by the lexer, parser, or code generator before any code is emitted. The compiler exits with a non-zero status and prints a diagnostic with file, line, and (where available) column.

| Class | Examples |
| --- | --- |
| Lexical | Unterminated string, invalid character, malformed numeric literal |
| Syntactic | Missing `;`, mismatched braces, unexpected token |
| Semantic | Undefined variable / function, duplicate `catch` key, `thread_create` non-literal arg, OPTMAX violation (§18.2) |
| Type | OPTMAX type-annotation requirement, integer-cast on incompatible type |
| Resource | Source file > 100 MB, parser nesting depth > 256, IR > 1,000,000 instructions |

**Runtime errors** — produced by checks inserted into the generated code. All of them are program-fatal (see §16.4).

**Recoverable vs non-recoverable:**
- **Recoverable (within a function):** `throw N;` matched by a local `catch(N)` block.
- **Non-recoverable:** every other runtime error class. Bounds, divide-by-zero, asserts, and unmatched throws all abort the process.

---

## 17. Memory and Ownership System

### 17.0 Ownership states

**Enum definition** (from `codegen.h`):
```cpp
enum class OwnershipState {
    Owned,        // Variable owns its value — full read/write access
    Borrowed,     // Has ≥1 immutable borrows — readable but not writable
    MutBorrowed,  // Has one mutable alias — source is completely locked
    Frozen,       // Permanently immutable — all loads are invariant
    Moved,        // Ownership transferred out — use is a compile error
    Invalidated   // Explicitly killed — use is a compile error
};
```

**Tracking:** Per-variable borrow state stored in `VarBorrowState`:
```cpp
struct VarBorrowState {
    int  immutBorrowCount = 0;
    bool mutBorrowed = false;
    bool moved = false;
    bool invalidated = false;
    bool frozen = false;
};
```

**State transitions:**
- `Owned` → `Borrowed` (immutable borrow)
- `Owned` → `MutBorrowed` (mutable borrow)
- `Owned` → `Frozen` (freeze permanently)
- `Owned` → `Moved` (move ownership)
- Any → `Invalidated` (explicit kill)

---

### 17.1 Move semantics — `move var x = expr;`

**Syntax:**
```omscript
move var x = expr;
```

**Semantics:**
1. Evaluate `expr` and bind to `x`.
2. Mark the source of `expr` (if it's a variable) as `Moved`.
3. Any subsequent use of the source is a compile-time error.

**Example:**
```omscript
var a = 42;
move var b = a;
println(b);  // 42
// println(a);  // ERROR: use of moved variable
```

**Move expression:** `var y = move x;`
- Transfer ownership from `x` to `y`.
- `x` becomes dead.

---

### 17.2 Borrow — `borrow var x = expr;`

**Syntax:**
```omscript
borrow var x = expr;
```

**Semantics:**
1. Create an immutable alias `x` to the source variable.
2. Increment `immutBorrowCount` of the source.
3. Source remains readable but NOT writable while the borrow is active.
4. When `x` goes out of scope, decrement `immutBorrowCount`.

**Example:**
```omscript
var a = 42;
{
    borrow var ref = a;
    println(ref);  // 42
    println(a);    // 42 (source still readable)
    // a = 99;     // ERROR: cannot write to borrowed variable
}
a = 99;  // OK now (borrow ended)
```

---

### 17.3 Borrow mut — `borrow mut var x = expr;`

**Syntax:**
```omscript
borrow mut var x = expr;
```

**Semantics:**
1. Create a mutable alias `x` to the source variable.
2. Set `mutBorrowed = true` for the source.
3. Source is completely LOCKED (no reads or writes) while the mutable borrow is active.
4. When `x` goes out of scope, clear `mutBorrowed`.

**Example:**
```omscript
var a = 42;
{
    borrow mut var ref = a;
    ref = 99;
    // println(a);  // ERROR: cannot read mutably borrowed variable
}
println(a);  // 99 (borrow ended)
```

---

### 17.4 `invalidate x;`

**Semantics:** Immediately free the heap allocation associated with `x` and mark it as dead.

**Which types get freed:**
- **Strings:** Calls `free()` on the string pointer.
- **Arrays (heap-allocated):** Calls `free()` on the array pointer.
- **Dictionaries:** Calls destructor on the `std::unordered_map` and `free()` on the wrapper.
- **Structs:** Calls `free()` on the struct pointer.
- **Heap pointers (`ptr`):** Calls `free()` if the pointer was allocated via `malloc`/`calloc`/`realloc`.
- **BigInts:** Calls destructor on the GMP `mpz_t` and `free()` on the wrapper.

**Tracking-set cleanup:** All compile-time tracking maps (`stringVars_`, `nonNegValues_`, etc.) are cleared of the variable.

**Dead-var poisoning:** The variable's `VarBorrowState` is set to `invalidated = true`; any subsequent use is a compile-time error.

**Example:**
```omscript
var s = "hello";
println(s);  // "hello"
invalidate s;
// println(s);  // ERROR: use of invalidated variable
```

---

### 17.5 `freeze x;`

**Syntax:**
```omscript
freeze x;
```

**Semantics:**
1. Mark variable `x` as permanently immutable.
2. Set `frozen = true` in `VarBorrowState`.
3. All subsequent loads from `x` are annotated with `!invariant` metadata (LLVM optimization hint).
4. Any write to `x` is a compile-time error.

**Example:**
```omscript
var a = 42;
freeze a;
println(a);  // 42
// a = 99;   // ERROR: cannot write to frozen variable
```

---

### 17.6 `reborrow` semantics

**Syntax:**
```omscript
reborrow var ref = source;
```

**Semantics:** Create a new immutable borrow from an existing borrow (chain borrows).

**Example:**
```omscript
var a = 42;
borrow var r1 = a;
reborrow var r2 = r1;
println(r2);  // 42
```

---

### 17.7 `prefetch` (variable-level vs use-site `@prefetch`)

**Variable-level:** `prefetch x;`  
**Semantics:** Emit `llvm.prefetch` intrinsic to load the address of `x` into cache (hint to hardware).

**Use-site annotation:** `@prefetch` on a loop annotation (see section 8.12 in Part 1).

---

### 17.8 No-aliasing guarantee

**Noalias metadata:** When ownership analysis proves that a pointer does NOT alias any other pointer, LLVM `noalias` metadata is attached to loads/stores and function arguments.

**Benefits:**
- Enables aggressive load/store reordering.
- Improves vectorization.
- Allows the optimizer to assume no memory dependences.

**Emission rules:**
- `Owned` variables: always noalias (no other aliases exist).
- `Borrowed` variables: noalias with respect to other borrows (rust-style borrow rules).
- `MutBorrowed` variables: exclusive access (noalias with all other pointers).

---

### 17.9 Pointer types (`ptr`, `ptr<T>`)

#### Element-type tracking

**`ptrElemTypes_` map:** Tracks the element type of each pointer variable:
```cpp
std::unordered_map<std::string, llvm::Type*> ptrElemTypes_;
```

**Element-type inference:** From `&x` based on `varTypeAnnotations_`:
- If `x: T`, then `&x` has element type `T`.
- If `x` has no annotation, element type is `i64` (default).

**Untyped `ptr` sentinel:** When no element type is known, the pointer is opaque (`ptr` in source, `i8*` in LLVM IR).

#### Heap vs stack origin tracking

**Heap pointers:** Allocated via `malloc`, `calloc`, `realloc`, or `alloc<T>(N)` (when N is dynamic or too large).

**Stack pointers:** Allocated via `alloca` in the function entry block (when escape analysis proves safe).

**Tracking:** `heapPtrs_` set stores names of heap-allocated pointers (enables automatic `free()` on `invalidate`).

#### Stack-allocated `alloc<T>` with backing alloca

**Smart allocator:** `alloc<T>(count)` decides at compile time:
- If `count` is constant AND `count*sizeof(T) <= 4096`: emit `alloca` in entry block.
- Otherwise: emit `malloc` call.

**Lifetime.end emission:** Stack-allocated pointers have `llvm.lifetime.end` intrinsic emitted at scope exit (optimization hint).

#### Allowed initializers for `ptr`

**Validation rule** (from `parser.cpp` around line 1530):
- `ptr` variables MUST be initialized with one of:
  - `&variable` (address-of)
  - `malloc(...)`, `calloc(...)`, `realloc(...)`, `alloc<T>(...)`
  - Another `ptr` variable
  - A function call that returns `ptr`

**Disallowed:**
```omscript
var p: ptr = 42;  // ERROR: invalid initializer for ptr
```

**Allowed:**
```omscript
var x = 42;
var p: ptr = &x;  // OK
var q: ptr = malloc(100);  // OK
```

---

### 17.10 Scope-based drop (RAII-like)

**Not implemented.** OmScript does NOT automatically call `invalidate` at scope exit. Heap allocations persist until explicitly invalidated or the program terminates.

**Future feature:** Automatic drop at scope exit for owned variables.

---

### 17.11 Allowed/disallowed conversions

**`ptr` → `int`:**
- Allowed implicitly (pointer values are stored as i64).
- Use case: Pass pointer across function boundaries.

**`int` → `ptr`:**
- Allowed via explicit cast: `IntToPtr`.
- Use case: Reconstruct pointer from i64 value.

**`ptr<T>` → `ptr<U>`:**
- Allowed (all pointers are i8* in LLVM IR).
- No type safety enforcement at runtime.

---

## 18. OPTMAX

### 18.1 `OPTMAX=:` ... `OPTMAX!:` block syntax

**Syntax:**
```omscript
OPTMAX=:
fn function_name(param: type, ...) {
    // function body
}
OPTMAX!:
```

**Semantics:**
- Declare a function inside an OPTMAX block.
- The function and all code inside it are subject to aggressive optimization and strict restrictions.

**Example:**
```omscript
OPTMAX=:
fn sum_range(n: int) -> int {
    var total: int = 0;
    for (i: int in 0...n) {
        total = total + i;
    }
    return total;
}
OPTMAX!:
```

---

### 18.2 What is forbidden inside an OPTMAX block

**Restrictions** (enforced by parser):
1. **Type annotations required:** All parameters and variables MUST have explicit type annotations.
2. **No nested OPTMAX blocks:** Cannot nest `OPTMAX=:` inside another OPTMAX block.
3. **No I/O:** Cannot call `print`, `println`, `input`, etc.
4. **No side effects:** No global variable mutations, no file I/O.
5. **Deterministic only:** All operations must be pure and deterministic.

**Example of forbidden code:**
```omscript
OPTMAX=:
fn bad() {
    var x = 42;  // ERROR: missing type annotation
    println(x);  // ERROR: I/O not allowed in OPTMAX
}
OPTMAX!:
```

---

### 18.3 `@optmax(level=N, ...)` function annotation

**Syntax:**
```omscript
@optmax(level=3, inline=true)
fn function_name(...) {
    // body
}
```

**Parameters:**
- `level`: Optimization level (0-3). Higher = more aggressive.
- `inline`: Force inlining (true/false).
- `unroll`: Force loop unrolling (true/false).

**Effect:** Apply OPTMAX-level optimizations to a specific function without requiring an OPTMAX block.

---

### 18.4 OPTMAX synchronize behavior on parse errors

**Error recovery:** If a parse error occurs inside an OPTMAX block, the parser automatically emits `OPTMAX!:` to close the block, preventing "unterminated OPTMAX block" cascading errors.

**Implementation** (from `parser.cpp` line 519):
```cpp
if (inOptMaxFunction) {
    errors_.push_back("Parse error: Unterminated OPTMAX block");
}
```

---

### 18.5 Loop variables and OPTMAX restrictions

**Rule:** All loop variables inside OPTMAX blocks MUST have explicit type annotations.

**Example:**
```omscript
OPTMAX=:
fn sum_array(a: array) -> int {
    var total: int = 0;
    for (i: int in 0...len(a)) {  // Type annotation required
        total = total + a[i];
    }
    return total;
}
OPTMAX!:
```

---

### 18.6 Code-gen / optimization implications

**LLVM IR annotations:**
- Functions in OPTMAX blocks are marked with `optnone` removed (always optimized).
- Loop metadata includes `llvm.loop.unroll.enable`, `llvm.loop.vectorize.enable`.

**Compile-time reasoning:**
- OPTMAX functions are candidates for CF-CTRE (compile-time reasoning and evaluation).
- Loops with known bounds may be unrolled entirely.

---

## 19. Built-in Functions

### 19.1 I/O

#### `print(any) → i64`

Print value followed by newline to stdout. Returns 0.

**Example:**
```omscript
print(42);       // "42\n"
print("hello");  // "hello\n"
```

---

#### `println(any) → i64`

Alias for `print`.

---

#### `print_char(i64) → i64`

Print ASCII character by code to stdout (no newline). Returns the argument.

**Example:**
```omscript
print_char(65);  // "A"
```

---

#### `write(any) → i64`

Print value WITHOUT newline to stdout. Returns 0.

**Example:**
```omscript
write("hello");
write(" world");  // "hello world"
```

---

#### `input() → i64`

Read a signed integer from stdin.

**Example:**
```omscript
var x = input();
println(x);
```

---

#### `input_line() → string`

Read a line from stdin (up to 1024 characters), trimming the trailing newline. Returns heap-allocated string.

**Example:**
```omscript
var line = input_line();
println(line);
```

---

### 19.2 Math

See section 11.6 from the research summary. Key functions:

#### `abs(numeric) → same`

Absolute value. Uses `llvm.abs.i64` (int) or `llvm.fabs.f64` (float).

---

#### `min(a, b) → common`

Minimum of two values. Uses `llvm.smin` (int) or `llvm.minnum` (float).

---

#### `max(a, b) → common`

Maximum of two values. Uses `llvm.smax` (int) or `llvm.maxnum` (float).

---

#### `sign(numeric) → i64`

Return -1, 0, or 1 based on sign. Range metadata `[-1, 2)`.

---

#### `clamp(val, lo, hi) → common`

Clamp `val` to `[lo, hi]`: `max(lo, min(val, hi))`.

---

#### `pow(base, exp) → numeric`

Integer exponentiation (binary method O(log exp)) or float `llvm.pow`.

---

#### `sqrt(numeric) → f64`

Square root via `llvm.sqrt`.

---

#### `cbrt(numeric) → f64`

Cube root via libm `cbrt`.

---

#### `floor(numeric) → f64`

Round down.

---

#### `ceil(numeric) → f64`

Round up.

---

#### `round(numeric) → f64`

Round to nearest integer (ties to even).

---

#### Trigonometric functions

`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2` — all return f64.

---

#### `exp(numeric) → f64`

Exponential (e^x).

---

#### `exp2(numeric) → f64`

2^x.

---

#### `log(numeric) → f64`

Natural log.

---

#### `log2(numeric) → i64 | f64`

Base-2 logarithm. Behaviour depends on the **argument type**:

- **Integer argument** → returns `i64` (the floor of `log2(n)`). Implemented as `63 - clz(n)` via the `llvm.ctlz.i64` intrinsic, so it lowers to a single `BSR`/`LZCNT` on x86. Returns `-1` if `n ≤ 0`.
- **Float argument** → returns `f64` from `llvm.log2.f64` (only used when the argument is statically a float in a comptime-folding context; the runtime path is integer-only).

For a true floating-point base-2 log on integer inputs, cast the input first: `log2(f64(n))`.

---

#### `log10(numeric) → f64`

Base-10 log.

---

#### `gcd(i64, i64) → i64`

Greatest common divisor (Euclidean or binary GCD).

---

#### `lcm(i64, i64) → i64`

Least common multiple: `|a * b| / gcd(a, b)`.

---

#### `hypot(a, b) → f64`

Hypotenuse: sqrt(a^2 + b^2), avoiding overflow.

---

#### `fma(a, b, c) → f64`

Fused multiply-add: (a * b) + c with single rounding.

---

#### `copysign(x, y) → f64`

x with sign of y.

---

#### `is_even(i64) → bool`

Return 1 if even, 0 if odd.

---

#### `is_odd(i64) → bool`

Return 1 if odd, 0 if even.

---

#### `is_power_of_2(i64) → bool`

Return 1 if x is a power of 2, 0 otherwise.

---

#### `fast_sqrt(numeric) → f64`

Fast-math square root. Lowers to `llvm.sqrt.f64` with the `afn` (approximate-function) fast-math flag set, allowing the backend to substitute a reciprocal-sqrt-and-multiply or a hardware approximation. Less accurate than `sqrt` but typically 2-3× faster on x86-64. Use only when ULP-level accuracy is not required.

---

#### `is_nan(numeric) → bool`

Return 1 if the argument is an IEEE-754 NaN, 0 otherwise. The argument is interpreted as `f64`. Implemented as `x != x` — works on signaling and quiet NaNs alike.

---

#### `is_inf(numeric) → bool`

Return 1 if the argument is `+∞` or `-∞`, 0 otherwise. The argument is interpreted as `f64`. Implemented via `llvm.fabs.f64` followed by an equality test against `0x7FF0000000000000`.

---

#### `min_float(a, b) → f64` / `max_float(a, b) → f64`

Float-specific minimum/maximum. Unlike `min` / `max` (which dispatch on argument type), these always treat both arguments as `f64` and use `llvm.minnum.f64` / `llvm.maxnum.f64` — IEEE-754 minNum/maxNum semantics, which propagate non-NaN over NaN. Use these when you have mixed `int`/`float` arguments and want float-domain comparison without the implicit-promotion ambiguity of generic `min`/`max`.

---

### 19.3 Arithmetic with explicit overflow/precision mode

#### `fast_add(a, b) → numeric`

Addition with fast-math flags (reassociate, nsw, nuw).

---

#### `fast_sub(a, b) → numeric`

Subtraction with fast-math flags.

---

#### `fast_mul(a, b) → numeric`

Multiplication with fast-math flags.

---

#### `fast_div(a, b) → numeric`

Division with fast-math flags (no NaN checks).

---

#### `precise_add(a, b) → numeric`

Addition with full IEEE 754 semantics (no fast-math).

---

#### `precise_sub(a, b) → numeric`

Subtraction with full IEEE 754 semantics.

---

#### `precise_mul(a, b) → numeric`

Multiplication with full IEEE 754 semantics.

---

#### `precise_div(a, b) → numeric`

Division with full IEEE 754 semantics.

---

### 19.4 Bit manipulation

See section 11.6 from research summary. Key functions:

#### `popcount(i64) → i64`

Count set bits. Returns [0, 64].

---

#### `clz(i64) → i64`

Count leading zeros. Returns [0, 64]. `clz(0) = 64`.

---

#### `ctz(i64) → i64`

Count trailing zeros. Returns [0, 64]. `ctz(0) = 64`.

---

#### `bitreverse(i64) → i64`

Reverse bit order.

---

#### `bswap(i64) → i64`

Byte swap (endianness conversion).

---

#### `rotate_left(val, amt) → i64`

Circular left shift.

---

#### `rotate_right(val, amt) → i64`

Circular right shift.

---

#### `saturating_add(a, b) → i64`

Add with INT64_MAX/MIN clamping.

---

#### `saturating_sub(a, b) → i64`

Subtract with INT64_MAX/MIN clamping.

---

### 19.5 Type utilities

#### `typeof(any) → string`

Return a string representing the type: `"int"`, `"float"`, `"string"`, `"array"`, `"dict"`, etc.

**Example:**
```omscript
println(typeof(42));      // "int"
println(typeof(3.14));    // "float"
println(typeof("hello")); // "string"
```

---

#### `sizeof(type_name) → i64`

Return the byte size of a type as a compile-time constant.

**Example:**
```omscript
println(sizeof(int));    // 8
println(sizeof(float));  // 8
```

---

### 19.5.1 Integer type-cast functions

**Functions:** `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `int`, `uint`, `bool`

**Semantics:** Convert value to the specified integer width/signedness.

**Implementation:** LLVM `trunc`, `sext`, `zext` instructions.

**Example:**
```omscript
var x = 257;
var y = u8(x);  // 1 (257 % 256)
```

**Deep-dive table:** Deferred to Part 3.

---

### 19.6 Time / system

#### `time() → i64`

Return current Unix timestamp (seconds since epoch).

**Example:**
```omscript
var t = time();
println(t);
```

---

#### `sleep(i64) → i64`

Sleep for N seconds. Returns 0.

**Example:**
```omscript
sleep(2);  // Sleep for 2 seconds
```

---

### 19.6.1 Shell / process

#### `command(string) → i64` (alias: `shell`)

Execute shell command via `system()`. Returns exit code.

**Example:**
```omscript
var rc = command("ls -la");
println(rc);  // 0 on success
```

---

#### `sudo_command(string, string) → i64`

Execute command with sudo, providing password as second argument.

**Example:**
```omscript
var rc = sudo_command("apt update", "password");
```

---

#### `exit(i64?) → void` / `exit_program(i64?) → void`

Terminate the program with the given exit code. The argument is **optional** — `exit()` with no argument exits with status `0`. `exit_program` is a synonym for `exit`. The exit code is truncated to `i32` before being passed to the platform `exit(3)` syscall, so the meaningful range is `0..=255` on POSIX.

After the call the compiler emits an LLVM `unreachable` and starts a fresh dead block — code following `exit()` is never executed but is still type-checked.

**Example:**
```omscript
exit();      // status 0
exit(0);     // status 0 (explicit)
exit(2);     // status 2 (POSIX "misuse of shell builtins" — your choice)
```

---

#### `env_get(string) → string`

Get environment variable value.

**Example:**
```omscript
var home = env_get("HOME");
println(home);
```

---

#### `env_set(string, string) → i64`

Set environment variable. Returns 0 on success.

**Example:**
```omscript
env_set("MY_VAR", "value");
```

---

### 19.7 Optimizer hints

#### `assume(bool) → void`

Tell the optimizer to assume the condition is true. Generates `llvm.assume` intrinsic.

**Example:**
```omscript
assume(x > 0);  // Optimizer can use this fact
```

---

#### `unreachable() → void`

Mark code path as unreachable. Generates `llvm.trap` if reached.

**Example:**
```omscript
if (x < 0) {
    unreachable();  // Should never happen
}
```

---

#### `expect(i64, i64) → i64`

Provide branch prediction hint: `expect(value, expected_value)`. Returns `value`.

**Example:**
```omscript
if (expect(x == 0, 1)) {  // Expect x == 0 to be true
    // Likely path
}
```

---

### 19.8 String formatting

See section 12.8.

---

### 19.9 Environment

See section 19.6.1.

---

### 19.10 Array interleave

See `array_zip` in section 11.6.

---

### 19.11 High-precision integer arithmetic

#### `mulhi(i64, i64) → i64`

Return the high 64 bits of the signed 128-bit product.

**Example:**
```omscript
var hi = mulhi(1000000000000, 1000000000000);
```

---

#### `mulhi_u(i64, i64) → i64`

Return the high 64 bits of the unsigned 128-bit product.

---

#### `absdiff(i64, i64) → i64`

Overflow-safe absolute difference: |a - b| using 128-bit intermediate.

---

### 19.12 Arbitrary-precision integers (bigint) — full API

#### `bigint(string) → bigint`

Create bigint from string (base-10).

**Example:**
```omscript
var big = bigint("123456789012345678901234567890");
```

---

#### `bigint_add(bigint, bigint) → bigint`

Add two bigints.

---

#### `bigint_sub(bigint, bigint) → bigint`

Subtract two bigints.

---

#### `bigint_mul(bigint, bigint) → bigint`

Multiply two bigints.

---

#### `bigint_div(bigint, bigint) → bigint`

Divide two bigints (floor division).

---

#### `bigint_mod(bigint, bigint) → bigint`

Modulo operation.

---

#### `bigint_neg(bigint) → bigint`

Negate bigint.

---

#### `bigint_abs(bigint) → bigint`

Absolute value.

---

#### `bigint_pow(bigint, i64) → bigint`

Exponentiation (base^exp).

---

#### `bigint_gcd(bigint, bigint) → bigint`

GCD of two bigints.

---

#### `bigint_eq(bigint, bigint) → bool`

Equality comparison.

---

#### `bigint_lt(bigint, bigint) → bool`

Less-than comparison.

---

#### `bigint_le(bigint, bigint) → bool`

Less-than-or-equal comparison.

---

#### `bigint_gt(bigint, bigint) → bool`

Greater-than comparison.

---

#### `bigint_ge(bigint, bigint) → bool`

Greater-than-or-equal comparison.

---

#### `bigint_cmp(bigint, bigint) → i64`

Three-way comparison: return -1, 0, or 1.

---

#### `bigint_tostring(bigint) → string`

Convert bigint to base-10 string.

---

#### `bigint_to_i64(bigint) → i64`

Convert bigint to i64 (truncates if too large).

---

#### `bigint_bit_length(bigint) → i64`

Return the number of bits in the bigint.

---

#### `bigint_is_zero(bigint) → bool`

Return 1 if bigint is zero, 0 otherwise.

---

#### `bigint_is_negative(bigint) → bool`

Return 1 if bigint is negative, 0 otherwise.

---

#### `bigint_shl(bigint, i64) → bigint`

Left shift (multiply by 2^n).

---

#### `bigint_shr(bigint, i64) → bigint`

Right shift (divide by 2^n, floor).

---

### 19.13 The `std::` namespace

**Built-in namespace:** Every standard library function is accessible as `std::name` without any import statement.

**Dispatch rules:**
- `std::abs(x)` resolves to the same function as `abs(x)`.
- No difference in semantics; purely a namespace prefix for clarity.

**List of std:: symbols:**

See parser registration in `parser.cpp` lines 52-100. Key entries:
- Math: `std::abs`, `std::min`, `std::max`, `std::pow`, `std::sqrt`, etc.
- Trig: `std::sin`, `std::cos`, `std::tan`, `std::asin`, etc.
- Bit ops: `std::popcount`, `std::clz`, `std::ctz`, `std::bitreverse`, etc.
- Type casts: `std::to_int`, `std::to_float`, `std::to_string`, etc.
- String: `std::str_len`, `std::str_upper`, `std::str_split`, etc.
- Array: `std::len`, `std::push`, `std::pop`, `std::sort`, etc.
- I/O: `std::print`, `std::println`, `std::input`, etc.

---

### 19.14 Generic collection operations

#### `filter(collection, predicate_fn) → same`

Type-dispatched filter. Examines the static type of `collection`:

- `string` → forwards to `str_filter` (§12.7), returning a new string of characters that pass the predicate.
- otherwise → forwards to `array_filter` (§11.6), returning a new array of elements that pass the predicate.

The predicate is given as the **name of an existing function** (string literal — same convention as `array_map` / `thread_create`), not as a lambda value. The function must take one argument and return a truthy value.

**Example:**
```omscript
fn keep_even(x: int) -> int { return x % 2 == 0 ? 1 : 0; }

var nums  = [1, 2, 3, 4, 5];
var evens = filter(nums, "keep_even");        // → [2, 4]
var only_az = filter("Hello, World!", "is_alpha"); // string path
```

This is the only generic dispatcher in the collection-builtin family — `map`, `reduce`, `find`, etc. do **not** have a generic form and must be called with their explicit `array_*` / `str_*` / `map_*` name.

---

### 19.15 Matrix operations

OmScript ships a minimal row-major dense-matrix API on top of arrays. Matrices are returned as opaque array-typed values (the codegen tracks them via `arrayReturningFunctions_`), and elements are stored as `i64`. There is no separate `Matrix` type — interoperate with the array API where helpful, but treat the layout as opaque.

#### `mat_new(rows: i64, cols: i64) → array`

Allocate a new `rows × cols` matrix, zero-initialised. Returns an opaque array handle.

#### `mat_fill(rows: i64, cols: i64, value: i64) → array`

Allocate a new `rows × cols` matrix with every element set to `value`.

#### `mat_get(m: array, i: i64, j: i64) → i64`

Return the element at row `i`, column `j`. Bounds-checked at runtime in debug builds.

#### `mat_set(m: array, i: i64, j: i64, value: i64) → i64`

Store `value` at row `i`, column `j`. Returns `value`.

#### `mat_rows(m: array) → i64` / `mat_cols(m: array) → i64`

Return the number of rows / columns of `m`.

#### `mat_mul(a: array, b: array) → array`

Standard dense matrix multiply. Returns a new `mat_rows(a) × mat_cols(b)` matrix. Requires `mat_cols(a) == mat_rows(b)` — mismatched dimensions are a runtime error.

#### `mat_transp(m: array) → array`

Return the transpose of `m` as a new matrix.

**Example:**
```omscript
var a = mat_fill(2, 3, 1);
var b = mat_fill(3, 2, 2);
var c = mat_mul(a, b);          // 2×2, every element = 6
println(mat_get(c, 0, 0));      // 6
```

---

### 19.16 Region allocation and raw memory

Low-level escape hatches for hand-managing memory. Use these when the ownership system (§17) and built-in arrays / strings / dicts cannot express a needed allocation pattern. They are unsafe by design — there are no bounds checks on raw pointers and no use-after-free detection across regions.

#### `newRegion() → i64`

Create a fresh memory region (an arena). Returns an opaque region handle. Allocations inside the region (`alloc`) live until the region is freed by leaving its lexical scope (region cleanup is wired into the function epilogue).

#### `alloc(region: i64, size: i64) → ptr`

Allocate `size` bytes inside `region`. Returns a `ptr` to the allocation. The pointer is valid until the region is destroyed.

#### `malloc(size: i64) → ptr`

Heap-allocate `size` bytes via the platform `malloc(3)`. The caller is responsible for freeing the result with `free`.

#### `free(p: ptr) → i64`

Release a pointer previously returned by `malloc`. **Do not** call `free` on pointers obtained via `alloc(region, …)` — those are owned by the region.

**Choosing between region, malloc, and managed types:**

| Need | Recommended API |
|------|-----------------|
| String / array / dict / struct | The managed type (no manual free; see §17) |
| Many short-lived allocations with a clear lifetime | `newRegion` + `alloc` (bulk free at scope exit) |
| Long-lived, irregular lifetime | `malloc` + `free` |
| Inter-op with C ABI requiring a raw buffer | `malloc` |

---

## 20. Concurrency

OmScript's concurrency model is a thin layer over the host's POSIX threading primitives (`pthread_*`). All thread and mutex handles are passed around as plain `i64` values that wrap the underlying `pthread_t` / `pthread_mutex_t*`. There is no managed thread pool, no async runtime, no green threads, and no garbage-collection-aware safe-point machinery — threads run to completion, and the user is responsible for joining them and freeing mutexes.

**Model summary:**

| Concept | Backing primitive | Handle type |
| --- | --- | --- |
| Thread | `pthread_create` / `pthread_join` | `i64` (pthread_t) |
| Mutex | `pthread_mutex_*` | `i64` (`pthread_mutex_t*` cast to int) |
| Parallel loop | `@parallel for` → loop-parallelism metadata | n/a |
| Atomics | — | not yet implemented |

### 20.1 Threads

#### 20.1.1 `thread_create(fn_name: string) → i64`

**Description:** Spawn a new OS thread that calls the OmScript function named by `fn_name`. The argument **must be a string literal** containing the name of an existing top-level function — it is resolved at compile time, not at runtime, and a non-literal or unknown name is a compile-time error.

The spawned thread executes `fn_name()` with **no arguments**; the target function's return value is discarded. To pass data into the thread, use module-level (`global var`) state guarded by a mutex.

**Returns:** A thread handle (`pthread_t` reinterpreted as `i64`). Pass this value to `thread_join`.

**Errors:**
- *Compile-time:* `thread_create requires a string literal function name` if the argument is not a string literal.
- *Compile-time:* `thread_create: unknown function 'foo'` if the named function does not exist in the current translation unit.

**Example:**
```omscript
global var counter: int = 0;
global var lock: int = 0;  // initialized in main()

fn worker() {
    mutex_lock(lock);
    counter = counter + 1;
    mutex_unlock(lock);
}

fn main() {
    lock = mutex_new();
    var t1 = thread_create("worker");
    var t2 = thread_create("worker");
    thread_join(t1);
    thread_join(t2);
    mutex_destroy(lock);
    println(counter);   // 2
    return 0;
}
```

---

#### 20.1.2 `thread_join(tid: i64) → i64`

**Description:** Block the calling thread until the thread identified by `tid` has terminated. Internally calls `pthread_join(tid, NULL)` — the joined thread's return value is **discarded**, not returned to OmScript.

**Returns:** Always `0`. (The return value exists only so the call can be used as an expression.)

**Errors:** Passing an invalid or already-joined `tid` invokes platform-defined behaviour from `pthread_join` (typically returns `EINVAL` or `ESRCH`); OmScript does not surface this.

**Example:**
```omscript
var t = thread_create("worker");
thread_join(t);
```

---

### 20.2 Mutexes

Mutexes are heap-allocated `pthread_mutex_t` structures, returned as opaque `i64` handles. The allocation is sized at **64 bytes** — enough to cover `pthread_mutex_t` on every supported platform (40 bytes on Linux x86_64, 64 bytes on macOS) — and initialized via `pthread_mutex_init` with default attributes. Default-attribute mutexes are **non-recursive**: a thread that locks the same mutex twice without unlocking deadlocks.

> **Note:** OmScript currently exposes only the four operations below. There is **no `mutex_try_lock`**, no read/write lock, and no condition variables.

#### 20.2.1 `mutex_new() → i64`

**Description:** Allocate a new `pthread_mutex_t`, initialize it with default attributes (`pthread_mutex_init(m, NULL)`), and return its address as an `i64`.

**Returns:** A mutex handle. Always non-zero on success; allocation failure is not surfaced (the call may abort if `malloc` fails).

**Lifetime:** The caller owns the mutex and must call `mutex_destroy` to release the underlying memory.

**Example:**
```omscript
var m = mutex_new();
```

---

#### 20.2.2 `mutex_lock(m: i64) → i64`

**Description:** Acquire the mutex `m`, blocking the calling thread until it becomes available. Wraps `pthread_mutex_lock`.

**Returns:** Always `0`.

**Errors:** Locking a mutex that the current thread already holds (with default, non-recursive attributes) deadlocks.

**Example:**
```omscript
mutex_lock(m);
// critical section
mutex_unlock(m);
```

---

#### 20.2.3 `mutex_unlock(m: i64) → i64`

**Description:** Release the mutex `m`. Wraps `pthread_mutex_unlock`. The current thread must own the mutex; unlocking a mutex held by another thread is undefined behaviour at the pthread level.

**Returns:** Always `0`.

---

#### 20.2.4 `mutex_destroy(m: i64) → i64`

**Description:** Destroy the mutex via `pthread_mutex_destroy` and free its backing allocation. After this call the handle `m` is invalid and **must not be used**.

**Returns:** Always `0`.

**Errors:** Destroying a locked mutex is undefined behaviour at the pthread level.

---

### 20.3 Atomics

**Status:** Not implemented in the current code generator. There are no atomic load / store / RMW built-ins or atomic types. Use `mutex_lock` / `mutex_unlock` to protect shared state.

---

### 20.4 Memory model

OmScript does **not** expose explicit memory ordering controls (no equivalent of C++ `memory_order_*`). The effective guarantees come from the LLVM defaults plus the pthread primitives:

- Plain loads and stores compile to LLVM unordered memory operations — they are **not** sequentially consistent across threads, and the compiler is permitted to reorder them.
- `mutex_lock` and `mutex_unlock` provide acquire / release semantics respectively (inherited from `pthread_mutex_*`), which is sufficient to publish writes made inside a critical section to the next thread that locks the same mutex.
- All shared mutable state must be protected by a mutex; relying on plain reads/writes from multiple threads is a data race and is undefined behaviour.

---

### 20.5 `parallel for`

**Syntax:** (full grammar in §8.12)
```omscript
@parallel
for (i in 0...n) {
    // body
}
```

**Semantics:** Annotates the loop with LLVM loop-parallelism metadata (`llvm.loop.parallel_accesses` and friends), enabling auto-vectorization and parallel-execution back-ends. The loop body is **not** automatically dispatched to worker threads by the OmScript runtime; parallel execution depends on the LLVM optimizer recognizing the form and on target-specific lowering.

**Restrictions:**
- Iterations must be independent — no loop-carried data dependencies.
- Body must not rely on a particular execution order or have observable side effects between iterations (I/O, mutex operations, etc.).
- Inside `OPTMAX` blocks the body must additionally satisfy OPTMAX restrictions (§18).

---

## 21. File I/O

OmScript exposes a small set of synchronous, **whole-file** I/O built-ins backed by the C runtime (`fopen` / `fread` / `fwrite` / `fclose`) and POSIX `access`. There is no streaming, seeking, or file-handle abstraction at the language level — each call opens, performs its operation, and closes the file in one step.

**I/O model:**
- Files are opened in **binary mode** (`"rb"`, `"wb"`, `"a"`).
- Paths are interpreted by the underlying C library (relative paths resolve against the process's current working directory).
- Errors do **not** throw or abort — every built-in returns a sentinel value (empty string, or non-zero return code) so callers can branch explicitly.
- There is no implicit buffering or text-mode newline translation.

### 21.1 Summary

| Built-in | Signature | On success | On failure |
| --- | --- | --- | --- |
| `file_read` | `(path: string) → string` | File contents | Empty string `""` |
| `file_write` | `(path: string, content: string) → i64` | `0` | `1` |
| `file_append` | `(path: string, content: string) → i64` | `0` | `1` |
| `file_exists` | `(path: string) → bool` | `1` if exists | `0` |

---

### 21.2 `file_read(path: string) → string`

**Description:** Read the entire contents of `path` into a newly-allocated, null-terminated string. The file is opened in binary mode, so no newline translation occurs.

**Returns:** The file contents as a `string`. Returns the empty string `""` if the file cannot be opened or its size cannot be determined (e.g., non-seekable stream).

**Errors:** Never throws. Failure is reported as an empty string — distinguish from a legitimately empty file using `file_exists` first if needed.

**Example:**
```omscript
var content = file_read("input.txt");
println(content);
```

---

### 21.3 `file_write(path: string, content: string) → i64`

**Description:** Write `content` to `path`, **truncating** any existing file. Opens in binary mode (`"wb"`) and writes exactly `strlen(content)` bytes.

**Returns:** `0` on success, `1` if the file could not be opened for writing.

**Example:**
```omscript
var rc = file_write("output.txt", "hello world");
if (rc != 0) {
    println("write failed");
}
```

---

### 21.4 `file_append(path: string, content: string) → i64`

**Description:** Append `content` to the end of `path`. Opens in append mode (`"a"`); creates the file if it does not exist.

**Returns:** `0` on success, `1` if the file could not be opened.

**Example:**
```omscript
file_append("log.txt", "Log entry\n");
```

---

### 21.5 `file_exists(path: string) → bool`

**Description:** Test whether `path` exists and is accessible to the current process. Implemented via POSIX `access(path, F_OK)`.

**Returns:** `1` (true) if the path exists, `0` (false) otherwise. Note that a `false` result may also indicate a permission error preventing the existence check.

**Example:**
```omscript
if (file_exists("config.txt")) {
    println("Config found");
}
```

---

## 22. Lambda Expressions

OmScript lambdas are anonymous functions with a lightweight `|params| body` syntax, designed primarily for use with the higher-order array built-ins (`array_map`, `array_filter`, `array_reduce`, `array_any`, `array_every`, `array_count`, `array_find`).

**Implementation model — important:** Lambdas are *not* runtime closures. The parser desugars every lambda into a top-level named function (`__lambda_N`) and replaces the lambda expression with a string literal containing that name. The higher-order built-ins receive that string and resolve it to the generated function at code-gen time. The consequences are:

- Lambdas **cannot capture** variables from the enclosing scope. Reference any non-parameter identifier and you reference a top-level / `global` symbol of the same name, not a local.
- A lambda's runtime *value* is a `string` (the generated function's name), not a callable. You can store it in a variable, but you cannot directly invoke it as `f(x)` from OmScript code — it can only flow into a built-in that knows how to call it by name.
- All lambdas with the same body produce distinct generated functions; there is no deduplication.

### 22.1 Syntax

```ebnf
lambda      ::= '|' [params] '|' expression
params      ::= param { ',' param }
param       ::= identifier [ ':' type ]
```

- The body is **a single expression** (no statement block, no `return` keyword). Whatever the expression evaluates to becomes the function's return value.
- Parameters with no type annotation default to `i64` (this is the only inferred type — there is no full inference at parse time).
- Annotate explicitly when the element type is anything else: `|x:float| x * 2.0`, `|s:string| str_len(s)`.
- Empty parameter list is written as `||`: `var make_zero = || 0;`.

**Examples:**

```omscript
|x| x * 2                    // i64 → i64
|x:float| x * 2.0            // float → float
|x, y| x + y                 // (i64, i64) → i64
|acc, x| acc + x             // for array_reduce
|s:string| str_len(s)        // string → i64
|| 42                        // () → i64
```

---

### 22.2 Captures (not supported)

Lambdas cannot capture local variables. The following does **not** do what it appears to:

```omscript
fn scale(arr, factor) {
    return array_map(arr, |x| x * factor);   // ❌ `factor` is not captured
}
```

Inside the generated `__lambda_N` function, `factor` is an unbound name — it resolves to a top-level / `global var` named `factor` if one exists, or fails at codegen otherwise. Workarounds:

1. **Promote to global state** (and serialize access if multiple threads are involved).
2. **Bake the value into a literal lambda:**
   ```omscript
   fn scale_by_2(arr) {
       return array_map(arr, |x| x * 2);    // ✅ literal 2
   }
   ```
3. **Write a named helper function** and reference it by name (which is what the higher-order built-ins want anyway):
   ```omscript
   fn double(x) { return x * 2; }
   var doubled = array_map(arr, "double");  // pass the name as a string
   ```

---

### 22.3 First-class function values

The built-ins that accept a lambda also accept a **string literal containing a function name**, because that's what a lambda compiles to. Both forms are interchangeable:

```omscript
fn square(n) { return n * n; }

var a = array_map([1, 2, 3], |x| x * x);  // lambda form
var b = array_map([1, 2, 3], "square");   // named-function form (identical effect)
```

Functions cannot be passed as bare identifiers in this position — only string literals or lambdas. This is what allows the parser to resolve the target at compile time and emit a direct call.

---

### 22.4 Higher-order built-in interaction

The following built-ins accept a lambda or a function-name string. See §11.7 for full signatures.

| Built-in | Lambda shape |
| --- | --- |
| `array_map(arr, fn)` | `|x| → T` |
| `array_filter(arr, fn)` | `|x| → bool` |
| `array_reduce(arr, fn, init)` | `|acc, x| → acc` |
| `array_any(arr, fn)` | `|x| → bool` |
| `array_every(arr, fn)` | `|x| → bool` |
| `array_count(arr, fn)` | `|x| → bool` |
| `array_find(arr, fn)` | `|x| → bool`, returns first matching index or `-1` |

---

### 22.5 Pipe-forward and spread interaction

Because a lambda evaluates to a function-name string and **cannot be invoked as a normal call**, it cannot appear directly on the right side of `|>`. This is illegal:

```omscript
5 |> |x| x * 2     // ❌ pipe target must be a callable named function
```

Use a named function, or pipe into a higher-order built-in:

```omscript
fn double(x) { return x * 2; }
var y = 5 |> double;                          // ✅ 10

var doubled = [1, 2, 3] |> array_map(|x| x * 2);  // ✅ — array_map is the pipe target
```

The spread operator `...arr` expands an array as positional arguments to a *function call*, independently of any lambdas:

```omscript
var a = [1, 2, 3];
var s = sum(...a);   // sum(1, 2, 3) — `sum` is a built-in, not a lambda
```

---

## 23. Import / Module System

### 23.1 `import path/to/module;` syntax

**Syntax:**
```omscript
import "path/to/module";
```

**File resolution:**
1. Resolve path relative to the current file's directory.
2. Append `.om` extension if not present.
3. Parse the imported file and merge its AST into the current program.

**Example:**
```omscript
import "utils";          // imports utils.om
import "lib/math";       // imports lib/math.om
```

---

### 23.2 `import module as alias;`

**Syntax:**
```omscript
import "module" as alias;
```

**Effect:** All symbols from `module` are prefixed with `alias::`.

**Example:**
```omscript
import "math" as m;
var x = m::sqrt(4);
```

---

### 23.3 Symbol visibility

**Public by default:** All top-level functions, structs, enums, and globals are visible to importing modules.

**No `pub` keyword:** OmScript does NOT have explicit visibility modifiers. Everything is public.

---

### 23.4 Circular-import handling

**Cycle detection:** The parser tracks imported files in `importedFiles_` set (shared across imports). If a file is imported twice, the second import is skipped.

**Example:**
```omscript
// a.om
import "b";

// b.om
import "a";  // Skipped (cycle detected)
```

---

### 23.5 Module organization

**Example structure:**
```
myproject/
  main.om
  lib/
    math.om
    string.om
  utils.om
```

**main.om:**
```omscript
import "lib/math";
import "utils";

fn main() {
    var x = math::sqrt(16);
    var s = utils::reverse("hello");
    println(x);
    println(s);
    return 0;
}
```

---

### 23.6 Re-exports

**Not explicitly supported.** Imported symbols are NOT automatically re-exported. To expose a symbol from an imported module, define a wrapper function:

```omscript
// wrapper.om
import "math";

fn my_sqrt(x) {
    return math::sqrt(x);
}
```

---

### 23.7 The `std::` namespace structure

**Flat namespace:** All standard library functions live in the top-level `std::` namespace (no sub-namespaces like `std::math::` or `std::string::`).

**Example:**
```omscript
std::abs(x);
std::str_upper(s);
std::array_map(a, f);
```

**Alias-free:** `std::name` always resolves to the same function as `name` (no aliasing or shadowing).

---

**End of Part 2**

## 24. Compiler CLI Reference

### 24.1 Invocation forms

The OmScript compiler (`omsc`) supports three primary invocation patterns:

1. **Subcommand mode** (recommended for projects):
   ```bash
   omsc <subcommand> [flags] [args]
   ```
   Used with project management commands (`build`, `run`, `init`, etc.).

2. **Single-file mode**:
   ```bash
   omsc [subcommand] file.om [flags] [-o output]
   ```
   Compiles a standalone `.om` source file directly.

3. **Legacy direct mode**:
   ```bash
   omsc file.om [flags]
   ```
   Equivalent to `omsc compile file.om`; provided for backwards compatibility.

### 24.2 Subcommands

| Subcommand           | Aliases                     | Description                                             |
|----------------------|-----------------------------|---------------------------------------------------------|
| `help`               | `-h`, `--help`              | Display usage information                               |
| `version`            | `-v`, `--version`           | Print compiler version (`4.1.1`)                        |
| `build`, `compile`   | `--build`, `--compile`      | Compile source to executable (default subcommand)       |
| `run`                | `-r`, `--run`               | Compile and execute, passing args after `--`            |
| `check`              | `--check`                   | Validate syntax and types without code generation       |
| `lex`, `tokens`      | `-l`, `--lex`, `--dump-tokens` | Dump lexer tokens and exit                         |
| `parse`, `ast`       | `--parse`, `--ast`, `--emit-ast` | Dump parsed AST and exit                          |
| `emit-ir`            | `-e`, `-i`, `--emit-ir`, `--ir` | Emit LLVM IR and exit                              |
| `clean`              | `-C`, `--clean`             | Remove build artifacts (object files, executables)      |
| `init`               | `--init`                    | Initialize a new OmScript project with `oms.toml`       |
| `install`, `update`  | `--install`, `--update`     | Install or update the compiler from GitHub releases     |
| `uninstall`          | `--uninstall`               | Uninstall the compiler                                  |
| `pkg`, `package`     | `--pkg`                     | Package manager subcommand (see §24.7)                  |

**Usage examples**:
```bash
omsc build main.om                   # compile to ./main (or main.exe on Windows)
omsc run main.om -- arg1 arg2        # compile + run with arguments
omsc build --release -o bin/myapp    # release build, custom output path
omsc check lib/*.om                  # validate multiple files
omsc emit-ir main.om                 # dump LLVM IR to stdout
```

### 24.3 Flags

Flags are organized by category. Most boolean flags support negation via `-fno-<feature>` (e.g., `-fno-lto` disables LTO).

#### Output Control

| Flag              | Short | Default     | Description                                          |
|-------------------|-------|-------------|------------------------------------------------------|
| `-o <path>`       | —     | `./a.out`   | Output file path                                     |
| `--emit-obj`      | —     | `false`     | Emit object file (`.o`) only, skip linking           |
| `--dry-run`       | —     | `false`     | Validate and codegen, but don't write files          |
| `-V`, `--verbose` | `-V`  | `false`     | Print LLVM IR, pass timings, diagnostic details      |
| `-q`, `--quiet`   | `-q`  | `false`     | Suppress non-error output                            |
| `--time`          | —     | `false`     | Show compilation phase timing breakdown              |
| `--dump-ast`      | —     | `false`     | Dump parsed AST to stdout before codegen             |

#### Optimization

| Flag                   | Short | Default         | Description                                             |
|------------------------|-------|-----------------|---------------------------------------------------------|
| `-O0`                  | —     | —               | No optimization (debug builds)                          |
| `-O1`                  | —     | —               | Basic optimization (-foptmax, loop mustprogress)        |
| `-O2`                  | —     | (profile default) | Standard optimization (enables e-graph, superopt)     |
| `-O3`                  | —     | —               | Aggressive optimization (all passes, high unroll)       |
| `-foptmax`             | —     | `true`          | OPTMAX block-level optimization                         |
| `-fegraph`             | —     | `true` (O2+)    | E-graph equality saturation pass                        |
| `-fsuperopt`           | —     | `true` (O2+)    | Superoptimizer idiom recognition and synthesis          |
| `-fsuperopt-level=N`   | —     | `2`             | Superoptimizer aggressiveness (0–3)                     |
| `-fhgoe`               | —     | `true`          | Hardware Graph Optimization Engine                      |
| `-fvectorize`          | —     | `true`          | SIMD vectorization hints                                |
| `-funroll-loops`       | —     | `true`          | Loop unrolling hints                                    |
| `-floop-optimize`      | —     | `true`          | Polyhedral loop transformations (tiling, interchange)   |
| `-fparallelize`        | —     | `true`          | Auto-parallelization of independent loops               |

#### Target

| Flag              | Short | Default    | Description                                          |
|-------------------|-------|------------|------------------------------------------------------|
| `-march=<cpu>`    | —     | `native`   | Target CPU architecture (e.g., `x86-64-v3`, `znver3`) |
| `-mtune=<cpu>`    | —     | (same as `-march`) | CPU model for scheduling tuning            |
| `-fpic`           | —     | `true`     | Generate position-independent code                   |

#### Features

| Flag              | Short | Default    | Description                                          |
|-------------------|-------|------------|------------------------------------------------------|
| `-flto`           | —     | `false`    | Link-time optimization (whole-program analysis)      |
| `-ffast-math`     | —     | `false`    | Unsafe floating-point optimizations (reassociation, reciprocals, ignore NaN/Inf) |
| `-fstack-protector` | —   | `false`    | Stack canary protection against buffer overflows     |
| `-static`         | —     | `false`    | Static linking (embed runtime, no shared libs)       |

#### Debug

| Flag              | Short | Default    | Description                                          |
|-------------------|-------|------------|------------------------------------------------------|
| `-g`, `--debug`   | `-g`  | `false`    | Emit DWARF debug info for GDB/LLDB                   |
| `-s`, `--strip`   | `-s`  | `false`    | Strip symbols from output binary                     |
| `-k`, `--keep-temps` | `-k` | `false` | Keep temporary files (for `run` command only)        |

#### Diagnostics

The compiler emits structured diagnostics to stderr. Errors use exit code 1; warnings do not halt compilation.

#### PGO (Profile-Guided Optimization)

| Flag              | Short | Default    | Description                                          |
|-------------------|-------|------------|------------------------------------------------------|
| `-fpgo-gen=<path>` | —    | —          | Instrument binary to write profile to `<path>` on exit |
| `-fpgo-use=<path>` | —    | —          | Use profile from `<path>` for guided optimization    |

Profile format: LLVM raw profile (`.profraw`), converted via `llvm-profdata merge`.

#### Miscellaneous

| Flag                | Short | Default | Description                                          |
|---------------------|-------|---------|------------------------------------------------------|
| `--release`         | —     | —       | Shortcut for `--profile release`                     |
| `--profile <name>`  | —     | `debug` | Use named profile from `oms.toml`                    |
| `--profile=<name>`  | —     | —       | Alternate form of `--profile`                        |

### 24.4 Output formats and file extensions

| Mode           | Extension(s)         | Description                                             |
|----------------|----------------------|---------------------------------------------------------|
| Executable     | (none), `.exe`       | Native binary (default on Unix/Windows)                 |
| Object file    | `.o`, `.obj`         | Produced with `--emit-obj`                              |
| LLVM IR        | `.ll`                | Human-readable text (via `emit-ir`)                     |
| LLVM Bitcode   | `.bc`                | Binary IR (when LTO enabled)                            |
| Assembly       | `.s`                 | Target assembly (not directly exposed; use `-S` via `clang`) |

### 24.5 Optimization levels

| Level | Passes Enabled                                                                 |
|-------|--------------------------------------------------------------------------------|
| `-O0` | None. Debug mode: no inlining, no loop transforms, keep all assertions.       |
| `-O1` | OPTMAX, loop mustprogress, basic constant folding, dead-code elimination.     |
| `-O2` | **All O1 + e-graph equality saturation, superoptimizer (level 2), HGOE, polyhedral loop optimizations, vectorization, unrolling, inlining.** Default for most builds. |
| `-O3` | All O2 + aggressive unrolling (factor 8), higher inline threshold, speculative transforms. |

**Default**: `O2` unless overridden by profile or explicit flag.

**Per-level pass schedule** (full detail in §25.3; this is a one-line summary):
- **O0**: AST validation, type checking, codegen — no AST optimizations.
- **O1**: + Purity inference + CF-CTRE + LLVM SimplifyCFG/Mem2Reg/SROA/EarlyCSE.
- **O2**: + std::synthesize expansion, e-graph saturation (`maxNodes = 50,000`, `maxIterations = 30`), abstract interpretation, polyhedral optimizer, full LLVM midend (IPSCCP, GVN, DSE, vectorizers), superoptimizer (level 2: idiom + algebraic + branch→select + synthesis), HGOE (when a hardware profile is available).
- **O3**: All O2 passes with the superoptimizer forced to level 3 (deeper synthesis: `maxInstructions = 5`, `costThreshold = 0.9`) and the LLVM `-O3` preset (which raises LLVM's own unroll/inline thresholds). E-graph and CF-CTRE limits are **not** raised at O3 — they are constants.

### 24.6 Target specification

`-march=<cpu>` and `-mtune=<cpu>` control code generation and scheduling:

- **`native`** (default): Auto-detect host CPU via LLVM `sys::getHostCPUName()`.
- **Generic x86-64 levels**: `x86-64`, `x86-64-v2`, `x86-64-v3`, `x86-64-v4`.
- **Intel**: `skylake`, `cascadelake`, `icelake-server`, `sapphirerapids`, `alderlake`.
- **AMD**: `znver1`, `znver2`, `znver3`, `znver4`.
- **ARM**: `cortex-a72`, `cortex-a76`, `neoverse-n1`, `neoverse-v1`.

**Effect of target**:
- Instruction selection (SSE4.2, AVX2, AVX-512, etc.).
- HGOE latency tables and port mappings (see §26.3).
- Vectorization width (128-bit SSE vs. 256-bit AVX vs. 512-bit AVX-512).

**Example**:
```bash
omsc build -march=znver3 -mtune=znver3 server.om  # AMD Zen 3 tuning
```
### 24.7 Package manager subcommands

The `pkg` subcommand manages dependencies:

```bash
omsc pkg <action> [args]
```

| Action    | Description                                                        |
|-----------|--------------------------------------------------------------------|
| `init`    | Create `oms.toml` in current directory (same as `omsc init`)       |
| `add <name>` | Add dependency to `[dependencies]` section of `oms.toml`        |
| `install` | Fetch and install all dependencies listed in `oms.toml`            |
| `remove <name>` | Remove dependency from `oms.toml`                            |
| `list`    | List installed packages in `om_packages/`                          |
| `build`   | Build the current project (same as `omsc build`)                   |
| `run`     | Build and run (same as `omsc run`)                                 |

**Dependency resolution**:
Dependencies are fetched from the default registry URL (GitHub `user-packages/` directory) or a custom registry set via `OMSC_REGISTRY_URL` environment variable.

**Example workflow**:
```bash
omsc pkg init                  # creates oms.toml
omsc pkg add http              # adds http library
omsc pkg install               # downloads to om_packages/http/
omsc pkg build                 # compiles with dependency
```

### 24.8 Project file format (oms.toml)

`oms.toml` is a minimal-TOML manifest supporting:

#### `[project]` section

| Key            | Type     | Description                                    |
|----------------|----------|------------------------------------------------|
| `name`         | `string` | Project name (used as default output filename) |
| `version`      | `string` | Semantic version (informational)               |
| `authors`      | `string[]` | Author names (informational)                 |
| `license`      | `string` | SPDX license identifier (informational)        |

#### `[dependencies]` section

Map of `<package> = "<version>"` or `<package> = "<git-url>"`.

```toml
[dependencies]
http = "1.0.0"
json = "https://github.com/user/omscript-json.git"
```

#### `[profile.<name>]` section

Define build profiles. Built-in profiles are `debug` and `release`; custom profiles can be added.

| Key              | Type   | Default (debug) | Default (release) | Description                              |
|------------------|--------|-----------------|-------------------|------------------------------------------|
| `opt_level`      | `int`  | `0`             | `3`               | Optimization level (0–3)                 |
| `debug_info`     | `bool` | `true`          | `false`           | Emit DWARF debug info                    |
| `strip`          | `bool` | `false`         | `true`            | Strip symbols                            |
| `lto`            | `bool` | `false`         | `false`           | Link-time optimization                   |
| `fast_math`      | `bool` | `false`         | `false`           | Unsafe FP math                           |
| `optmax`         | `bool` | `true`          | `true`            | OPTMAX optimization                      |
| `egraph`         | `bool` | `false`         | `true`            | E-graph equality saturation              |
| `superopt`       | `bool` | `false`         | `true`            | Superoptimizer pass                      |
| `superopt_level` | `int`  | `0`             | `2`               | Superoptimizer aggressiveness (0–3)      |
| `hgoe`           | `bool` | `false`         | `true`            | Hardware graph optimization              |
| `vectorize`      | `bool` | `false`         | `true`            | SIMD vectorization                       |
| `unroll_loops`   | `bool` | `false`         | `true`            | Loop unrolling                           |
| `loop_optimize`  | `bool` | `false`         | `true`            | Polyhedral loop optimizations            |
| `parallelize`    | `bool` | `false`         | `true`            | Auto-parallelization                     |
| `whole_program`  | `bool` | `false`         | `true`            | Whole-program analysis (unused currently)|
| `stack_protector`| `bool` | `false`         | `false`           | Stack canary protection                  |
| `static_link`    | `bool` | `false`         | `false`           | Static linking                           |

**Example `oms.toml`**:
```toml
[project]
name = "myapp"
version = "0.1.0"

[dependencies]
http = "1.0"

[profile.debug]
opt_level = 0
debug_info = true

[profile.release]
opt_level = 3
strip = true
lto = true

[profile.perf]
opt_level = 3
debug_info = true
strip = false
fast_math = true
```

### 24.9 Environment variables

| Variable             | Description                                                        |
|----------------------|--------------------------------------------------------------------|
| `OMSC_BINARY_PATH`   | Override compiler installation path (for self-update)              |
| `OMSC_REGISTRY_URL`  | Custom package registry URL (default: GitHub user-packages)        |
| `OMSC_DUMP_SCHEDULE` | When set, dump HGOE scheduling decisions to stderr                 |
| `HOME` / `USERPROFILE` | User home directory (for config and cache)                       |

### 24.10 Exit codes

| Code | Meaning                                                              |
|------|----------------------------------------------------------------------|
| `0`  | Success                                                              |
| `1`  | Compilation error (lexer, parser, type checker, or codegen failure)  |
| `2`  | File I/O error (cannot read source, cannot write output)             |
| `3`  | Internal compiler error (assertion failure, LLVM error)              |

---

## 25. Compilation Pipeline (Internal)

### 25.1 Phase ordering

The OmScript compiler processes source code through eleven phases:

1. **Lexical Analysis** (`Lexer`)  
   Tokenize source text into a stream of `Token` objects. Errors: E001 (invalid character), E002 (unterminated string).

2. **Parsing** (`Parser`)  
   Build an abstract syntax tree (`Program` containing `FunctionDecl`, `Statement`, `Expression` nodes). Errors: E003 (unexpected token), E004 (missing semicolon).

3. **Type Pre-Analysis**  
   Infer string and array element types for variables and expressions. Populates `Program::stringVars`, `Program::arrayElementTypes`.

4. **Constant-Return Detection**  
   Identify functions that always return a compile-time constant (used by CF-CTRE).

5. **Purity Inference**  
   Mark functions as `@pure` when they have no side effects and deterministic output. Propagates across call graph.

6. **Effect Inference**  
   Build `FunctionEffects` summaries: `hasIO`, `hasMutation`, `readsMemory`, `writesMemory`. Used by loop optimizer legality checks.

7. **Synthesis Expansion** (`runSynthesisPass`)  
   Replace function bodies containing `std::synthesize` calls with synthesized expressions.

8. **CF-CTRE** (Cross-Function Compile-Time Reasoning Engine)  
   Execute pure functions at compile time to fold constants, eliminate dead branches, and detect uniform return values. (See §28 for full details.)

9. **Abstract Interpretation** (`CTAbstractInterpreter`)  
   Compute variable ranges (`CTInterval` lattice) via flow-sensitive analysis. Proves bounds-check safety, division safety, and enables range-conditioned rewrites.

10. **E-Graph Optimization** (`runEGraphOptimizer`)  
    Apply algebraic rewrite rules exhaustively until saturation. Extracts the minimum-cost term from each equivalence class. (See §26.1.)

11. **Code Generation** (`CodeGenerator`)  
    Emit LLVM IR, run LLVM optimization passes, invoke linker. Produces executable or object file.

### 25.2 Pass manager organization

The compiler uses two pass managers:

#### AST-level pass manager (`OptimizationOrchestrator`)
- Phases 3–10 (pre-codegen).
- Two run modes (`include/opt_orchestrator.h:96-110`):
  - **Pipeline mode** (`runPrepasses()`): runs the full per-O-level pass pipeline in dependency order.
  - **Demand mode** (`runToProvide(fact)`): runs only the minimal set of passes whose `provides_` declarations cover the requested analysis fact.
- Invalidation tracking: transformations invalidate dependent analyses (e.g., e-graph invalidates `purity`) — the cascade is computed by `AnalysisDependencyGraph` (see §25.2.3).

#### LLVM IR pass manager (`llvm::ModulePassManager`)
- Phases 11+ (post-codegen).
- Fixed pipeline (no demand-driven scheduling).
- Stages: canonicalization → loop transforms → midend (IPSCCP, GVN, DSE) → vectorizer → superoptimizer → HGOE.

### 25.2.1 Pass framework (PassMetadata / PassRegistry / IPass)

Every AST-level pass is described by a `PassMetadata` struct (`include/opt_pass.h:80-95`) and registered in the global `PassRegistry`. The orchestrator never hard-codes pass classes; it discovers them through the registry and orders them by their declared dependencies.

**`PassMetadata` fields** (`opt_pass.h:80-95`):

| Field | Meaning |
|-------|---------|
| `id` | Stable numeric ID assigned at registration time |
| `name` | Short identifier (e.g. `"purity"`) used in diagnostics and tests |
| `description` | One-line description for `--verbose` output |
| `phase` | `PassPhase` (see below) |
| `kind` | `PassKind`: `Analysis` / `SemanticTransform` / `CostTransform` |
| `requires_` | Analysis facts that must be valid before the pass runs |
| `provides_` | Analysis facts the pass produces or refreshes |
| `invalidates_` | Analysis facts the pass invalidates (when it modifies the program) |

**`PassPhase`** (`opt_pass.h:37-43`) — coarse pipeline stage assignment:

| Value | Used for |
|-------|----------|
| `Preprocessing` | Source-level analysis before semantic checks |
| `EvaluationAnalysis` | Purity detection, effect inference, CF-CTRE |
| `ASTTransform` | AST rewrites (e-graph, OPTMAX folder, loop fusion) |
| `IRPipeline` | *Reserved* — LLVM pass-manager pipeline |
| `BackendTuning` | *Reserved* — superoptimizer, HGOE, post-pipeline cleanup |

**`PassKind`** (`opt_pass.h:48-52`) controls O-level gating:
- `Analysis` — runs at every level that needs the fact
- `SemanticTransform` — must always be correct; safe at any level
- `CostTransform` — optional, cost-driven; skipped at O0

**`AnalysisFact`** (`opt_pass.h:61-72`) defines the canonical fact identifiers passes refer to: `string_types`, `array_types`, `constant_returns`, `purity`, `effects`, `synthesis`, `cfctre`, `egraph`, `range_analysis`, `rlc`. The `PassId::k*` extern variables in the same namespace expose stable numeric IDs after registration so tests can refer to passes without hard-coding numbers.

**`IPass`** (`opt_pass.h:146-157`) is the polymorphic interface AST-level passes implement (`metadata()` + `run(Program*, OptimizationContext&)`). IR-level passes that live inside LLVM's `PassManager` are described only by their metadata and do not implement `IPass`.

The registry's `topologicalOrder(subset)` (`opt_pass.h:121-122`) computes a valid run order honouring `requires → provides` edges and throws `std::logic_error` if the dependency graph contains a cycle.

### 25.2.2 PassContract & IRInvariant

`PassContract` (`opt_pass.h:226-252`) is a richer companion to `PassMetadata` that adds **structural IR invariants** on top of the analysis-fact model. It exists so the scheduler can reason about whether the LLVM IR is in the shape a pass needs (e.g. the loop vectorizer needs `LoopSimplify` form), not only about whether semantic facts have been computed.

A `PassContract` declares six lists:
- `requires_facts` / `provides_facts` / `invalidates_facts` — same model as `PassMetadata`
- `requires_inv` — `IRInvariant`s that must hold before the pass runs
- `establishes_inv` — `IRInvariant`s the pass guarantees on exit
- `invalidates_inv` — `IRInvariant`s the pass breaks (the scheduler must re-establish them before any subsequent consumer)
- `preserves_inv` — `IRInvariant`s the pass provably does not break

The four currently defined `IRInvariant` values (`opt_pass.h:203-208`):

| Invariant | Meaning |
|-----------|---------|
| `LoopSimplify` | Loops have dedicated preheaders and a single backedge |
| `LCSSA` | Loop-Closed SSA form — uses of loop-defined values exit through PHIs |
| `CanonicalIV` | Induction variables are in canonical form (IndVarSimplify completed) |
| `SimplifiedCFG` | Control-flow graph has been simplified (SimplifyCFGPass completed) |

`PassContract` is currently an adjunct to `PassMetadata`; the source comment at `opt_pass.h:223-225` notes that future work will migrate to `PassContract` as the sole pass descriptor.

### 25.2.3 AnalysisDependencyGraph (cascading invalidation)

`AnalysisDependencyGraph` (`opt_pass.h:283-343`) records "fact A depends on fact B" edges. When a transform invalidates fact B, every fact that transitively depends on B is also invalidated. Callers therefore only need to invalidate the *directly* affected fact; the cascade is computed automatically.

The standard OmScript dependency graph is built by `AnalysisDependencyGraph::createDefault()` (`opt_pass.h:336`):

```
constant_returns → (no dependencies)
purity           → constant_returns
effects          → purity
synthesis        → purity, effects
cfctre           → purity, effects, synthesis
egraph           → cfctre
range_analysis   → purity, effects, cfctre
```

Read this as "the named fact depends on the listed facts": invalidating `purity` therefore cascades to `effects`, `synthesis`, `cfctre`, `egraph`, and `range_analysis`. Lookup is via `getAllDependents(key)`, which performs a BFS over the dependency edges and returns the fact itself plus every transitive dependent.

**Thread safety** (`opt_pass.h:279-282`): construction (`addDependency`) is **not** thread-safe and must happen during single-threaded static initialisation; subsequent reads from multiple compilation threads are safe.

### 25.2.4 PipelineStage (six-stage compilation pipeline)

`PipelineStage` (`include/optimization_manager.h:83-109`) provides the stable vocabulary the `OptimizationManager` uses to label passes, schedule requests, and emit progress diagnostics. The six stages and their fixed ordering are:

| Stage | Mandate |
|-------|---------|
| `AST_ANALYSIS` (0) | Read-only AST analyses: string/array type pre-analysis, constant-return detection, purity inference, effect inference |
| `AST_TRANSFORM` (1) | Semantics-preserving AST transforms: synthesis expansion, CF-CTRE, e-graph saturation, range analysis |
| `IR_CANONICALIZE` (2) | LLVM IR normalization that establishes invariants for later loop transforms: LoopSimplify, LCSSA, IndVarSimplify, SimplifyCFG |
| `LOOP_TRANSFORM` (3) | Polyhedral and structural loop transforms (interchange, tiling, skewing, reversal, fusion, fission) — all routed through `UnifiedLoopTransformer` (see §26.14) |
| `IR_MIDEND` (4) | LLVM midend scalar + vectorization: inlining, IPSCCP, GVN, DSE, loop vectorizer, SLP, post-vec cleanup |
| `LATE_SUPEROPT_HGOE` (5) | Late peephole + synthesis: superoptimizer (§26.2), HGOE hardware-guided emission (§26.3), post-pipeline simplification |

Stages run in numerical order and `PassMetadata::phase` (a `PassPhase` value) maps to the corresponding `PipelineStage`.

### 25.3 Per-O-level pass list

#### O0 (debug)
1. Lexer
2. Parser
3. Type pre-analysis
4. Codegen (no optimization)

#### O1 (basic)
1. Lexer
2. Parser
3. Type pre-analysis
4. Purity inference (lightweight, no cross-function analysis)
5. CF-CTRE (same fuel/depth limits as O2 — see below)
6. Codegen
7. LLVM: SimplifyCFG, Mem2Reg, SROA, EarlyCSE

#### O2 (standard)
1–6. (All AST phases)
7. **Synthesis expansion** (if `std::synthesize` present)
8. **CF-CTRE** (fuel limit `kMaxInstructions = 10,000,000`, depth limit `kMaxDepth = 128` — see `include/cfctre.h:483-484`)
9. **Abstract interpretation**
10. **E-graph optimization** (`SaturationConfig`: `maxNodes = 50,000`, `maxIterations = 30` — see `include/egraph.h:331-332`)
11. Codegen
12. LLVM canonicalization: LoopSimplify, LCSSA, IndVarSimplify
13. **Polyhedral optimizer** (tiling, interchange, skewing — see §26.13)
14. LLVM midend: Inlining, IPSCCP, GVN, LICM, DSE, Loop Vectorizer, SLP Vectorizer
15. **Superoptimizer** (idiom recognition + algebraic + branch→select + synthesis, level 2 default — see §26.2)
16. **HGOE** (only when a hardware profile is available — i.e. `-march=` or `-mtune=` resolves to a known microarch — see §26.3)
17. Post-pipeline cleanup: AggressiveDCE, GlobalDCE

**CF-CTRE fuel/depth limits are constants, not per-O-level knobs.** All O-levels that run CF-CTRE share the same `kMaxInstructions` / `kMaxDepth` budgets. See `include/cfctre.h:483-484` and §28.10.

#### O3 (aggressive)
All O2 passes with the following raised knobs (verified in `src/codegen_opt.cpp:4381-4387`):
- Superoptimizer level forced to ≥ 3 → `synthesis.maxInstructions = 5` (vs. default `3` from `SynthesisConfig`) and `synthesis.costThreshold = 0.9`
- LLVM loop-unroll and inline thresholds: inherited from the LLVM pipeline preset for `-O3`; OmScript does not currently override these
- E-graph and CF-CTRE limits are **not** raised at O3 — they are constants (see above)

### 25.4 Diagnostics flow

Errors and warnings flow through the `Diagnostic` class:
```
Diagnostic(level, code, message, location) → DiagnosticManager → stderr
```

**Diagnostic codes**:
- `E001`–`E099`: Lexer/parser errors
- `E100`–`E199`: Type errors
- `E200`–`E299`: Semantic errors (purity, effects)
- `E300`–`E399`: Codegen errors (LLVM failures)

**Colorization**: Enabled when stderr is a TTY (ANSI escape codes).

### 25.5 Caching/incremental compilation

**Current status**: Not implemented. Every invocation recompiles from source.

**Planned**: Timestamp-based invalidation of `oms.toml` → build cache mapping.

---

## 26. Advanced Optimization Features

### 26.1 E-Graph Equality Saturation

#### Data Model

An **e-graph** (equivalence graph) is a compact representation of many equivalent program expressions. It consists of:

- **E-node** (`ENode`, `include/egraph.h:101-132`): A single operation with child class IDs, an optional integer `value` (for `Const`), `fvalue` (for `ConstF`), and `name` (for `Var` / `Call`). E-node equality uses bit-pattern comparison for floats so that NaN constants with the same payload deduplicate correctly.
- **E-class** (`EClass`, `egraph.h:157-176`): An equivalence class of e-nodes representing the same value, plus cached analysis flags (`constVal`, `isZero`, `isOne`, `isNonNeg`, `isPowerOfTwo`, `isBoolean`, `isFloat`, `isInt`) used by relational rules.
- **Union-find structure**: Efficiently merges e-classes when a rewrite proves two terms equivalent. Hash-consing on `ENode` (via `ENodeHash`, `egraph.h:135-150`) deduplicates structurally identical nodes on insertion.

The supported operation set is fixed by the `Op` enum (`egraph.h:55-98`): constants and variables (`Const`, `ConstF`, `Var`); arithmetic (`Add`, `Sub`, `Mul`, `Div`, `Mod`, `Neg`); bitwise (`BitAnd`, `BitOr`, `BitXor`, `BitNot`, `Shl`, `Shr`); comparisons (`Eq`, `Ne`, `Lt`, `Le`, `Gt`, `Ge`); logical (`LogAnd`, `LogOr`, `LogNot`); math (`Pow`, `Sqrt`); and special (`Ternary`, `Call`, `Nop`). Anything outside this set is opaque to the e-graph and is not subject to equality saturation.

**Example**:
```omscript
let x = (a + 0) * 1;
```
The e-graph after saturation contains:
```
Class1: {Var("a")}
Class2: {Const(0)}
Class3: {Add(Class1, Class2), Var("a")}   // a+0 ≡ a
Class4: {Const(1)}
Class5: {Mul(Class3, Class4), Var("a")}   // a*1 ≡ a
```
The cost model selects `Var("a")` (cost 0) over `Mul(Add(...), ...)` (cost 3).

#### Rewrite Rules (from `egraph_optimizer.cpp`)

The engine applies these rules exhaustively:

**Algebraic identities**:
```
x + 0 → x
x - 0 → x
x * 1 → x
x * 0 → 0
x / 1 → x
0 / x → 0  (if x ≠ 0)
x - x → 0
x / x → 1  (if x ≠ 0)
x % 1 → 0
```

**Commutativity**:
```
x + y → y + x
x * y → y * x
x & y → y & x
x | y → y | x
x ^ y → y ^ x
```

**Associativity**:
```
(x + y) + z → x + (y + z)
(x * y) * z → x * (y * z)
```

**Distributivity**:
```
x * (y + z) → (x * y) + (x * z)
x * (y - z) → (x * y) - (x * z)
```

**Bitwise**:
```
x & 0 → 0
x & ~0 → x
x | 0 → x
x | ~0 → ~0
x ^ 0 → x
x ^ x → 0
~(~x) → x
```

**Strength reduction** (when range analysis proves safety):
```
x / 2 → x >> 1   (when x ≥ 0)
x * 2 → x << 1
x % (2^n) → x & (2^n - 1)   (when x ≥ 0)
```

**Comparison folding**:
```
x < x → false
x <= x → true
x == x → true
x != x → false
```

#### Termination

The engine terminates when:
1. **Node limit reached**: `SaturationConfig::maxNodes = 50,000` (`include/egraph.h:331`). This is a fixed default — it is **not** raised at O3.
2. **Iteration limit reached**: `SaturationConfig::maxIterations = 30` (`include/egraph.h:332`) — one iteration = apply all rules to all nodes once.
3. **Saturation**: No new e-nodes added in an iteration.

A third `SaturationConfig` knob — `enableConstantFolding` (default `true`, `egraph.h:333`) — controls whether the engine folds `Op(Const, Const)` patterns during saturation in addition to applying rewrite rules.

#### Patterns and Rewrite Rules

Rewrite rules use the `Pattern` type (`egraph.h:182-241`) which has two kinds:

- **`Wildcard`** (`Pattern::Wild("?a")`) — matches any e-class and binds it to the named variable.
- **`OpMatch`** — requires a specific `Op` and (recursively) matching child patterns. Specialised constructors include:
  - `OpPat(op, children)` — match an operation and its children
  - `ConstPat(val)` / `ConstFPat(val)` — match a specific integer or float constant (set `matchConst` / `matchConstF`)
  - `AnyConst()` — match any integer constant without value constraint

A successful match produces a `Subst` (`egraph.h:244`) — a map from wildcard names to the bound class IDs.

A `RewriteRule` (`egraph.h:263-275`) bundles four things:

| Field | Meaning |
|-------|---------|
| `name` | Human-readable rule name (used in diagnostics) |
| `lhs` | Left-hand side `Pattern` to match |
| `rhs` | `RhsBuilder` callback `ClassId(EGraph&, const Subst&)` that constructs the replacement |
| `guard` | Optional `RuleGuard` predicate `bool(const EGraph&, const Subst&)` |

The optional **guard** turns the engine from a purely syntactic rewriter into a *relational* one: a guard can inspect bound `EClass` analysis flags (e.g. `isPowerOfTwo`, `isNonNeg`) before allowing the RHS to be built. This is how rules like "rewrite `x * c` to `x << log2(c)` only when `c` is a power of two" stay sound.

#### Cost Model and Extraction

Once saturation completes, the engine **extracts** a single representative e-node per class to produce the final program. The cost is computed by `CostModel` (`egraph.h:306-323`) with two notable parameters beyond per-node latency:

| Field | Default | Meaning |
|-------|---------|---------|
| `regBudget` | `13` | Architectural GPRs the extractor may assume are simultaneously available (16 x86-64 GPRs minus RSP, RBP, and one frame/scratch reserve). Set to 0 to disable register-pressure-aware extraction. |
| `spillPenalty` | `5.0` | Cycles charged per excess simultaneously-live value beyond `regBudget`. Models a stack-slot spill+reload round-trip. |

When the extractor would exceed `regBudget`, each excess live value adds `spillPenalty` to the candidate's cost, biasing the selection toward shallower (lower-pressure) sub-trees even when their per-node latency is higher. HGOE may install target-specific `regBudget` / `spillPenalty` values from a resolved `MicroarchProfile` (e.g. AArch64's 31 GPRs → `regBudget = 27`).

The `INFINITE_COST` sentinel (`egraph.h:283`, `1e18`) is used to mark unextractable nodes so they never win cost comparisons.

#### Cost Model

Each e-node has a cost:
```
Const → 0
Var   → 0
UnaryOp → 1 + cost(child)
BinaryOp → 2 + cost(left) + cost(right)
```

The **extraction** phase walks each e-class and selects the minimum-cost representative.

#### Extraction

After saturation, the extractor builds the output AST by recursively choosing the cheapest e-node from each e-class. Ties are broken by:
1. Prefer `Const` and `Var` (zero cost).
2. Prefer unary over binary.
3. Prefer earlier-inserted node (deterministic tie-break).

#### Examples

**Example 1: Constant folding**
```omscript
let x = (3 + 5) * 2;
```
After saturation:
```
Class1: {Const(3)}
Class2: {Const(5)}
Class3: {Add(Class1, Class2), Const(8)}   // 3+5 = 8
Class4: {Const(2)}
Class5: {Mul(Class3, Class4), Const(16)}  // 8*2 = 16
```
Extracted: `Const(16)`.

**Example 2: Algebraic simplification**
```omscript
let y = x * 1 + 0 - 0;
```
After saturation:
```
Class1: {Var("x")}
Class2: {Const(1)}
Class3: {Mul(Class1, Class2), Var("x")}   // x*1 = x
Class4: {Const(0)}
Class5: {Add(Class3, Class4), Var("x")}   // x+0 = x
Class6: {Sub(Class5, Class4), Var("x")}   // x-0 = x
```
Extracted: `Var("x")`.

### 26.2 Superoptimizer

#### Strategy

The superoptimizer searches for instruction sequences that:
1. **Compute the same function** as the original code (verified via test vectors).
2. **Are strictly cheaper** (lower latency, fewer uops, better throughput).

It operates in two modes:

##### Idiom Recognition
Pattern-match known high-level operations encoded in low-level IR:
- **Popcount**: Kernighan's algorithm (loop with `x &= x-1`).
- **Byte swap**: Shift-and-mask sequences for endian conversion.
- **Bit rotation**: `(x << n) | (x >> (w - n))`.
- **Min/max**: `select(cmp, a, b)` patterns.

When recognized, emit LLVM intrinsic (`@llvm.ctpop`, `@llvm.bswap`, `@llvm.fshl`, `@llvm.smin`).

##### Enumerative Synthesis
For small expression trees (≤3 instructions), enumerate all candidate sequences using allowed ops (`add`, `sub`, `mul`, `shl`, `shr`, `and`, `or`, `xor`, `neg`) and test against 16 random test vectors.

#### Pattern Database

From `superoptimizer.cpp`:

**Idioms** (enum `Idiom`):
```
PopCount, ByteSwap, RotateLeft, RotateRight,
CountLeadingZeros, CountTrailingZeros,
AbsoluteValue, IntMin, IntMax, IsPowerOf2,
SignExtend, BitFieldExtract,
MultiplyByConst, DivideByConst,
ConditionalNeg, SaturatingAdd, SaturatingSub,
ConditionalIncrement, ConditionalDecrement,
AverageWithoutOverflow, SignFunction, NextPowerOf2
```

**Example transforms**:
```c++
// Hacker's Delight §5-2: average without overflow
(a & b) + ((a ^ b) >> 1)  →  floor((a + b) / 2)

// Hacker's Delight §2-7: sign function
select(x > 0, 1, select(x < 0, -1, 0))  →  sign(x)

// Hacker's Delight §3-1: next power of 2
clz(x-1) → bit_smear + 1 → 1 << (bw - ctlz(x-1))
```

#### Levels

Controlled by `-fsuperopt-level=N` (default `2` — see `include/codegen.h:1267`). The superoptimizer runs only at `O2+` and only when `-fno-superopt` was not passed; it is gated on `enableSuperopt_ && optimizationLevel >= O2` in `src/codegen_opt.cpp:4369`.

| Level | Idioms | Algebraic | Branch→Select | Synthesis | Synthesis tuning |
|-------|--------|-----------|---------------|-----------|------------------|
| 0     | ✗      | ✗         | ✗             | ✗         | (superopt disabled entirely) |
| 1     | ✓      | ✓         | ✗ (`enableBranchOpt = false`) | ✗ (`enableSynthesis = false`) | n/a |
| 2     | ✓      | ✓         | ✓             | ✓         | `SynthesisConfig` defaults: `maxInstructions = 3`, `costThreshold = 1.0` |
| 3     | ✓      | ✓         | ✓             | ✓ (deeper) | `maxInstructions = 5`, `costThreshold = 0.9` |

Level 3 is also forced when the global optimization level is `O3`, regardless of `-fsuperopt-level=` (`codegen_opt.cpp:4384`). There is **no fixed per-level "candidate budget" knob**; cost-bounded candidate enumeration is governed by `SynthesisConfig::maxInstructions` (sequence length) and the cost threshold above.

#### Verification

Each candidate is evaluated on `SynthesisConfig::numTestVectors = 16` test vectors (`include/superoptimizer.h:128`) of the form:
```c++
TestVector { inputs: [a, b, ...], expectedOutput: f(a, b, ...) }
```
A candidate is accepted iff every test vector matches the original. This is a **probabilistic** equivalence check, not a formal proof — the superoptimizer does not currently invoke an SMT solver.

#### Examples

**Example 1: Popcount**
```llvm
; Original (loop-based Kernighan algorithm)
%cnt = phi i64 [ 0, %entry ], [ %cnt.inc, %loop ]
%x = phi i64 [ %input, %entry ], [ %x.next, %loop ]
%x.next = and i64 %x, %x.sub  ; x &= x - 1
%cnt.inc = add i64 %cnt, 1
br i1 %cmp, %loop, %exit

; Superoptimized
%result = call i64 @llvm.ctpop.i64(i64 %input)
```

**Example 2: Multiply by constant**
```llvm
; Original
%mul = mul i64 %x, 7

; Superoptimized (lea + shift)
%t1 = shl i64 %x, 3    ; x * 8
%t2 = sub i64 %t1, %x  ; x * 7
```

### 26.3 HGOE (Hardware Graph Optimization Engine)

HGOE models the target CPU as a directed graph of execution resources and schedules instructions to minimize pipeline stalls.

#### Pass Order
1. **Build hardware graph** from `-march` profile.
2. **Classify operations** into resource types (IntegerALU, VectorALU, FMAUnit, LoadUnit, etc.).
3. **Construct dependency graph** of LLVM IR instructions (data + control dependencies).
4. **Compute RecMII** (Recurrence-constrained Minimum Initiation Interval) for loops.
5. **List scheduling** with priority function: `priority = latency + num_successors`.
6. **Instruction selection** (prefer instructions that map to underutilized ports).
7. **Vectorization width recommendation** (fit in L1 cache, maximize SIMD utilization).

#### Hardware Profiles

From `hardware_graph.cpp`, built-in profiles:

| Profile          | `-march` Value       | L1D (KB) | L2 (KB) | Integer ALU Ports | Vector ALU Ports | FMA Units |
|------------------|----------------------|----------|---------|-------------------|------------------|-----------|
| **Generic x86-64** | `x86-64`          | 32       | 256     | 2                 | 1                | 0         |
| **Skylake**      | `skylake`            | 32       | 256     | 4                 | 2                | 2         |
| **Zen 3**        | `znver3`             | 32       | 512     | 4                 | 2                | 2         |
| **Ice Lake**     | `icelake-server`     | 48       | 512     | 5                 | 2                | 2         |
| **Sapphire Rapids** | `sapphirerapids`  | 48       | 2048    | 5                 | 2                | 2         |
| **Neoverse V1**  | `neoverse-v1`        | 64       | 1024    | 4                 | 2                | 2         |

The cache cells in this table are illustrative; the authoritative numbers live in the per-microarch profile builders in `src/hardware_graph.cpp` (e.g. `zen3Profile`, `zen4Profile`, `alderlakeProfile`, `neoverseV2Profile`, `graviton3Profile` / `graviton4Profile`, `lunarLakeProfile`, `zen5Profile`). When in doubt, consult the profile function for the CPU you care about.

#### Floating-Point Precision Levels

Independent of `-march`, HGOE supports per-variable / per-operation floating-point precision control via the `FPPrecision` enum (`include/hardware_graph.h:48-53`):

| Level | Meaning |
|-------|---------|
| `Strict` | Full IEEE-754 compliance. No reassociation, no NaN/Inf assumptions. |
| `Medium` | Allow limited reassociation and some vectorization; preserve NaN/Inf. |
| `Fast` | Equivalent to `-ffast-math`: reassociation, reciprocal transforms, ignoring NaN/Inf, fused operations all permitted. |

`FPPrecision` is an alternative to the global `-ffast-math` flag for cases where only some computations are safe to relax. When two operands carry different precisions they are merged via a *conservative meet* (`hardware_graph.h:68-71`): the **stricter** level wins, so combining a `Strict` operand with a `Fast` operand yields `Strict` semantics. The string names `"strict" / "medium" / "fast"` are exposed by `fpPrecisionName()` (`hardware_graph.h:56-63`).

#### Cache-Aware Optimization

HGOE includes a cache model used to choose tile sizes and software-prefetch distances. The `CacheModel` struct (`hardware_graph.h:80-90`, defaults shown) is built from the resolved `MicroarchProfile` via `buildCacheModel()`:

| Field | Default | Meaning |
|-------|---------|---------|
| `l1Size` / `l1Latency` / `l1LineSize` | 32 KB / 4 cyc / 64 B | L1D parameters |
| `l2Size` / `l2Latency` | 256 KB / 12 cyc | L2 parameters |
| `l3Size` / `l3Latency` | 8192 KB / 40 cyc | L3 parameters |
| `memLatency` / `memBandwidth` | 200 cyc / 40.0 GB/s | Main memory parameters |

Memory accesses inside loops are classified by an `AccessPattern` (`hardware_graph.h:98-104`): `Unknown`, `Sequential` (best locality), `Strided` (predictable, may miss cache), `Random` (poor locality), `Streaming` (write-once / read-once, bypass-friendly). The classification feeds prefetch-insertion and AoS→SoA layout decisions.

The cache-aware pass reports its work through `CacheOptStats` (`hardware_graph.h:107-112`):

| Counter | Meaning |
|---------|---------|
| `loopsTiled` | Loops with tiling metadata added |
| `loopsInterchanged` | Loops reordered for locality |
| `prefetchesInserted` | Software prefetch hints added |
| `layoutHints` | AoS→SoA suggestions emitted |

#### Operation Classes

| Class           | Example LLVM Ops                     | Typical Latency (cycles) |
|-----------------|--------------------------------------|--------------------------|
| `IntegerALU`    | `add`, `sub`, `and`, `or`, `xor`, `shl`, `shr` | 1             |
| `IntegerMul`    | `mul i32`, `mul i64`                 | 3–4                      |
| `IntegerDiv`    | `sdiv`, `udiv`                       | 10–40                    |
| `VectorALU`     | `fadd <4 x float>`, `vadd`           | 3–4                      |
| `FMAUnit`       | `@llvm.fma.f64`, `vfmadd`            | 4–5                      |
| `LoadUnit`      | `load`, `load <4 x i32>`             | 4 (L1 hit)               |
| `StoreUnit`     | `store`, `store <4 x i32>`           | 1 (write-back)           |
| `BranchUnit`    | `br`, `switch`, `indirectbr`         | 1 (predicted)            |

#### Latency Table

From `hardware_graph.cpp`:

```cpp
double getOperationLatency(ResourceType rt, const MicroarchProfile& prof) {
    switch (rt) {
    case ResourceType::IntegerALU:    return 1.0;
    case ResourceType::IntegerMul:    return prof.intMulLatency;   // 3–4
    case ResourceType::IntegerDiv:    return prof.intDivLatency;   // 20–40
    case ResourceType::VectorALU:     return prof.vecAluLatency;   // 3
    case ResourceType::FMAUnit:       return prof.fmaLatency;      // 4
    case ResourceType::LoadUnit:      return prof.l1Latency;       // 4
    case ResourceType::StoreUnit:     return 1.0;                  // store is write-back
    case ResourceType::BranchUnit:    return 1.0;                  // predicted
    case ResourceType::L1DCache:      return prof.l1Latency;
    case ResourceType::L2Cache:       return prof.l2Latency;       // 12
    case ResourceType::L3Cache:       return prof.l3Latency;       // 40
    case ResourceType::MainMemory:    return prof.memLatency;      // 200
    default: return 1.0;
    }
}
```

#### Priority

List scheduler priority:
```
priority(inst) = latency(inst) + height(inst)
height(inst)   = longest path from inst to any root in the DAG
```

#### List Scheduling

Algorithm (from `hardware_graph.cpp`):
```
1. Build dependency DAG (data + control edges).
2. Compute height for each node.
3. Initialize ready queue with nodes that have no predecessors.
4. While ready queue is not empty:
   a. Select node with highest priority.
   b. Find earliest cycle when all resource constraints are satisfied.
   c. Schedule node at that cycle.
   d. Update port usage counters.
   e. Add successors to ready queue when all predecessors are scheduled.
```

#### Instruction Selection

When multiple LLVM instructions can implement the same operation:
- Prefer instructions that map to underutilized ports.
- Example: prefer `lea` over `add` + `shl` when AGU is idle.

#### Vectorize-Width

HGOE recommends SIMD width based on:
- **L1 cache capacity**: tile loops so working set fits in L1.
- **Vector register count**: avoid spills.
- **Operation mix**: balance scalar and vector ops to avoid port stalls.

**Example**: For a loop processing `float` arrays, HGOE recommends width=8 (256-bit AVX) on Skylake, width=16 (512-bit AVX-512) on Ice Lake.

#### RecMII (Recurrence-constrained Minimum Initiation Interval)

For loops with carried dependencies:
```
RecMII = max(cycle_length / parallelism)
```
where `cycle_length` = sum of latencies along the recurrence.

**Example**:
```omscript
for i in 0..n {
    sum = sum + arr[i];   // carried dependence on `sum`
}
```
RecMII = latency(`fadd`) = 4 cycles. Scheduler ensures loop II ≥ 4.

### 26.4 OPTMAX Optimization-Pipeline Implications

**OPTMAX** is a block-level optimization pass that runs **before** IR generation. It:
1. Identifies expression-heavy basic blocks (≥5 operations, no calls).
2. Builds a local data-flow graph.
3. Applies strength reduction, constant folding, and dead-code elimination.
4. Emits simplified AST.

**Pipeline integration**:
- OPTMAX runs at O1+ (disabled at O0).
- Precedes CF-CTRE (so CF-CTRE sees simplified expressions).
- Does **not** interact with e-graph (separate AST-level pass).

**Example**:
```omscript
let y = x * 10 + x * 5;
```
OPTMAX combines to `x * 15` before e-graph sees it.

### 26.5 PGO (Profile-Guided Optimization)

#### Record Phase (`-fpgo-gen=<path>`)
1. Compiler instruments binary with `__llvm_profile_write()` calls.
2. On program exit, profile data is written to `<path>` (`.profraw` format).

#### Use Phase (`-fpgo-use=<path>`)
1. Compiler reads profile and annotates IR with:
   - `!prof` metadata on branches (taken/not-taken counts).
   - `!prof` metadata on calls (call counts).
2. LLVM passes consume profile:
   - **Inliner**: inline hot call sites.
   - **Block placement**: layout hot blocks contiguously.
   - **Vectorizer**: prioritize hot loops.

#### File Format

LLVM raw profile (`.profraw`) contains:
- Function counters (one per instrumentation point).
- Edge counters (branch taken counts).

Merge multiple runs via:
```bash
llvm-profdata merge *.profraw -o merged.profdata
```

#### Feedback Consumed

- **Branch weights**: `br i1 %cond, label %hot [weight 1000], label %cold [weight 1]`.
- **Call site hotness**: inline functions called >100 times.
- **Loop trip counts**: unroll loops with known small trip counts.

### 26.6 Escape Analysis & Stack Allocation

**Not yet implemented**. Planned:
- Detect heap allocations (`newRegion()`, `alloc()`) that do not escape function scope.
- Lower to `alloca` (stack allocation) when safe.

### 26.7 Bounds-Check Hoisting

Integrated into CF-CTRE abstract interpretation (Phase 9):
- For each array access `arr[i]`, compute interval `i ∈ [lo, hi]`.
- If `hi < len(arr)` and `lo ≥ 0`, mark access as safe.
- Codegen skips bounds check for safe accesses.

**Example**:
```omscript
for i in 0..10 {
    print(arr[i]);   // i ∈ [0, 9], proven safe if len(arr) ≥ 10
}
```

### 26.8 Allocation Elimination & SROA

LLVM's **SROA** (Scalar Replacement of Aggregates) pass:
- Splits `alloca`'d structs into individual SSA values.
- Enables further optimizations (constant propagation, dead-store elimination).

**Example**:
```omscript
struct Point { x: int, y: int }
let p = Point { x: 3, y: 4 };
let z = p.x + p.y;
```
After SROA:
```llvm
%x = alloca i64
%y = alloca i64
store i64 3, i64* %x
store i64 4, i64* %y
%x.val = load i64, i64* %x
%y.val = load i64, i64* %y
%z = add i64 %x.val, %y.val
```
After `mem2reg`:
```llvm
%z = add i64 3, 4
```

### 26.9 Reduction Recognition

**Loop vectorizer** recognizes reductions:
```omscript
let sum = 0;
for i in 0..n {
    sum += arr[i];
}
```
Transformed to:
```llvm
%vec.sum = call <4 x float> @llvm.reduce.fadd(<4 x float> %partial)
```

**Supported reductions**: `add`, `mul`, `min`, `max`, `and`, `or`, `xor`.

### 26.10 OptStats

The optimizer **orchestrator** (not the optimization manager) tracks pass-execution statistics in an internal `RunStats` struct (`include/opt_orchestrator.h:120-127`):

| Field | Meaning |
|-------|---------|
| `passesRun` | Number of passes that actually executed |
| `passesSkipped` | Number of passes whose precondition was already satisfied (analysis cache hit) |
| `totalElapsedMs` | Wall-clock time across the whole pass pipeline (ms) |
| `passTimings` | Per-pass `(name, elapsed_ms)` records |

These are **not** exposed as a public flag. When `--verbose` is set the orchestrator prints a summary line of the form:
```
Optimizer: <run> ran, <skip> skipped, <ms> ms total
```
and (per-pass timings are recorded but printed only when verbose mode is on; see `src/opt_orchestrator.cpp:493-496`). There are currently **no per-component counters** for e-graph nodes created, superoptimizer replacements, HGOE rescheduling, polyopt loop counts, etc. — those would require adding fields to `RunStats` and wiring each subsystem to populate them.

### 26.11 Compile-Time Array Evaluation

CF-CTRE evaluates array operations at compile time:
```omscript
const arr = comptime {
    let a = [1, 2, 3];
    push(a, 4);
    a
};
```
Result: `arr = [1, 2, 3, 4]` (heap-allocated in `CTHeap`, serialized into LLVM global constant).

### 26.12 RLC (Region Lifetime Coalescing)

#### Algorithm

RLC merges disjoint region lifetimes to eliminate redundant allocations:

1. **Liveness analysis**: Compute `[create, invalidate]` intervals for each region variable.
2. **Disjointness check**: Regions `R1` and `R2` are disjoint if `R1.invalidate < R2.create`.
3. **Coalescing**: Redirect `alloc(R2, size)` calls to `alloc(R1, size)`.
4. **Validation**: Verify no reference to `R1` survives past its `invalidate`.

#### Diagnostics

- **E013**: Region variable has no `invalidate` before function exit (leak).
- **E014**: Region variable used after `invalidate` (use-after-free).

#### Example

```omscript
fn process() {
    let r1 = newRegion();
    let data1 = alloc(r1, 1024);
    // ... use data1 ...
    invalidate(r1);

    let r2 = newRegion();          // RLC merges r2 into r1
    let data2 = alloc(r2, 512);    // reuses r1's memory
    // ... use data2 ...
    invalidate(r2);
}
```

After RLC:
```omscript
fn process() {
    let r1 = newRegion();
    let data1 = alloc(r1, 1024);
    // ... use data1 ...
    // invalidate(r1) removed

    // r2 eliminated
    let data2 = alloc(r1, 512);    // reuses r1
    // ... use data2 ...
    invalidate(r1);
}
```

### 26.13 Polyhedral Optimizer

Implemented in `src/polyopt.cpp` (~1,800 lines). The polyhedral optimizer:
1. **Detects SCoPs** (Static Control Parts): loops with affine bounds and subscripts.
2. **Builds iteration domains**: polyhedra `{[i,j] : 0 ≤ i < N, 0 ≤ j < M}`.
3. **Computes dependences**: Fourier-Motzkin elimination to test `∃ (i,j) : Write(i,j) before Read(i,j)`.
4. **Applies transforms** (each individually toggleable — see `PolyOptConfig`):
   - **Tiling** (`enableTiling`): Insert tile loops `for ii in 0..N step T` (tile size selected from L1/L2 cache hints).
   - **Interchange** (`enableInterchange`): Swap loop orders to maximize locality.
   - **Skewing** (`enableSkewing`): `j' = j + factor * i` (enables wavefront parallelism).
   - **Reversal** (`enableReversal`): Iterate the loop backwards when legal and profitable.
   - **Fusion** (`enableFusion`): Merge adjacent loops with compatible iteration spaces.
   - **Fission** (`enableFission`): Split a loop body into multiple independent loops.
5. **Regenerates IR**: emits transformed loops back into the AST/IR.

**Configuration knobs** (`include/polyopt.h:108-145`, all defaults shown):

| Knob | Default | Meaning |
|------|---------|---------|
| `enableTiling` / `enableInterchange` / `enableSkewing` / `enableFusion` / `enableFission` / `enableReversal` | `true` | Per-transform on/off switches |
| `l1CacheBytes` | `0` (auto-detect via TTI) | L1 data-cache size used to choose tile sizes |
| `l2CacheBytes` | `0` (auto-detect) | L2 data-cache size for outer-tile sizing |
| `cacheLineBytes` | `64` | Cache line size in bytes (x86-64 default) |
| `maxLoopDepth` | `6` | Maximum loop-nest depth analysed (deeper nests have exponential analysis cost) |
| `maxScopStatements` | `32` | Maximum statements per SCoP — larger SCoPs are bypassed |

**Legality**: A transform is legal iff the transformed dependence distance vectors are lexicographically non-negative.

### 26.14 Loop Transformation Framework

See `loop_transform_framework.h`. The **UnifiedLoopTransformer** centralizes all loop transforms:

```cpp
LoopTransformRequest req;
req.transform  = LoopTransform::Tiling;
req.outerLoop  = loop;
req.function   = &F;
req.effects    = ctx.effects(F.getName().str());

UnifiedLoopTransformer xformer(legality, costModel);
LoopTransformResult res = xformer.execute(req, SE, DT, LI);
if (res.applied()) { /* update analyses */ }
```

**Supported transforms** (enum `LoopTransform`):
- `Tiling` — cache blocking
- `Interchange` — reorder loop levels
- `Reversal` — reverse iteration direction
- `Skewing` — shear transformation
- `Fusion` — merge adjacent loops
- `Fission` — split loop body

**Decision flow**:
1. Query `LegalityService::checkLegality()` (high-level effect check).
2. Query `polyopt::checkLoopLegality()` (fine-grained dependence check).
3. Query `CostModel::isProfitable()`.
4. Dispatch to `polyopt::optimizeFunction()` if all checks pass.

---

## 27. Integer Type-Cast Reference

OmScript provides explicit integer type casts as built-in functions. These are **not** part of the type system (OmScript is dynamically typed at runtime) but are codegen-time operations that emit truncation or sign-extension instructions.

### 27.1 Overview Table

| Cast     | Signature        | Domain          | Result Type (LLVM) | Operation                          | Overflow Behavior |
|----------|------------------|-----------------|--------------------|------------------------------------|--------------------|
| `u64(x)` | `int → int`      | Any integer     | `i64`              | Identity (no-op)                   | N/A                |
| `i64(x)` | `int → int`      | Any integer     | `i64`              | Identity (no-op)                   | N/A                |
| `int(x)` | `int → int`      | Any integer     | `i64`              | Identity (no-op)                   | N/A                |
| `uint(x)`| `int → int`      | Any integer     | `i64`              | Identity (no-op)                   | N/A                |
| `u32(x)` | `int → int`      | `[0, 2^32-1]`   | `i64` (zero-extended) | Truncate to 32 bits + zero-extend | Wraps mod 2^32     |
| `i32(x)` | `int → int`      | `[-2^31, 2^31-1]` | `i64` (sign-extended) | Truncate to 32 bits + sign-extend | Wraps mod 2^32     |
| `u16(x)` | `int → int`      | `[0, 2^16-1]`   | `i64` (zero-extended) | Truncate to 16 bits + zero-extend | Wraps mod 2^16     |
| `i16(x)` | `int → int`      | `[-2^15, 2^15-1]` | `i64` (sign-extended) | Truncate to 16 bits + sign-extend | Wraps mod 2^16     |
| `u8(x)`  | `int → int`      | `[0, 255]`      | `i64` (zero-extended) | Truncate to 8 bits + zero-extend  | Wraps mod 256      |
| `i8(x)`  | `int → int`      | `[-128, 127]`   | `i64` (sign-extended) | Truncate to 8 bits + sign-extend  | Wraps mod 256      |
| `bool(x)`| `int → int`      | Any integer     | `i64` (0 or 1)     | Test non-zero → 1, zero → 0        | N/A                |

### 27.2 Identity casts: `u64`, `i64`, `int`, `uint`

**Semantics**: No-op at runtime. Returns input unchanged.

**LLVM IR**: No instruction emitted (SSA value forwarded).

**Purpose**: Documentation / explicitness in source code.

**Example**:
```omscript
let x: int = 42;
let y = u64(x);   // y == 42, no IR generated
```

### 27.3 `u32(x)`, `i32(x)`

#### `u32(x)` — Unsigned 32-bit truncation + zero-extend

**Bit-level operation**:
1. Truncate `x` to low 32 bits: `x' = x & 0xFFFFFFFF`.
2. Zero-extend to 64 bits (high 32 bits are 0).

**Overflow behavior**: Wraps modulo 2³².

**LLVM IR**:
```llvm
%trunc = trunc i64 %x to i32
%result = zext i32 %trunc to i64
```

**Compile-time folding**: If `x` is a constant, fold to `x & 0xFFFFFFFF`.

**Example**:
```omscript
let a = u32(-1);         // a = 4294967295 (0xFFFFFFFF)
let b = u32(0x1_0000_0000);  // b = 0 (wraps)
```

#### `i32(x)` — Signed 32-bit truncation + sign-extend

**Bit-level operation**:
1. Truncate `x` to low 32 bits.
2. Sign-extend to 64 bits (replicate bit 31 into bits 32–63).

**Overflow behavior**: Wraps modulo 2³².

**LLVM IR**:
```llvm
%trunc = trunc i64 %x to i32
%result = sext i32 %trunc to i64
```

**Compile-time folding**: If `x` is constant:
```c++
int32_t tmp = (int32_t)x;  // truncate + reinterpret as signed
return (int64_t)tmp;       // sign-extend
```

**Example**:
```omscript
let c = i32(0x8000_0000);   // c = -2147483648 (sign bit set)
let d = i32(0x7FFF_FFFF);   // d = 2147483647
let e = i32(0x1_8000_0000); // e = -2147483648 (wraps, bit 31 set)
```

### 27.4 `u16(x)`, `i16(x)`

#### `u16(x)` — Unsigned 16-bit truncation + zero-extend

**Bit-level operation**:
1. Truncate to low 16 bits: `x & 0xFFFF`.
2. Zero-extend to 64 bits.

**LLVM IR**:
```llvm
%trunc = trunc i64 %x to i16
%result = zext i16 %trunc to i64
```

**Example**:
```omscript
let f = u16(0x1_FFFF);   // f = 0xFFFF = 65535
let g = u16(-1);         // g = 65535 (0xFFFF)
```

#### `i16(x)` — Signed 16-bit truncation + sign-extend

**Bit-level operation**:
1. Truncate to low 16 bits.
2. Sign-extend (replicate bit 15 into bits 16–63).

**LLVM IR**:
```llvm
%trunc = trunc i64 %x to i16
%result = sext i16 %trunc to i64
```

**Example**:
```omscript
let h = i16(0x8000);     // h = -32768 (0x8000 sign-extends to 0xFFFF_FFFF_FFFF_8000)
let i = i16(0x7FFF);     // i = 32767
```

### 27.5 `u8(x)`, `i8(x)`

#### `u8(x)` — Unsigned 8-bit truncation + zero-extend

**Bit-level operation**:
1. Truncate to low 8 bits: `x & 0xFF`.
2. Zero-extend to 64 bits.

**LLVM IR**:
```llvm
%trunc = trunc i64 %x to i8
%result = zext i8 %trunc to i64
```

**Example**:
```omscript
let j = u8(255);    // j = 255
let k = u8(256);    // k = 0 (wraps)
let l = u8(-1);     // l = 255 (0xFF)
```

#### `i8(x)` — Signed 8-bit truncation + sign-extend

**Bit-level operation**:
1. Truncate to low 8 bits.
2. Sign-extend (replicate bit 7 into bits 8–63).

**LLVM IR**:
```llvm
%trunc = trunc i64 %x to i8
%result = sext i8 %trunc to i64
```

**Example**:
```omscript
let m = i8(127);    // m = 127
let n = i8(128);    // n = -128 (0x80 sign-extends to 0xFFFF_FFFF_FFFF_FF80)
let o = i8(-1);     // o = -1 (0xFF sign-extends to 0xFFFF_FFFF_FFFF_FFFF)
```

### 27.6 `bool(x)`

**Semantics**: Test if `x` is non-zero.

**Result**: `1` if `x ≠ 0`, `0` if `x == 0`.

**LLVM IR**:
```llvm
%cmp = icmp ne i64 %x, 0
%result = zext i1 %cmp to i64
```

**Compile-time folding**: If `x` is constant, fold to `(x != 0) ? 1 : 0`.

**Example**:
```omscript
let p = bool(42);   // p = 1
let q = bool(0);    // q = 0
let r = bool(-5);   // r = 1
```

### 27.7 Compile-time folding rules table

When the argument to a cast is a **compile-time constant**, CF-CTRE folds the cast:

| Cast     | Constant Folding Rule (C++ equivalent)                          |
|----------|-----------------------------------------------------------------|
| `u64(x)` | `x`                                                             |
| `i64(x)` | `x`                                                             |
| `int(x)` | `x`                                                             |
| `uint(x)`| `x`                                                             |
| `u32(x)` | `(uint64_t)(uint32_t)x`                                         |
| `i32(x)` | `(int64_t)(int32_t)x`                                           |
| `u16(x)` | `(uint64_t)(uint16_t)x`                                         |
| `i16(x)` | `(int64_t)(int16_t)x`                                           |
| `u8(x)`  | `(uint64_t)(uint8_t)x`                                          |
| `i8(x)`  | `(int64_t)(int8_t)x`                                            |
| `bool(x)`| `(x != 0) ? 1 : 0`                                              |

### 27.8 Type-system interaction

Casts are **runtime operations**, not type annotations. OmScript has a single `int` type at the AST level. The cast functions are built-in functions that emit IR at codegen time.

**Contrast with statically-typed languages**:
- In C: `uint32_t x = (uint32_t)y;` — type-level constraint enforced at compile time.
- In OmScript: `let x = u32(y);` — runtime operation, `x` still has dynamic type `int`.

### 27.9 Worked examples

#### Example 1: Bit manipulation with `u8`

```omscript
fn pack_rgb(r: int, g: int, b: int) -> int {
    let r8 = u8(r);
    let g8 = u8(g);
    let b8 = u8(b);
    return (r8 << 16) | (g8 << 8) | b8;
}

let color = pack_rgb(255, 128, 64);   // color = 0xFF8040
```

**LLVM IR** (simplified):
```llvm
define i64 @pack_rgb(i64 %r, i64 %g, i64 %b) {
  %r8 = and i64 %r, 255
  %g8 = and i64 %g, 255
  %b8 = and i64 %b, 255
  %r_shift = shl i64 %r8, 16
  %g_shift = shl i64 %g8, 8
  %tmp = or i64 %r_shift, %g_shift
  %result = or i64 %tmp, %b8
  ret i64 %result
}
```

#### Example 2: Signed vs. unsigned 32-bit

```omscript
let val = 0xFFFF_FFFF;

let unsigned = u32(val);   // unsigned = 4294967295 (0xFFFFFFFF, zero-extended)
let signed   = i32(val);   // signed   = -1 (0xFFFFFFFF, sign-extended to 0xFFFF_FFFF_FFFF_FFFF)

print(unsigned);  // 4294967295
print(signed);    // -1
```

#### Example 3: Overflow wrapping

```omscript
let big = 0x1_0000_0000;   // 2^32
let wrapped = u32(big);    // wrapped = 0 (wraps modulo 2^32)

let neg = -1;
let byte = u8(neg);        // byte = 255 (wraps: -1 mod 256 = 255)
```

#### Example 4: `bool` for conditionals

```omscript
fn is_nonzero(x: int) -> int {
    return bool(x);
}

print(is_nonzero(0));      // 0
print(is_nonzero(42));     // 1
print(is_nonzero(-5));     // 1
```


---

## 28. CF-CTRE — Cross-Function Compile-Time Reasoning Engine

### 28.1 Purpose & Pipeline Position

**CF-CTRE** (Cross-Function Compile-Time Reasoning Engine) is a deterministic interpreter embedded in the OmScript compiler that executes pure functions at compile time. It replaces the traditional constant-folding pass with a full execution engine capable of:

1. **Cross-function constant propagation**: Inline call results across function boundaries.
2. **Dead-branch elimination**: Prove if-statements always take one branch.
3. **Array constant evaluation**: Fold array operations to compile-time constants.
4. **Uniform-return detection**: Identify functions that always return the same value.
5. **Range analysis integration**: Compute `CTInterval` lattice for variables.

**Pipeline position**:
```
Parser → Type Pre-Analysis → Purity Inference → Effect Inference → Synthesis Expansion
         → CF-CTRE → Abstract Interpretation → E-Graph → Code Generation
```

CF-CTRE runs **after** purity inference (so it knows which functions to execute) and **before** e-graph (so e-graph sees folded constants).

### 28.2 Core Object Model

#### Classes (from `cfctre.h`)

| Class             | Purpose                                                                 |
|-------------------|-------------------------------------------------------------------------|
| `CTValue`         | A single compile-time value (int, float, bool, string, array handle).  |
| `CTInterval`      | Abstract domain for range analysis (lattice: BOTTOM, [lo, hi], TOP).   |
| `CTArray`         | Fixed-length array stored on compile-time heap.                         |
| `CTHeap`          | Deterministic heap for array allocation (monotone handles, `std::map`). |
| `CTFrame`         | Execution context for a single function invocation (locals, IP, control-flow signals). |
| `CTEngine`        | Main interpreter engine (memoisation cache, function registry).         |
| `CTAbstractInterpreter` | Per-function abstract interpretation using `CTInterval` lattice.  |

#### `CTValue` — Compile-Time Value

**Kind**:
```cpp
enum class CTValueKind : uint8_t {
    CONCRETE_U64,     // Unsigned 64-bit integer
    CONCRETE_I64,     // Signed 64-bit integer
    CONCRETE_F64,     // 64-bit floating point
    CONCRETE_BOOL,    // Boolean (0/1)
    CONCRETE_STRING,  // Heap-owned UTF-8 string
    CONCRETE_ARRAY,   // Array (stored in CTHeap, referenced by handle)
    UNINITIALIZED,    // Placeholder
    SYMBOLIC,         // Unknown value (for partial evaluation)
};
```

**Storage**:
- Scalars: stored inline in `union { u64, i64, f64, bool }`.
- Strings: stored in `std::string str`.
- Arrays: stored in `CTHeap`, referenced by `CTArrayHandle arr`.

**Factory methods**:
```cpp
CTValue::fromI64(42)
CTValue::fromString("hello")
CTValue::fromArray(handle)
CTValue::symbolic()   // creates a unique symbolic value for partial evaluation
```

#### `CTInterval` — Abstract Integer Lattice

**Lattice structure**:
```
                     TOP (any int64)
                      /       \
                 [lo, hi]    [lo', hi']
                      \       /
                     BOTTOM (no values)
```

**Operations**:
- **Join** (`join`): Least upper bound (union of ranges).
- **Meet** (`intersect`): Greatest lower bound (intersection of ranges).
- **Widen** (`widen`): Ensure convergence on loop back-edges.
- **Narrow** (`narrowLT`, `narrowGE`, etc.): Apply branch-condition constraints.

**Transfer functions**:
```cpp
CTInterval a = CTInterval::range(0, 10);
CTInterval b = CTInterval::range(5, 15);
CTInterval sum = a.opAdd(b);   // [5, 25]
```

### 28.3 Function Eligibility Rules

A function is eligible for CF-CTRE evaluation if:
1. **Marked pure**: No I/O, no global mutation, deterministic.
2. **All arguments are concrete or symbolic**: No uninitialized values.
3. **Recursion depth < 128**: Prevents infinite recursion.
4. **Fuel remaining**: Instruction budget not exhausted.

**Purity inference** (phase 5 of pipeline):
- Mark function `@pure` if:
  - Body contains no `print`, `input`, `file_read`, `file_write`, `sleep`, `random`, `time`.
  - All called functions are also `@pure`.
  - No global variable writes.

### 28.4 Execution Model

CF-CTRE uses a **recursive AST interpreter** with these guarantees:

1. **Deterministic**: Same inputs → same output (no randomness, no I/O).
2. **Memoised**: Cache `(fnName, args) → result` to avoid recomputation.
3. **Fuel-bounded**: Terminate after 10,000,000 instructions (configurable).
4. **Depth-bounded**: Terminate at recursion depth 128.
5. **Heap-safe**: All array allocations tracked; no leaks.

**Interpreter loop** (pseudocode):
```cpp
CTValue evalExpr(CTFrame& frame, const Expression* expr) {
    ++stats_.instructionsExecuted;
    if (fuel_-- <= 0) return CTValue::symbolic();  // out of fuel

    switch (expr->type()) {
    case INTEGER_LITERAL:
        return CTValue::fromI64(expr->intValue);
    case BINARY_EXPR:
        CTValue lhs = evalExpr(frame, expr->left);
        CTValue rhs = evalExpr(frame, expr->right);
        return evalBinaryOp(expr->op, lhs, rhs);
    case CALL_EXPR:
        std::vector<CTValue> args = evalArgs(frame, expr->args);
        return evalCall(frame, expr->fnName, args);
    // ... 30+ expression types ...
    }
}
```

**Control-flow signals**:
- `frame.hasReturned` — function returned.
- `frame.didBreak` — loop `break`.
- `frame.didContinue` — loop `continue`.

### 28.5 Instruction Semantics — CF-CTRE Operations

CF-CTRE is an **AST interpreter**, not a bytecode interpreter. It evaluates OmScript AST nodes directly. Here are the key operation categories:

#### Arithmetic Operations

| AST Node        | Operands       | Semantics                                      |
|-----------------|----------------|------------------------------------------------|
| `ADD`           | `lhs, rhs`     | `lhs + rhs` with int64/float64 semantics       |
| `SUB`           | `lhs, rhs`     | `lhs - rhs`                                    |
| `MUL`           | `lhs, rhs`     | `lhs * rhs`                                    |
| `DIV`           | `lhs, rhs`     | `lhs / rhs` (returns `symbolic()` if `rhs == 0`) |
| `MOD`           | `lhs, rhs`     | `lhs % rhs` (returns `symbolic()` if `rhs == 0`) |
| `NEG`           | `val`          | `-val`                                         |

**Overflow behavior**: Wraps modulo 2⁶⁴ (same as runtime).

#### Comparison Operations

| AST Node        | Operands       | Result                                         |
|-----------------|----------------|------------------------------------------------|
| `LT`            | `lhs, rhs`     | `CTValue::fromBool(lhs < rhs)`                 |
| `LE`            | `lhs, rhs`     | `CTValue::fromBool(lhs <= rhs)`                |
| `GT`            | `lhs, rhs`     | `CTValue::fromBool(lhs > rhs)`                 |
| `GE`            | `lhs, rhs`     | `CTValue::fromBool(lhs >= rhs)`                |
| `EQ`            | `lhs, rhs`     | `CTValue::fromBool(lhs == rhs)`                |
| `NE`            | `lhs, rhs`     | `CTValue::fromBool(lhs != rhs)`                |

**String comparison**: Lexicographic order for `<`, `<=`, etc.

#### Logical Operations

| AST Node        | Operands       | Semantics                                      |
|-----------------|----------------|------------------------------------------------|
| `LOG_AND`       | `lhs, rhs`     | Short-circuit: `lhs.isTruthy() && rhs.isTruthy()` |
| `LOG_OR`        | `lhs, rhs`     | Short-circuit: `lhs.isTruthy() || rhs.isTruthy()` |
| `LOG_NOT`       | `val`          | `!val.isTruthy()`                              |

**Truthy**: Non-zero int/float, non-empty string, valid array handle.

#### Bitwise Operations

| AST Node        | Operands       | Semantics                                      |
|-----------------|----------------|------------------------------------------------|
| `BIT_AND`       | `lhs, rhs`     | `lhs & rhs` (64-bit)                           |
| `BIT_OR`        | `lhs, rhs`     | `lhs | rhs`                                    |
| `BIT_XOR`       | `lhs, rhs`     | `lhs ^ rhs`                                    |
| `BIT_NOT`       | `val`          | `~val`                                         |
| `SHL`           | `lhs, rhs`     | `lhs << rhs` (rhs clamped to [0, 63])          |
| `SHR`           | `lhs, rhs`     | Arithmetic right shift `lhs >> rhs`            |

#### Array Operations

| AST Node           | Operation                                      |
|--------------------|------------------------------------------------|
| `ARRAY_LITERAL`    | Allocate `CTArray` on `CTHeap`, return handle  |
| `INDEX_EXPR`       | `heap_.load(arr, idx)` — bounds-checked load   |
| `INDEX_ASSIGN`     | `heap_.store(arr, idx, val)`                   |
| `ARRAY_LEN`        | `heap_.length(arr)`                            |

**Bounds checking**: Out-of-bounds access returns `CTValue::uninit()` (not an error).

#### String Operations

| AST Node           | Operation                                      |
|--------------------|------------------------------------------------|
| `STRING_LITERAL`   | `CTValue::fromString(str)`                     |
| `STR_CONCAT`       | `lhs.str + rhs.str`                            |
| `STR_LEN`          | `str.size()`                                   |
| `CHAR_AT`          | `str[idx]` (returns `uninit()` if out-of-bounds) |

#### Control-Flow Operations

| AST Node        | Semantics                                      |
|-----------------|------------------------------------------------|
| `IF_STMT`       | Evaluate condition → execute then/else branch  |
| `FOR_STMT`      | Iterate over range with symbolic loop reasoning |
| `WHILE_STMT`    | Loop until condition false (fuel-bounded)      |
| `RETURN_STMT`   | Set `frame.returnValue`, `frame.hasReturned = true` |
| `BREAK_STMT`    | Set `frame.didBreak = true`                    |
| `CONTINUE_STMT` | Set `frame.didContinue = true`                 |

### 28.6 Cross-Function Call Rules

When CF-CTRE encounters a call expression `f(a, b, c)`:

1. **Check memoisation cache**: If `(f, [a, b, c])` is cached, return cached result.
2. **Check recursion depth**: If `currentDepth_ >= 128`, return `symbolic()`.
3. **Look up function**: If `f` is not registered or not pure, return `symbolic()`.
4. **Evaluate arguments**: Recursively evaluate `a`, `b`, `c`.
5. **Execute callee**: Create new `CTFrame`, bind parameters, execute body.
6. **Memoize result**: Cache `(f, [a, b, c]) → result`.
7. **Return result**.

**Example**:
```omscript
fn square(x: int) -> int { return x * x; }
fn sum_squares(a: int, b: int) -> int { return square(a) + square(b); }

const result = comptime { sum_squares(3, 4) };   // CF-CTRE folds to 25
```

**Call graph**:
```
sum_squares(3, 4)
  ↓
  square(3) → 9   (memoized)
  square(4) → 16  (memoized)
  ↓
  9 + 16 = 25
```

### 28.7 Pipeline / SIMD-Tile Execution

When CF-CTRE encounters a `pipeline` statement:
```omscript
pipeline (0..1024 step 8) |i| {
    stage { let x = input[i]; }
    stage { let y = process(x); }
    stage { output[i] = y; }
}
```

**Execution**:
1. Partition range `[0, 1024)` into tiles of width 8.
2. Execute each tile sequentially:
   ```
   Tile [0, 8):   execute all stages for i in [0, 8)
   Tile [8, 16):  execute all stages for i in [8, 16)
   ...
   Tile [1016, 1024): execute all stages for i in [1016, 1024)
   ```
3. Last tile may be partial (mask off invalid lanes).

**Semantics**: Matches runtime SIMD execution (see Language Reference Part 2, §18 Pipeline-Parallel Execution).

### 28.8 Specialization Engine

CF-CTRE supports **partial evaluation** with symbolic arguments:

**Example**:
```omscript
fn power_of_two(n: int) -> int { return 1 << n; }

// CF-CTRE evaluates with symbolic n:
// Result: symbolic expression "1 << n" (not folded)
```

**Use case**: Generate specialized versions of functions for specific argument patterns:
```omscript
fn generic_filter(arr: int[], threshold: int) -> int[] {
    let result = [];
    for x in arr {
        if x > threshold { push(result, x); }
    }
    return result;
}

// Call site 1: threshold=0 → specialise to "keep positive"
// Call site 2: threshold=100 → specialise to "keep > 100"
```

**Specialization key**:
```cpp
std::string specializationKey(const std::string& fnName,
                               const std::vector<CTValue>& args) const {
    std::string key = fnName + "(";
    for (const auto& arg : args) {
        key += arg.memoHash() + ",";
    }
    key += ")";
    return key;
}
```

**Memoisation**: Specialized versions are cached separately.

### 28.9 Output and Integration Contract

CF-CTRE produces:

1. **Memoisation cache**: `(fnName, args) → CTValue`.
2. **Uniform-return map**: `fnName → constantValue` (for functions that always return the same constant).
3. **Dead-function set**: Functions unreachable from any entry point.
4. **Abstract interpretation results**: Range maps, safe-operation sets, dead-branch sets.

**CodeGenerator integration**:
- Query `engine_.executeFunction(fnName, args)` for call sites with constant arguments.
- Replace call with folded constant.
- Mark uniform-return functions with `!noundef` metadata.
- Skip dead-branch codegen.

### 28.10 Performance Characteristics

**Fuel and depth budgets are constants, not per-O-level knobs** (`include/cfctre.h:483-484`):

| Constant | Value | Meaning |
|----------|-------|---------|
| `kMaxInstructions` | `10,000,000` | Total instructions the abstract interpreter may execute per CF-CTRE invocation |
| `kMaxDepth` | `128` | Maximum call-frame depth (bounds recursive evaluation) |

The same budgets apply at every optimization level that runs CF-CTRE — there is **no separate "100,000-instruction" O1 mode**. If you need a tighter budget, disable CF-CTRE entirely with the corresponding `-fno-` flag rather than expecting an O-level scaler.

The fuel counter (`fuel_`) is incremented once per evaluated abstract operation. On if-statements with symbolic conditions both branches are evaluated on a saved-and-restored fuel counter, then the post-merge counter is set to `max(fuelAfterThen, fuelAfterElse)` so symbolic branching costs the worst of the two paths (`src/cfctre.cpp:2772-2796`).

**Typical compile-time cost** (illustrative, not guaranteed):
- Simple function (arithmetic): ~100 instructions.
- Loop (10 iterations): ~1,000 instructions.
- Recursive function (depth 10): ~10,000 instructions.

**Worst case**: Pathological recursion or loops exhaust fuel → CF-CTRE returns `CTValue::uninit()` and the caller falls back to runtime evaluation (no error is raised — the program still compiles, just without that fold).

### 28.11 Programmer-Visible Effects

CF-CTRE evaluation is **transparent** to the programmer:
1. **Constants are folded**: `const x = comptime { 3 + 5 }` → `x = 8`.
2. **Dead branches eliminated**: If-statements with constant conditions disappear.
3. **Array literals optimized**: `const arr = [1, 2, 3]` allocated as LLVM global.
4. **Error messages reference original source**: No "expanded" or "inlined" code in diagnostics.

**Debugging**: Use `--verbose` to see CF-CTRE statistics.

### 28.12 Worked Examples

#### Example 1: Factorial

```omscript
fn factorial(n: int) -> int {
    if n <= 1 { return 1; }
    return n * factorial(n - 1);
}

const f10 = comptime { factorial(10) };   // CF-CTRE folds to 3628800
```

**Execution trace** (abbreviated):
```
factorial(10)
  ↓
  10 * factorial(9)
    ↓
    9 * factorial(8)
      ...
        ↓
        2 * factorial(1)
          ↓
          1
  ↓
  3628800 (memoized)
```

#### Example 2: Dead-branch elimination

```omscript
const DEBUG = false;

fn process(x: int) -> int {
    if DEBUG {
        print("Debug: x = " + to_string(x));   // never executed
    }
    return x * 2;
}
```

CF-CTRE proves `DEBUG == false` → marks then-branch dead → codegen skips the branch entirely.

**Generated LLVM IR**:
```llvm
define i64 @process(i64 %x) {
  %result = mul i64 %x, 2
  ret i64 %result
}
```

#### Example 3: Array constant folding

```omscript
const squares = comptime {
    let arr = [];
    for i in 0..10 {
        push(arr, i * i);
    }
    arr
};
```

CF-CTRE executes the loop at compile time → `squares = [0, 1, 4, 9, 16, 25, 36, 49, 64, 81]`.

**Generated LLVM IR**:
```llvm
@squares = private constant [10 x i64] [
    i64 0, i64 1, i64 4, i64 9, i64 16,
    i64 25, i64 36, i64 49, i64 64, i64 81
]
```

### 28.13 Partial Evaluation with Symbolic Values

CF-CTRE supports **symbolic execution** for partial evaluation:

**Example**:
```omscript
fn double_if_positive(x: int) -> int {
    if x > 0 { return x * 2; }
    return x;
}

// CF-CTRE evaluates with symbolic x:
// Result: ternary expression "x > 0 ? x * 2 : x"
```

**Use case**: Inline functions into specialized contexts:
```omscript
fn generic_map(arr: int[], f: fn(int) -> int) -> int[] {
    let result = [];
    for x in arr {
        push(result, f(x));
    }
    return result;
}

// Call site: generic_map(data, double_if_positive)
// CF-CTRE inlines double_if_positive, folds ternary when x's range is known.
```

---

## 29. std::synthesize — Compile-Time Program Synthesis

### 29.1 Overview

`std::synthesize` is a **compile-time program synthesis engine** that enumerates candidate integer expressions, verifies them against input/output examples, and returns the best match. It is a standard-library function evaluated by CF-CTRE.

**Key properties**:
- **Enumerative search**: Breadth-first enumeration of expression trees up to depth 4 (default) or 8 (max).
- **Verification**: Candidates tested against 16 test vectors (default).
- **Cost-driven**: Selects the lowest-cost expression (instruction count + depth penalty).
- **Compile-time bounded**: Budget of 200,000 candidate evaluations (default).

### 29.2 Syntax (Verified from `synthesize.h`)

**Function signatures**:
```omscript
std::synthesize(examples)                              // default ops, depth 4
std::synthesize(examples, ops)                         // explicit ops, depth 4
std::synthesize(examples, ops, max_depth)              // explicit ops + depth
std::synthesize(examples, ops, max_depth, cost_hint)   // + "size" or "speed"
```

**Arguments**:
- `examples`: `int[][]` — Each inner array is `[in0, in1, ..., inN-1, expected_output]`.
- `ops`: `string[]` — Allowed operators (default: all integer ops).
- `max_depth`: `int` — Maximum expression-tree depth (default: 4, max: 8).
- `cost_hint`: `string` — `"size"` (minimize instruction count) or `"speed"` (default).

**Return value**:
- At runtime: Result of applying synthesized expression to first example's inputs.
- At compile time (when all inputs constant): Folded to constant.

### 29.3 Specification Language

**Operators** (from `synthesize.h`, enum `SynthOp`):

**Terminals**:
- `PARAM` — Reference to parameter index (e.g., `p0`, `p1`).
- `CONST` — Small integer constant (`-8` to `8`).

**Unary**:
- `NEG` — Negation (`-x`).
- `ABS` — Absolute value (`abs(x)`).
- `NOT` — Bitwise NOT (`~x`).

**Binary**:
- Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`.
- Bitwise: `AND`, `OR`, `XOR`, `SHL`, `SHR`.
- Power: `POW` (exponentiation, `y` clamped to [0, 30]).
- Min/Max: `MIN2`, `MAX2`.

**Example specs**:
```omscript
// Multiply-add: f(a, b, c) = a * b + c
std::synthesize([[1,2,3,5], [2,3,4,10], [0,5,1,1]])

// Absolute difference: f(a, b) = |a - b|
std::synthesize([[5,3,2], [3,5,2], [10,10,0]])

// Power of 2 test: f(x) = (x & (x-1)) == 0
std::synthesize([[1,1], [2,1], [3,0], [4,1], [5,0]], ["+","-","*","&","|"])
```

### 29.4 Search Algorithm

**Enumeration** (from `synthesize.cpp`):
```cpp
void enumerate(int depth, int nParams, const std::vector<SynthOp>& allowedOps,
               std::vector<std::unique_ptr<SynthNode>>& out) {
    if (depth == 0) {
        // Base case: terminals
        for (int i = 0; i < nParams; ++i) {
            out.push_back(std::make_unique<SynthNode>(SynthOp::PARAM, i));
        }
        for (int c = -8; c <= 8; ++c) {
            out.push_back(std::make_unique<SynthNode>(SynthOp::CONST, c));
        }
        return;
    }
    // Recursive case: combine smaller expressions
    auto subExprs = /* enumerate(depth - 1, ...) */;
    for (auto op : allowedOps) {
        if (isUnary(op)) {
            for (auto& child : subExprs) {
                out.push_back(std::make_unique<SynthNode>(op, cloneTree(child)));
            }
        } else {  // binary
            for (auto& left : subExprs) {
                for (auto& right : subExprs) {
                    out.push_back(std::make_unique<SynthNode>(op, cloneTree(left), cloneTree(right)));
                }
            }
        }
    }
}
```

**Evaluation**:
```cpp
int64_t eval(const SynthNode* node, const std::vector<int64_t>& inputs) {
    switch (node->op) {
    case SynthOp::PARAM:  return inputs[node->value];
    case SynthOp::CONST:  return node->value;
    case SynthOp::ADD:    return eval(node->left) + eval(node->right);
    case SynthOp::MUL:    return eval(node->left) * eval(node->right);
    // ... all ops ...
    }
}
```

**Verification**:
```cpp
bool verifyAll(const SynthNode* node, const std::vector<SynthExample>& examples) {
    for (const auto& ex : examples) {
        if (eval(node, ex.inputs) != ex.output) return false;
    }
    return true;
}
```

**Scoring**:
```cpp
double nodeCost(const SynthNode* node, bool preferSize) {
    int count = nodeCount(node);   // number of ops
    int depth = nodeDepth(node);   // tree depth
    double penalty = preferSize ? 1.5 : 0.2;
    return count + depth * penalty;
}
```

### 29.5 Body Replacement Pattern

When `std::synthesize` is the **sole expression** in a function's return statement, the `runSynthesisPass()` pre-codegen pass:

1. Detects the pattern: `return std__synthesize(...)`.
2. Extracts examples from the literal array argument.
3. Runs `SynthesisEngine::synthesize()`.
4. **Replaces the function body** with the synthesized expression AST.
5. Marks the function `@pure` and `@const_eval`.

**Example**:
```omscript
fn multiply_add(a: int, b: int, c: int) -> int {
    return std::synthesize([[1,2,3,5], [2,3,4,10], [0,5,1,1]]);
}
```

After `runSynthesisPass`:
```omscript
fn multiply_add(a: int, b: int, c: int) -> int {
    return a * b + c;   // synthesized
}
```

### 29.6 Restricting the Search

**Operator set**:
```omscript
std::synthesize(examples, ["+", "-", "*"])   // only add, sub, mul
```

**Depth limit**:
```omscript
std::synthesize(examples, [], 2)   // max depth 2 (fewer candidates)
```

**Cost hint**:
```omscript
std::synthesize(examples, [], 4, "size")   // prefer fewer instructions
```

### 29.7 Error Conditions

1. **No candidate found**: Warning emitted, function body unchanged.
2. **Synthesis timeout**: Budget exhausted (200,000 candidates), best-so-far returned.
3. **Invalid examples**: Mismatched input/output lengths → compile error.

### 29.8 Worked Examples

#### Example 1: Multiply-add

```omscript
fn mla(a: int, b: int, c: int) -> int {
    return std::synthesize([[1,2,3,5], [2,3,4,10], [0,5,1,1]]);
}
```

**Synthesis**:
- Enumerate: `a`, `b`, `c`, constants, `a+b`, `a*b`, `b*c`, `a*b+c`, ...
- Verify `a*b+c` against all examples → passes.
- Cost: 3 ops (mul, add, return).
- **Result**: `return a * b + c;`

#### Example 2: Absolute difference

```omscript
fn absdiff(a: int, b: int) -> int {
    return std::synthesize([[5,3,2], [3,5,2], [10,10,0]]);
}
```

**Synthesis**:
- Enumerate: `a-b`, `b-a`, `abs(a-b)`, `max(a-b, b-a)`, ...
- Verify `max(a-b, b-a)` → passes (equivalent to `abs(a-b)`).
- **Result**: `return max(a - b, b - a);`

#### Example 3: Power-of-2 test

```omscript
fn is_power_of_2(x: int) -> int {
    return std::synthesize([[1,1], [2,1], [3,0], [4,1], [5,0], [8,1], [15,0], [16,1]],
                           ["+", "-", "&"], 3);
}
```

**Synthesis**:
- Enumerate: `x&(x-1)`, `(x-1)&x`, `~(x&(x-1))`, ...
- Verify `(x & (x - 1)) == 0` → convert to `!bool(x & (x - 1))`.
- **Result**: `return (x & (x - 1)) == 0 ? 1 : 0;`

### 29.9 Implementation Notes / Limitations

**Current limitations**:
- Integer expressions only (no floats, no strings).
- Small constant pool (`-8` to `8`).
- No support for conditional expressions (ternary) in search space.
- Verification is **concrete** (test vectors), not **formal** (no proof).

**Future work**:
- Integration with Z3 SMT solver for formal verification.
- Support for floating-point synthesis.
- Larger constant pool via symbolic constants.

---

## 30. Build System and Project Layout

### 30.1 Project File Format

See §24.8 for full `oms.toml` specification. Key sections:

```toml
[project]
name = "myapp"
version = "0.1.0"

[dependencies]
http = "1.0.0"

[profile.debug]
opt_level = 0

[profile.release]
opt_level = 3
lto = true
```

### 30.2 Source Layout Conventions

**Recommended structure**:
```
myapp/
  oms.toml           # Project manifest
  src/
    main.om          # Entry point
    lib.om           # Library code
    util/
      string.om      # Utility modules
      math.om
  tests/
    test_main.om     # Test files
  om_packages/       # Downloaded dependencies (gitignored)
    http/
      http.om
```

**Entry point resolution**:
1. If `oms.toml` specifies `[project] main = "src/app.om"`, use that.
2. Otherwise, look for `src/main.om`.
3. Otherwise, use the file passed on command line.

### 30.3 Dependency Declaration

Dependencies are declared in `[dependencies]` section:
```toml
[dependencies]
http = "1.0.0"                                    # version from registry
json = { git = "https://github.com/user/json" }  # (future) git dependency
local = { path = "../local-lib" }                 # (future) local path
```

**Resolution**:
- Version dependencies fetched from `OMSC_REGISTRY_URL` (default: GitHub user-packages).
- Git dependencies cloned to `om_packages/<name>/.git`.

### 30.4 Build Graph

**Build order** (from `build_system.cpp`):
1. Parse `oms.toml`.
2. Resolve dependencies (fetch if missing).
3. Compile dependencies (depth-first).
4. Compile main source.
5. Link.

**Parallelism**: Not yet implemented (sequential builds).

### 30.5 Output Artifacts

| Artifact       | Path                        | Description                                   |
|----------------|-----------------------------|-----------------------------------------------|
| Executable     | `target/<profile>/<name>`   | Final linked binary                           |
| Object files   | `target/<profile>/<name>.o` | Intermediate object files (kept with `--emit-obj`) |
| LLVM IR        | (stdout)                    | Human-readable IR (via `emit-ir` subcommand)  |
| LLVM Bitcode   | `target/<profile>/<name>.bc` | Binary IR (with LTO)                         |

### 30.6 Package Manager Workflow

**Initialization**:
```bash
omsc pkg init   # creates oms.toml with default profile
```

**Adding dependencies**:
```bash
omsc pkg add http        # adds http = "latest" to [dependencies]
omsc pkg install         # downloads to om_packages/http/
```

**Building**:
```bash
omsc pkg build           # compiles with dependencies
```

**Listing**:
```bash
omsc pkg list            # shows installed packages
```

---

## 31. Quick-Start Cheat Sheet

### Variables
```omscript
let x = 42;               // mutable
const y = 100;            // immutable
let z: int = 0;           // explicit type (optional)
```

### Functions
```omscript
fn add(a: int, b: int) -> int {
    return a + b;
}

fn greet(name: string) {  // no return type
    print("Hello, " + name);
}
```

### Control Flow
```omscript
if x > 0 {
    print("positive");
} else if x < 0 {
    print("negative");
} else {
    print("zero");
}

let result = x > 0 ? "pos" : "neg";   // ternary
```

### Loops
```omscript
for i in 0..10 {             // range [0, 10)
    print(i);
}

for i in 0..100 step 5 {     // step size
    print(i);
}

while x > 0 {
    x = x - 1;
}
```

### Operators
```omscript
let sum = a + b;         // arithmetic
let prod = a * b;
let rem = a % b;
let pow = a ** b;        // exponentiation

let eq = a == b;         // comparison
let ne = a != b;

let and = a && b;        // logical
let or = a || b;

let band = a & b;        // bitwise
let bor = a | b;
let shl = a << 2;
```

### Arrays
```omscript
let arr = [1, 2, 3];
let first = arr[0];
arr[1] = 42;
let length = len(arr);
push(arr, 4);
let last = pop(arr);
```

### Strings
```omscript
let s = "hello";
let len = str_len(s);
let upper = str_upper(s);
let concat = s + " world";
let sub = str_substr(s, 0, 3);   // "hel"
```

### Ownership
```omscript
let r = newRegion();         // create region
let data = alloc(r, 1024);   // allocate in region
// ... use data ...
invalidate(r);               // free region
```

### Comptime
```omscript
const factorial_10 = comptime {
    fn fac(n: int) -> int {
        if n <= 1 { return 1; }
        return n * fac(n - 1);
    }
    fac(10)
};   // factorial_10 = 3628800 (compile-time constant)
```

### OPTMAX
```omscript
// Automatic: enabled at O1+
let y = x * 10 + x * 5;   // optimized to x * 15 by OPTMAX
```

### CLI
```bash
omsc build main.om                   # compile
omsc run main.om -- arg1 arg2        # compile + run
omsc build --release -o myapp        # release build
omsc build -O3 -flto -march=native   # max optimization
```

### std::synthesize
```omscript
fn multiply_add(a: int, b: int, c: int) -> int {
    return std::synthesize([[1,2,3,5], [2,3,4,10], [0,5,1,1]]);
}
// Compiler synthesizes: return a * b + c;
```

---

## 32. Glossary

| Term                  | Definition                                                                                                      |
|-----------------------|-----------------------------------------------------------------------------------------------------------------|
| **OPTMAX**            | Block-level AST optimization pass that applies strength reduction, constant folding, and dead-code elimination before IR generation. |
| **CF-CTRE**           | Cross-Function Compile-Time Reasoning Engine. Deterministic AST interpreter that executes pure functions at compile time to fold constants and eliminate dead branches. |
| **HGOE**              | Hardware Graph Optimization Engine. Models target CPU as a directed graph of execution resources and schedules instructions to minimize pipeline stalls. |
| **RLC**               | Region Lifetime Coalescing. Pass that merges disjoint region lifetimes to eliminate redundant allocations. |
| **PGO**               | Profile-Guided Optimization. Two-phase process: (1) instrument binary to collect runtime profile; (2) use profile to guide optimization decisions (inlining, block placement). |
| **SROA**              | Scalar Replacement of Aggregates. LLVM pass that splits `alloca`'d structs into individual SSA values. |
| **RecMII**            | Recurrence-constrained Minimum Initiation Interval. Lower bound on loop II imposed by carried dependencies. |
| **E-graph**           | Equivalence graph. Compact representation of many equivalent expressions via e-classes and union-find. |
| **E-class**           | Equivalence class of e-nodes representing the same value. |
| **E-node**            | A single operation (e.g., `Add`, `Mul`, `Const`) in the e-graph. |
| **Fuel**              | Instruction budget for CF-CTRE evaluation. Default: 10,000,000 instructions at O2. |
| **Comptime**          | Compile-time evaluation. Code inside `comptime { ... }` blocks is executed by CF-CTRE. |
| **Ptr-elem-type**     | Pointer element type. The type of data pointed to by a pointer (used internally by LLVM IR). |
| **Ownership state**   | State of a region variable: `created`, `invalidated`. Tracked by RLC pass. |
| **Freeze**            | Operation that converts a mutable reference to an immutable reference (future feature). |
| **Invalidate**        | Keyword that marks the end of a region's lifetime. After `invalidate(r)`, `r` cannot be used. |
| **Reborrow**          | Creating a new reference to the same data (future feature). |
| **No-alias**          | Guarantee that two pointers do not refer to overlapping memory. Enables aggressive optimization. |
| **Saturation**        | Termination condition for e-graph: no new e-nodes added in an iteration. |
| **Extraction**        | Phase of e-graph optimization where the minimum-cost term is selected from each e-class. |
| **SCoP**              | Static Control Part. Loop nest with affine bounds and subscripts, eligible for polyhedral optimization. |
| **Affine**            | Linear function of loop induction variables and constants (e.g., `2*i + j + 5`). |
| **Dependence distance vector** | Vector describing the iteration offset of a carried dependence (e.g., `(1, 0)` for loop-carried in outer loop). |
| **Fourier-Motzkin**   | Algorithm for eliminating variables from systems of linear inequalities. Used for dependence testing. |
| **SynthOp**           | Operator in the synthesis search space (e.g., `ADD`, `MUL`, `SHL`). |
| **CTValue**           | Compile-time value produced or consumed by CF-CTRE (int, float, bool, string, array). |
| **CTInterval**        | Abstract domain for range analysis. Lattice element representing a set of integer values. |
| **CTHeap**            | Compile-time heap for array allocation. Uses monotone handles and `std::map` for determinism. |
| **CTFrame**           | Execution context for a single CF-CTRE function invocation (locals, IP, control-flow signals). |
| **Symbolic value**    | Unknown value in partial evaluation (represented by `CTValue::symbolic()`). |
| **Memoisation**       | Caching `(fnName, args) → result` to avoid recomputation in CF-CTRE. |
| **Pure function**     | Function with no side effects and deterministic output. Eligible for CF-CTRE evaluation. |
| **Uniform return**    | Function that always returns the same constant value regardless of arguments. Detected by CF-CTRE. |
| **Dead branch**       | If-statement branch proven never executed. Eliminated by CF-CTRE. |
| **Safe access**       | Array access proven always in-bounds. Bounds check skipped in codegen. |
| **Safe division**     | Div/mod operation proven to have non-zero divisor. Trap omitted in codegen. |
| **Cheaper rewrite**   | Range-conditioned alternative operator (e.g., `x / 2 → x >> 1` when `x ≥ 0`). Suggested by CF-CTRE abstract interpretation. |
| **Widen**             | Lattice operation that ensures convergence on loop back-edges by extending bounds to ±∞. |
| **Narrow**            | Lattice operation that restricts a variable's range based on a branch condition (e.g., `x < 10` narrows to `[MIN, 9]`). |
| **List scheduling**   | Greedy algorithm for instruction scheduling. Selects highest-priority ready instruction at each cycle. |
| **Port contention**   | Situation where multiple instructions compete for the same execution port (e.g., two mul instructions on one MUL unit). |
| **Latency table**     | Mapping from operation types to execution latencies in cycles (used by HGOE cost model). |
| **Idiom recognition** | Superoptimizer technique: pattern-match high-level operations in low-level IR and replace with intrinsics. |
| **Enumerative synthesis** | Exhaustive search over expression trees up to a depth bound. Used by superoptimizer and `std::synthesize`. |
| **Test vector**       | Concrete input/output pair used to verify candidate expressions in synthesis. |
| **Verification**      | Process of checking that two expressions compute the same function (via test vectors or SMT solver). |
| **Cost model**        | Function that estimates the execution cost of an instruction or expression tree. |
| **Profitability**     | Criterion for applying an optimization: new code must be cheaper than old code by a threshold factor. |
| **Legality**          | Criterion for applying a transformation: semantics must be preserved (no reordering of observable effects). |
| **Pipeline stage**    | Phase of the compilation pipeline (e.g., AST_ANALYSIS, IR_MIDEND). |
| **Pass scheduler**    | System that runs optimization passes in correct order, satisfying preconditions and invalidating analyses. |
| **Fact**              | Boolean property of the program state (e.g., "purity analysis complete"). Tracked by pass scheduler. |
| **Invalidation**      | Process of marking a fact stale after a transformation changes the program state. |
| **Demand-driven**     | Scheduling strategy where passes are run only when their results are needed. |

---

## 33. Version & Compatibility

### Version

**OmScript Compiler Version**: `4.1.1`

Defined in `include/version.h`:
```cpp
#define OMSCRIPT_VERSION_MAJOR 4
#define OMSCRIPT_VERSION_MINOR 1
#define OMSCRIPT_VERSION_PATCH 1
#define OMSC_VERSION "4.1.1"
```

### Stability Statement

**Current status**: Active development. Breaking changes may occur between minor versions.

**Semantic versioning** (planned for v5.0):
- **Major**: Breaking language changes (e.g., syntax changes, removed features).
- **Minor**: New features, non-breaking additions.
- **Patch**: Bug fixes, performance improvements.

### Deprecation Notes

**Deprecated features** (to be removed in v5.0):
1. **Legacy direct mode**: `omsc file.om` (use `omsc compile file.om`).
2. **Implicit `int` return type**: Functions without `-> type` will require explicit return type.
3. **Global mutable variables**: Will require `unsafe` keyword.

**Migration guide**:
```omscript
// Old (deprecated):
fn compute(x) {     // implicit int parameter
    return x * 2;   // implicit int return
}

// New (v5.0):
fn compute(x: int) -> int {
    return x * 2;
}
```

**Compatibility**:
- Source files written for v4.x will compile with warnings in v4.1.
- v5.0 will require explicit migration (automated tool planned).

**LLVM compatibility**:
- Current: LLVM 17–21 supported.
- Future: LLVM 22+ will require updates to HGOE and superoptimizer (latency tables).

**End of Part 3**
