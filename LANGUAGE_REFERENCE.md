# OmScript Language Reference

## Table of Contents

**Part 1 — Language Core**
1. [Overview](#1-overview) — §1.1 Scope & Conformance, §1.2 Release Alignment, §1.3 How to Use, §1.4 Notation Conventions, §1.5 Design Goals, §1.6 Source of Truth, §1.7 Compilation Pipeline, §1.8 High-Level Feature Map
2. [Lexical Structure](#2-lexical-structure)
3. [Preprocessor](#3-preprocessor)
4. [Type System Overview](#4-type-system-overview) — scalar types, composite types, `ptr<T>`, `pslice<T>`, `funcptr`, `bigint`, SIMD
5. [Variables, Constants, and Comptime](#5-variables-constants-and-comptime) — `var`, `const`, `register var`, `atomic var`, `volatile var`, `global`, `comptime`, predefined constants (`INT_MAX`, `I8_MIN`, `U32_MAX`, …)
6. [Functions](#6-functions)
7. [Control Flow](#7-control-flow)
8. [Loops](#8-loops)
9. [Operators and Expressions](#9-operators-and-expressions) — arithmetic, comparison, logical, bitwise, `as` cast, `??`, `in`, range, spread, pipe `|>`, precedence table
10. [Collection Literals and Indexing](#10-collection-literals-and-indexing)

**Part 2 — Standard Library and Semantics**
11. [Arrays — Complete API](#11-arrays--complete-api)
12. [Strings — Complete API](#12-strings--complete-api)
13. [Dictionaries / Maps — Complete API](#13-dictionaries--maps--complete-api)
14. [Structs](#14-structs)
15. [Enums](#15-enums)
16. [Error Handling](#16-error-handling)
17. [Memory and Ownership System](#17-memory-and-ownership-system) — Ω Ownership spec v1.0: `move`, `borrow`, `shared`, `own`, `freeze`, `invalidate`, `ptr<T>`, `alloc<T>`, `nullptr`, `*p = v`, E015–E022, constraint matrix, `--no-ownership-checks`, `--mem-sanitize`
18. [OPTMAX](#18-optmax)
19. [Built-in Functions](#19-built-in-functions) — I/O, math, type conversions, `random`, character predicates, HTTP, `range`/`range_step`, matrix, bigint, optimizer hints
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

OmScript is a **statically typed**, compiled programming language built for native performance, predictable behavior, and practical systems programming ergonomics. It compiles through LLVM and exposes both low-level control surfaces and high-level productivity features (collections, lambdas, structured diagnostics, and keyword sugar).

Type annotations are supported across declarations and signatures, and local variable declarations may also use initializer-driven typing (for example `var score = 0;`). Composite runtime values such as arrays, maps, and strings are represented by runtime-managed heap structures accessed through typed pointers in generated IR.

### 1.1 Document Scope and Conformance

This document is the **language and tooling reference** for OmScript. It mixes:

- **Normative language rules** (syntax/semantics the compiler enforces)
- **Operational/tooling behavior** (CLI flags, diagnostics modes, optimization controls)
- **Implementation notes** (internal pipeline and optimizer architecture)

When this document uses RFC-style terms:

- **MUST / MUST NOT** = mandatory compiler/runtime behavior
- **SHOULD / SHOULD NOT** = recommended behavior or best practice
- **MAY** = optional behavior/usage pattern

### 1.2 Release Alignment and Authority

This reference is written as a **production-facing specification for the compiler snapshot in this repository**.

| Item | Value |
| --- | --- |
| Primary target | OmScript compiler version `4.9.0` |
| Authoritative implementation source | `include/version.h`, `src/`, `include/`, `examples/` |
| Intended audience | Language users, library authors, compiler contributors, and tooling integrators |
| Coverage | Surface syntax, type rules, built-ins, CLI behavior, diagnostics, optimizer controls, and selected internals |
| Authority order when docs disagree with code | Compiler behavior → validated examples/tests → this document |

**Production-readiness goal**: every user-visible language feature SHOULD be documented here with its syntax, semantics, constraints, and at least one representative example when the feature is non-trivial.

### 1.3 How to Use This Reference

- Read **§1–§10** for the core language surface.
- Read **§11–§23** for runtime semantics, the standard library, memory, concurrency, and modules.
- Read **§24–§33** for compiler operation, CLI usage, optimization controls, and implementation-facing details.
- Treat explicitly marked **Deprecated**, **Removed**, **Reserved**, and **Partially supported** notes as normative status markers, not commentary.
- When onboarding new users, prefer linking to the exact section that defines the behavior instead of duplicating the rule elsewhere.

### 1.4 Editorial and Notation Conventions

- Inline code font (e.g. `fn`, `var`, `thread_join`) names exact source syntax.
- Angle-bracket metavariables such as `<Type>` or `<Expr>` describe grammar placeholders, not literal tokens.
- Square brackets in syntax descriptions mean **optional** elements unless the brackets are shown inside a code block.
- Examples are informative unless a rule explicitly says **MUST**, **MUST NOT**, **SHOULD**, or **MAY**.
- Status markers are used consistently:
  - **Fully implemented** — supported in current compiler behavior.
  - **Partially supported** — accepted only in the documented subset; read the feature's section for the exact supported forms and limits.
  - **Deprecated** — still accepted, but emits warnings or is scheduled for removal.
  - **Removed** — no longer accepted by the compiler.
  - **Reserved** — token/word is set aside for future syntax and cannot be used normally.

### 1.5 Design Goals

- **Performance**: Native LLVM-backed compilation with optimization controls for real workloads
- **Control**: Fine-grained control over optimization strategy, memory behavior, and function-level hints
- **Safety-oriented explicitness**: Ownership/memory rules are exposed as language features instead of hidden runtime magic
- **Ergonomics**: Modern syntax with strong built-in collection/string APIs, lambdas, and concise keyword sugar
- **Compiler transparency**: Diagnostics, optimization feedback, and IR emission are first-class toolchain features

### 1.6 Source of Truth

This reference is grounded in the OmScript implementation (`src/`, `include/`) and validated examples (`examples/`). If an inconsistency is discovered, **compiler behavior is authoritative** and this document should be updated accordingly.

### 1.7 Compilation Pipeline

OmScript source code undergoes the following compilation stages:

1. **Lexer** — Tokenizes source text into a stream of lexical tokens (keywords, identifiers, literals, operators, punctuation)
2. **Parser** — Constructs an Abstract Syntax Tree (AST) from the token stream, enforcing syntactic rules; evaluates `comptime {}` blocks for conditional compilation and constant injection
3. **Semantic Analysis** — Validates type consistency, resolves identifiers, enforces declaration typing rules, and checks control-flow constraints
4. **Code Generation** — Traverses the AST to emit LLVM IR, applying function annotations and optimization hints
5. **Optimization Passes** — LLVM's optimization pipeline transforms the IR (inlining, vectorization, constant folding, dead-code elimination, loop transformations)
6. **Object Code Emission** — LLVM backend generates native machine code for the target architecture
7. **Linking** — Links object files with the OmScript runtime library to produce the final executable

### 1.8 High-Level Feature Map

- **Lexical structure**: Keywords, identifiers, literals (integer, float, string, bytes, interpolated), operators, comments (§2)
- **Conditional compilation**: `comptime {}` blocks, `comptime if COND {}` shorthand, built-in constants `OS`/`ARCH`/`VERSION`/`FILE`, `-D NAME[=VALUE]` CLI flags, `defined(NAME)` predicate (§3, §5.9)
- **Type system**: Scalar types (signed/unsigned integers, floats, bool, string), composite types (arrays, dicts, structs, enums, pointers, SIMD vectors, bigint), declaration typing rules (§4)
- **Variables and constants**: `var`, `const`, `register var`, `atomic var`, `volatile var`, `global`, `comptime`, compound assignment, destructuring (§5)
- **Functions**: Declaration syntax, parameters, return types, default parameters, expression-body functions, annotations (`@opt(hot)`, `@semantics(pure)`, `@memory(allocator)`, etc.), tail calls, lambdas; generic type parameters are reserved for a future version (§6)
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
| --- | --- |
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
| --- | --- |
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

**Threading:**
| Keyword | Purpose |
| --- | --- |
| `spawn` | Sugar for `thread_create(...)` |
| `join` | Sugar for `thread_join(...)` |
| `detach` | Sugar for `thread_detach(...)` |
| `lock` | Sugar for `mutex_lock(...)` |
| `unlock` | Sugar for `mutex_unlock(...)` |
| `trylock` | Sugar for `mutex_try_lock(...)` |

**Declarations:**
| Keyword | Purpose |
| --- | --- |
| `fn` | Function declaration |
| `var` | Mutable variable |
| `const` | Immutable constant |
| `register` | Register-allocation hint |
| `atomic` | Atomic load/store/RMW qualifier |
| `volatile` | Volatile (non-optimizable) load/store qualifier |
| `global` | Global variable scope |
| `struct` | Structure type |
| `enum` | Enumeration type |

**Exception handling:**
| Keyword | Purpose |
| --- | --- |
| `catch` | Top-level handler block: `catch(N) { ... }` (see §16) |
| `throw` | Raise an integer error code: `throw 42;` (see §16) |
| `try` | **Reserved** for future use — not currently a parser keyword (no `try { }` block exists in the language) |

**Ownership and memory:**
| Keyword | Purpose |
| --- | --- |
| `move` | Transfer ownership |
| `borrow` | Borrow reference |
| `reborrow` | Re-borrow from existing borrow |
| `mut` | Mutable borrow annotation |
| `invalidate` | Explicit invalidation (schedule deferred free) |
| `freeze` | Mark variable permanently read-only |
| `shared` | Transition variable to shared (read-only aliasable) ownership (Ω spec §3.1) |
| `own` | Restore unique ownership from shared state (Ω spec §3.1) |
| `construct` | In-place field initialisation of a `ptr<T>` (see §17.9.2a) |
| `jmp` | ⚠ **Deprecated** — unconditional jump to a named label (see §7.11) |
| `label` | Named jump target declaration for `jmp` (see §7.11) |

**Literals:**
| Keyword | Purpose |
| --- | --- |
| `true` | Boolean true |
| `false` | Boolean false |
| `null` | Null pointer literal (zero address) |
| `nullptr` | Alias for `null` — null pointer literal (Ω spec §2.2) |

**Operators and punctuation:**
| Keyword | Purpose |
| --- | --- |
| `in` | Membership test / for-loop iterator |
| `return` | Function return |

**Compiler hints:**
| Keyword | Purpose |
| --- | --- |
| `prefetch` | Memory prefetch hint |
| `likely` | Branch likely hint |
| `unlikely` | Branch unlikely hint |
| `comptime` | Compile-time evaluation |

**Special constructs:**
| Keyword | Purpose |
| --- | --- |
| `defer` | Execute at scope exit (LIFO) |
| `with` | Scoped variable binding |
| `import` | File inclusion |
| `swap` | Swap two variables |
| `pipeline` | Staged execution pipeline |
| `stage` | Named pipeline stage |

**Optimization markers:**
| Token | Purpose |
| --- | --- |
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

OmScript supports **character literals** using single quotes. The result type is `i32` (a Unicode code point).

**Syntax:**
```omscript
var c: int = 'A';        // 65
var nl: int = '\n';      // 10 (newline)
var tab: int = '\t';     // 9
var bs: int = '\\';      // 92 (backslash)
var sq: int = '\'';      // 39 (single quote)
var nul: int = '\0';     // 0 (null code point — valid in char literals only)
var u: int = '\u0041';   // 65 (Unicode scalar: 'A')
var U: int = '\U00000041'; // 65 (long Unicode scalar form)
```

**Rules:**
- Exactly one character (or one escape sequence) between the single quotes
- Supported escapes: `\n`, `\t`, `\r`, `\b`, `\f`, `\v`, `\\`, `\'`, `\0`, `\xHH` (two hex digits), `\uHHHH` (four hex digits), `\UHHHHHHHH` (eight hex digits)
- This escape list applies to **character literals**; string literals use their own table below and reject null-byte escapes.
- Yields a compile-time integer constant of type `i32` (the Unicode code point)
- Character literals may be compared with integers directly: `c == 65` or `c == 'A'`

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
| --- | --- | --- |
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

**Alternative prefix `f"..."`**: The `f"..."` prefix is an alias for `$"..."` and provides the same interpolation semantics. Both forms are accepted by the lexer.

```omscript
var n: int = 42;
var s1: string = $"value is {n}";   // $"..." form
var s2: string = f"value is {n}";   // f"..." alias — identical result
```

**Nested interpolations**: Not supported directly; use intermediate variables.

#### 2.5.8 Raw String Literals

A **raw string literal** is prefixed with `r` and disables all escape-sequence processing — backslashes are taken verbatim.

**Single-line form**: `r"..."` — backslashes are literal, no escape sequences are recognised.

```omscript
var pat: string = r"\d+\.\d+";   // two-character sequences \d, \., etc. preserved
var path: string = r"C:\Users\Alice\Documents";
println(r"\n");  // prints the two characters \ and n, not a newline
```

**Multi-line form**: `r"""..."""` — spans multiple lines with no escape interpretation.

```omscript
var query: string = r"""
SELECT *
FROM users
WHERE path LIKE 'C:\Users\%'
""";
```

**Rules**:
- The `r` prefix is part of the lexeme, not the string value.
- Mixing with `$"..."` / `f"..."` is not supported — raw strings cannot be interpolated.
- The null-byte restriction applies as in regular strings (raw strings cannot contain a literal NUL byte).
- Otherwise, all characters including `\` and `"` (except the closing `"` / `"""`) are passed through unchanged.

#### 2.5.9 Null Literal

The **null literal** is the keyword `null`.

```omscript
var p: ptr = null;  // Null pointer
```

Type: Context-dependent (typically pointer types). The semantics of `null` depend on the type system and are implementation-defined.

### 2.6 Operators and Punctuation

The following tokens represent operators and punctuation. They are listed with their symbolic forms and semantic categories (precise precedence and semantics in §9).

**Arithmetic operators:**
| Token | Symbol | Name |
| --- | --- | --- |
| `PLUS` | `+` | Addition |
| `MINUS` | `-` | Subtraction / Unary negation |
| `STAR` | `*` | Multiplication |
| `STAR_STAR` | `**` | Exponentiation |
| `SLASH` | `/` | Division |
| `PERCENT` | `%` | Modulus |

**Comparison operators:**
| Token | Symbol | Name |
| --- | --- | --- |
| `EQ` | `==` | Equality |
| `NE` | `!=` | Inequality |
| `LT` | `<` | Less than |
| `LE` | `<=` | Less than or equal |
| `GT` | `>` | Greater than |
| `GE` | `>=` | Greater than or equal |

**Logical operators:**
| Token | Symbol | Name |
| --- | --- | --- |
| `AND` | `&&` | Logical AND (short-circuit) |
| `OR` | `||` | Logical OR (short-circuit) |
| `NOT` | `!` | Logical NOT |

**Bitwise operators:**
| Token | Symbol | Name |
| --- | --- | --- |
| `AMPERSAND` | `&` | Bitwise AND / Address-of |
| `PIPE` | `|` | Bitwise OR |
| `CARET` | `^` | Bitwise XOR |
| `TILDE` | `~` | Bitwise NOT |
| `LSHIFT` | `<<` | Left shift |
| `RSHIFT` | `>>` | Right shift (always logical / unsigned-fill; sign bit is NOT propagated) |

**Assignment operators:**
| Token | Symbol | Name |
| --- | --- | --- |
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
| --- | --- | --- |
| `PLUSPLUS` | `++` | Increment (prefix/postfix) |
| `MINUSMINUS` | `--` | Decrement (prefix/postfix) |

**Special operators:**
| Token | Symbol | Name |
| --- | --- | --- |
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
| --- | --- | --- |
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
| --- | --- | --- |
| `downto` | Second token of a `for (i in HI downto LO ...)` range loop (§8.5) | Plain identifier |
| `step` | Optional trailing modifier of a `for (i in A...B step N)` loop (§8.5) | Plain identifier |
| `deopt` | Optional marker in `assume(c) else deopt { ... }` (§7.10) | Plain identifier |
| `as` | Import alias (`import "x.om" as foo`) and explicit casts | Plain identifier |
| `from` | Inside specific destructuring forms | Plain identifier |

These do not appear in the keyword table in §2.4 and you may shadow them, but doing so usually hurts readability. Treat them as reserved in idiomatic code.

---

## 3. Preprocessor

> **Status:** Removed. Migrate all legacy preprocessor usage to `comptime {}` blocks.

The OmScript preprocessor has been **removed**. Preprocessor directives (`#define`, `#ifdef`, `#if`, `#include`, etc.) are no longer supported and will produce a compile-time error if present in source code.

**Migration:** Replace all preprocessor usage with `comptime {}` blocks (§5.9). A complete migration table is provided in §5.9.1.

| C/C++ preprocessor | OmScript `comptime {}` equivalent |
| --- | --- |
| `#define MAX 1024` | `comptime { const MAX: int = 1024; }` |
| `#define PI 3.14` | `comptime { const PI: float = 3.14; }` |
| `#define NAME "hi"` | n/a — use a local `var` or string literal directly |
| `#define DEBUG` | `comptime { const DEBUG: bool = true; }` |
| `#ifdef DEBUG` / `#endif` | `comptime { if (defined(DEBUG)) { ... } }` |
| `#ifndef NDEBUG` | `comptime { if (!defined(NDEBUG)) { ... } }` |
| `#if OS == "linux"` | `comptime { if (OS == "linux") { ... } }` |
| `#if X == 0 && Y != 0` | `comptime { if (X == 0 && Y != 0) { ... } }` |
| `#error "message"` | `comptime { error("message"); }` |
| `#warning "message"` | `comptime { warning("message"); }` |
| `__OS__` | built-in `OS` comptime string: `"linux"` / `"windows"` / `"macos"` |
| `__ARCH__` | built-in `ARCH` comptime string: `"x86_64"` / `"aarch64"` |
| `__VERSION__` | built-in `VERSION` comptime string |
| `__FILE__` | built-in `FILE` comptime string (source file path) |
| `#define MAX(a,b) ...` | `fn max(a: int, b: int) -> int { ... }` — typed function |

For a full migration guide and examples, see **§5.9.1** (`comptime {}` blocks).

## 4. Type System Overview

OmScript is **statically typed**. Variable declarations may be typed explicitly (`var x: int = 1;`) or inferred from an initializer (`var x = 1;`). The type system includes scalar types (integers, floats, booleans, strings), composite types (arrays, dictionaries, structs, enums), pointer types, SIMD vectors, and arbitrary-precision integers (bigint).

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

### 4.2 Declaration Typing Rules

Variable declarations follow this rule:

- A declaration MUST include either an explicit type annotation or an initializer expression.
- A bare declaration with neither type nor initializer is invalid.

**Valid examples**:
```omscript
var x: int = 10;
const pi: float = 3.14;
var y = 20;
const name = "Alice";
```

**Invalid example** (causes a parse error):
```omscript
var y;  // Error: requires type annotation or initializer
```

**Additional notes**:
- In multi-variable declarations, later entries may inherit type from the first declaration.
- Inside OPTMAX-restricted contexts, explicit annotations are required by parser rules.

### 4.3 Scalar Types

#### Integer Types

OmScript provides signed and unsigned integer types of various widths:

| Type | Width | Signedness | Range | LLVM Type |
| --- | --- | --- | --- | --- |
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
- Right-shift `>>` is always **logical** (zero-filling) regardless of signedness. OmScript does not have an arithmetic right-shift operator; use `i32(x) >> n` with explicit sign-extension casts if arithmetic shift semantics are needed.

**Overflow behavior**: Integer overflow is **undefined behavior** for signed integers (enabling optimizations); unsigned integers wrap modulo 2ⁿ.

#### Floating-Point Types

| Type | Width | Precision | Range | LLVM Type |
| --- | --- | --- | --- | --- |
| `f32` | 32 bits | Single (IEEE 754) | ≈1.2e-38 to ≈3.4e38 | `float` |
| `f64` | 64 bits | Double (IEEE 754) | ≈2.2e-308 to ≈1.8e308 | `double` |
| `float` | 64 bits | Alias for `f64` | Same as `f64` | `double` |
| `double` | 64 bits | Alias for `f64` | Same as `f64` | `double` |

**Default type**: Float literals default to `f64`.

**NaN and infinity**: IEEE 754 special values (NaN, ±∞) are supported. Use `is_nan(x)` and `is_inf(x)` predicates to test.

**Fast math**: Functions annotated with `@fastmath` may reorder floating-point operations, enable algebraic reassociation, and assume no NaN/Inf values (trading precision for speed).

#### Boolean Type

| Type | Width | Values | LLVM Type |
| --- | --- | --- | --- |
| `bool` | 1 bit (logical) | `true`, `false` | `i1` |

**Memory representation**: `bool` occupies 1 byte in memory (for alignment) but is represented as `i1` in LLVM (1-bit integer).

**Conversion**: Integer-to-bool: `0` → `false`, non-zero → `true`. Bool-to-integer: `false` → `0`, `true` → `1`.

#### String Type

| Type | Representation | Encoding | LLVM Type |
| --- | --- | --- | --- |
| `string` | Heap-allocated, immutable | UTF-8 | `ptr` (opaque) |

**Semantics**:
- Strings are immutable heap objects (reference-counted in the runtime)
- Indexing `s[i]` returns an integer ASCII/UTF-8 byte value
- Concatenation `s1 + s2` creates a new string
- String length `len(s)` returns the byte count (not character count if multibyte UTF-8)
- Empty string `""` is a valid string

**String interning**: Compile-time string literals may be interned (implementation detail).

#### Character and Byte Types

OmScript provides a `byte` type as an alias for `u8` (unsigned 8-bit integer, range 0–255).

| Type | Width | Signedness | Range | LLVM Type | Notes |
| --- | --- | --- | --- | --- | --- |
| `byte` | 8 bits | Unsigned | 0 to 255 | `i8` | Alias for `u8` |

**Array of bytes**: `byte[]` denotes an array of byte values (same representation as `u8[]`).

```omscript
var b: byte = 0xFF;          // single byte
var buf: byte[] = [1, 2, 3]; // byte array literal
```

**Casting to `byte` and `byte[]`**:

- `expr as byte` — truncate any integer/float/pointer to a single byte (0–255).
- `expr as byte[]` — extract the raw little-endian bytes of a value:
  - **Integer**: `numBytes = bitWidth / 8` elements (e.g. `i64` → 8, `i128` → 16, `i256` → 32)
  - **Float**: bitcast to same-width integer, then extract bytes (`f32` → 4, `f64` → 8)
  - **Pointer**: 8 bytes (little-endian 64-bit address)
  - **String variable**: one element per UTF-8 byte of the string's character data

```omscript
var n: i64 = 0x0102030405060708;
var bytes: byte[] = n as byte[];
// bytes[0] = 8, bytes[1] = 7, ... bytes[7] = 1  (little-endian)

var x: f64 = 1.0;
var raw: byte[] = x as byte[];  // 8 bytes — IEEE 754 representation

var s: str = "Hi";
var sb: byte[] = s as byte[];   // [72, 105]  ('H', 'i')

var big: i128 = 1 as i128;
var bb: byte[] = big as byte[];  // 16 bytes; bb[0]=1, bb[1..15]=0
```

**`sizeof(byte)`** → 1

Characters are also represented as:
- Single-character strings: `"A"`
- Integer ASCII/UTF-8 codes: `65`

The `char_at(s, i)` function returns the integer ASCII code of the character at position `i` in string `s`. The `char_code(s)` function returns the integer code of the first byte of `s`. Use `to_char(code)` to convert an integer code back to a single-character string.

#### Void Type

| Type | Meaning | LLVM Type |
| --- | --- | --- |
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
- `dict[K, V]` (typed dictionary annotation — parsed but keys and values are stored as `i64`; the type parameters are informational only)

**Representation**: Hash map (heap-allocated, reference-counted).

**LLVM type**: Opaque pointer `ptr` to runtime map object.

**Operations**:
- Creation: `map_new()` or literal `{ k1: v1, k2: v2, ... }`
- Access: `map_get(d, key, default)` or subscript `d[key]` (returns `0` for missing keys)
- Mutation: `map_set(d, key, value)` (returns new map) or `d[key] = value` (in-place)
- Check: `map_has(d, key)` (returns 1 or 0)
- Removal: `map_remove(d, key)` (returns new map)
- Size: `map_size(d)`
- Keys: `map_keys(d)` (returns array of keys)
- Values: `map_values(d)` (returns array of values)

**Example**:
```omscript
var ages: dict = { "Alice": 30, "Bob": 25 };
var alice_age: int = map_get(ages, "Alice", 0);  // 30
var same: int = ages["Alice"];                   // 30 — subscript syntax
ages = map_set(ages, "Charlie", 35);             // functional style
ages["Dave"] = 28;                               // subscript assignment
var has_bob: int = map_has(ages, "Bob");         // 1
```

#### 4.4.3 SIMD Vector Types: `f32x4`, `i32x8`, `i16x8`, …

OmScript supports first-class fixed-width SIMD vector types as variable
annotations. The compiler lowers them to LLVM `<N x T>` vectors so the
backend's vectorizer and the host's native SIMD instructions are used
directly.

**Naming**: `<elemType>x<lanes>`, where `elemType` is `i8`, `u8`, `i16`,
`u16`, `i32`, `u32`, `i64`, `u64`, `f32`, or `f64` and `lanes` matches a
natural SSE / AVX / AVX-512 width:

| Lane width | SSE2 (128-bit) | AVX2 (256-bit) | AVX-512 (512-bit) |
| --- | --- | --- | --- |
| 8-bit  | `i8x16`, `u8x16`     | `i8x32`, `u8x32`        | — |
| 16-bit | `i16x8`, `u16x8`     | `i16x16`, `u16x16`      | — |
| 32-bit | `i32x4`, `u32x4`, `f32x4` | `i32x8`, `u32x8`, `f32x8` | `i32x16`, `u32x16`, `f32x16` |
| 64-bit | `i64x2`, `u64x2`, `f64x2` | `i64x4`, `u64x4`, `f64x4` | `i64x8`, `u64x8`, `f64x8` |

Signed and unsigned integer types share the same LLVM vector
representation; signedness is carried via the annotation and per-instruction
opcodes (`SDiv` vs `UDiv`, etc.).

**Operations**:
- Construction from an array literal of the matching length:
  `var v: f32x4 = [1.0, 2.0, 3.0, 4.0];`
- Element-wise arithmetic: `+`, `-`, `*`, `/`, `%`
- Element-wise bitwise (integer vectors only): `&`, `|`, `^`, `<<`, `>>`
- Lane read: `v[i]` (lifts narrow elements to the default expression width)
- Lane write: `v[i] = x`

**Pick a width portably** with the `__VECTOR_WIDTH__` predefined macro and
the `__SIMD_*__` feature macros — see §3.3.

**Example**:
```omscript
fn main() -> int {
    var a: i32x8 = [1, 2, 3, 4, 5, 6, 7, 8];
    var b: i32x8 = [1, 1, 1, 1, 1, 1, 1, 1];
    var c: i32x8 = a + b;       // element-wise add
    return c[2] + c[7];         // 4 + 9 = 13
}
```

#### 4.4.4 Pointer Type: `ptr` / `ptr<T>`

**Syntax**:
- `ptr` — Generic pointer (element type unknown, treated as opaque)
- `ptr<T>` — Typed pointer to elements of type `T`

**Representation**: Raw memory address (64-bit on all supported architectures). No hidden metadata.

**LLVM type**: `ptr` (LLVM opaque pointer)

**Operations**:
- **Address-of**: `&x` — produces `ptr<T>` when `x` has type `T`
- **Dereference read**: `*p` — loads the value pointed to by `p` using the element type of `ptr<T>`
- **Dereference write**: `*p = v` — stores `v` through pointer `p` (Ω spec §4.2)
- **Pointer arithmetic**: `p + n`, `p - n` — advances by `n * sizeof(T)` bytes (Ω spec §4.4)
- **Null literal**: `null` or `nullptr` (both are zero address, Ω spec §2.2)
- **Allocation**: `alloc<T>(n)` or `new T(n)` — allocate `n` elements of type `T`; `alloc<T>()` / `new T` (no parens) allocates exactly 1 element (Ω spec §4.1)
- **Boxing**: `store_ptr(value)` — allocate a stack slot, store `value` into it, return a `ptr<typeof(value)>`. The type of the returned pointer matches the expression type (i64, ptr, etc.). The slot is placed in the function entry block so `mem2reg`/SROA can promote it.
- **Deallocation**: `invalidate p` — free the heap allocation and mark `p` dead

**Valid initializers for `ptr`-typed variables**: Any expression that could produce a pointer at runtime is accepted: `&var`, function calls (builtin or user-defined), `null`/`nullptr`, pointer arithmetic (`p + n`), identifiers, or any complex expression. The only rejected initializers are non-zero integer literals, float literals, and string literals (e.g. `var p: ptr = 42` is an error).

**Safety**: In safe mode (default), the borrow checker enforces:
- No use-after-invalidate
- No double-invalidate (E019)
- No write to `shared` pointer (E020)
- Null dereference paths detected by `--mem-sanitize`

Use `--no-ownership-checks` for raw C-like pointer semantics (unsafe mode).

**Example**:
```omscript
fn main() -> int {
    var x: i64 = 42;
    var p: ptr<i64> = &x;             // address-of: stack pointer
    var q: ptr<i64> = alloc<i64>(4);  // allocate 4 i64 elements (stack, T1)
    var r: ptr<i64> = new i64(4);     // identical to alloc<i64>(4)

    // Typed dereference write and read
    *q = 10;
    *(q + 1) = 20;
    var sum = *q + *(q + 1);   // 30

    invalidate q;  // free heap allocation
    return sum;    // 30
}
```

**Boxing with `store_ptr`**:
```omscript
fn main() -> int {
    // Box an integer literal onto the stack and get a pointer to it
    var p: ptr<i64> = store_ptr(99);
    println(*p);   // 99

    // Write through the boxed pointer
    *p = 200;
    println(*p);   // 200

    // Box the value of any expression
    var a: i64 = 10;
    var b: ptr<i64> = store_ptr(a * 3 + 5);
    println(*b);   // 35

    return 0;
}
```

**Null pointer**:
```omscript
var p: ptr<i64> = null;    // zero pointer
var q: ptr<i64> = nullptr; // identical to null (Ω spec §2.2)
if (p == q) { println("both null"); }
```

#### 4.4.5 Reference Type: `ref` / `&T`

**Status**: Reference types are partially supported as **borrow-variable annotations**. A variable declared `borrow [mut] var r:&T = &x;` is a true pointer-backed alias for `x`: reads of `r` auto-deref to `T`, and writes through a `borrow mut` reference write back to `*ptr` (so the source variable observes the update). Reference types as struct fields, function parameters, return types, or in expression position are not yet formalized.

**Syntax**: `&T` denotes a borrowed reference to `T`. The initializer must be an `&<lvalue>` expression (e.g. `&x`, not just `x`).

**Semantics**:
- The alloca for `r` holds a pointer (`ptr` in LLVM IR), not a `T`.
- Reading `r` emits `load ptr` then `load T` (auto-deref); narrow integer types are sign- or zero-extended to the default expression width based on the `i*`/`u*` annotation.
- Writing `r = v` (only legal when declared `borrow mut`) coerces `v` to `T` and stores through the held pointer. Writing through an immutable `&T` is a compile-time error.
- Borrow-checker rules are unchanged: while `r` is alive, the source `x` cannot be moved, mut-borrowed (via another mut alias), or — for a `borrow mut` — read.

**Example**:
```omscript
fn main() -> int {
    var x:i64 = 4;
    {
        borrow mut r:&i64 = &x;
        r = 40;          // write-through: x is now 40
    }
    return x + 2;        // 42
}
```

**Out of scope (future work)**: `&T` to struct fields or array elements (use `reborrow` today), `&T` parameters / return types, comparisons of references, multi-level `&&T`.

#### 4.4.6 Struct Type

**Syntax**: `struct Name { field1, field2, ... }`

**Declaration**:
```omscript
struct Point {
    x,
    y
}
```

**Fields** must have explicit type annotations when the struct is declared with typed fields. For structs declared without per-field annotations (legacy form), fields default to `i64` and type information is inferred from usage.

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

#### `@repr(...)` — Struct Layout Control

Placed immediately before `struct`, `@repr(...)` controls the memory layout of the struct's fields:

| Annotation | Effect |
| --- | --- |
| `@repr(C)` | Stable, ABI-compatible C layout — fields in declaration order with natural alignment. Use for FFI interoperability. |
| `@repr(packed)` | Minimal memory — no padding bytes inserted between fields. |
| `@repr(align(N))` | Force the struct's alloca to be at least `N`-byte aligned (N must be a power of two). |
| `@repr(auto)` | Compiler optimizes layout freely (default). |
| `@repr(soa)` | Structure-of-arrays layout hint — **Reserved**: recorded in AST but not yet applied by any layout pass. Has no effect on the current compiler output. |

```omscript
@repr(C)
struct FFIPoint {
    x: i32,
    y: i32,
}

@repr(packed)
struct Wire {
    header: i8,
    payload: i32,
}

@repr(align(64))
struct CacheLine {
    value: int,
}

@repr(auto)
struct AutoLayout {
    a: int,
    b: int,
}

@repr(soa)
struct Particle {
    px: float,
    py: float,
    pz: float,
}
```

**Codegen details**:
- `@repr(packed)` → `isPacked = true` on the LLVM StructType (no inter-field padding)
- `@repr(C)` → fields in declaration order; `alloca` aligned to the target ABI alignment of the struct type
- `@repr(align(N))` → `alloca` alignment set to `N` bytes
- `@repr(auto)` → same as the default (compiler may reorder cold fields for locality at O2+)
- `@repr(soa)` → hint recorded in AST; no current effect — SoA layout transformation is not yet implemented

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

#### 4.4.9 Slice Pointer Type: `pslice<T>`

**Syntax**: `pslice<T>` where `T` is an element type (e.g., `i64`, `f64`).

**Representation**: A *fat pointer* — two machine words stored in two separate allocas:
- **ptr alloca**: holds the raw `ptr` to the data buffer (element type `T`).
- **len alloca**: holds an `i64` with the number of elements.

Unlike a plain `ptr<T>`, a `pslice<T>` carries its length alongside the pointer so that bounds checking and iteration do not require a separate length variable.

**LLVM type**: The ptr alloca is `ptr`; the len alloca is `i64`. There is no single LLVM struct — the compiler tracks the two allocas via the `psliceVarNames_` / `psliceLenAllocas_` internal tables.

**Operations**:
- **Creation**: `pslice_new<T>(rawPtr, count)` — wraps an existing raw pointer and a length into a `pslice<T>`.
- **Length query**: `pslice_len(s)` — returns the number of elements (i64).
- **Pointer extraction**: `pslice_ptr(s)` — returns the underlying raw pointer (ptr).
- **Indexing**: `s[i]` — compile-time bounds-check if both `s` and `i` are constants; otherwise emits a runtime `abort()` guard before the load.

**Example**:
```omscript
fn main() -> int {
    var buf: ptr<i64> = alloc<i64>(4);
    *buf = 10;
    *(buf + 1) = 20;
    *(buf + 2) = 30;
    *(buf + 3) = 40;

    var s: pslice<i64> = pslice_new<i64>(buf, 4);
    println(pslice_len(s));  // 4
    println(s[0]);           // 10
    println(s[2]);           // 30
    return 0;
}
```

**Restrictions**:
- `pslice<T>` variables cannot be passed directly across user-defined function boundaries (there is no single LLVM type to pass them as). Use `pslice_ptr` + `pslice_len` to unpack before calling.
- `pslice_new<T>` requires the explicit type parameter — `pslice_new(ptr, len)` (without `<T>`) is a parse error.

#### 4.4.10 Executable Pointer Type: `funcptr`

**Syntax**: `funcptr` (no type parameter — always represents a callable native function pointer).

**Representation**: An opaque `ptr` to executable machine code. Internally the compiler tracks `funcptr` variables in a separate `funcptrVarNames_` set so that the dereference operator `*f` is dispatched as a call rather than a memory load.

**LLVM type**: `ptr` (same as other pointer types; the distinction is in how the compiler handles `*f`).

**Operations**:
- **Obtain from a named function**: `funcptr_from("name")` — resolves the OmScript/C function named `name` and returns its address as a `funcptr`. The argument must be a string literal.
- **Create from machine-code bytes**: `funcptr_new(byteArray, n)` — allocates a W^X (write-then-execute) page of executable memory, copies `n` bytes from the `i64[]` byte array into it, and returns a `funcptr` to the executable copy.
- **Call**: `*f` — calls the function pointer as `fn() -> i64` with no arguments. The result is `i64`.

**Valid initializers** (accepted in `var f: funcptr = …`):
- `funcptr_from("name")` — function address
- `funcptr_new(arr, n)` — JIT-compiled bytes
- Another `funcptr` variable

**Example — wrapping an existing function**:
```omscript
fn double(x: int) -> int { return x * 2; }

fn main() -> int {
    var f: funcptr = funcptr_from("double");
    // Calls double() via the native pointer (no arguments passed —
    // funcptr call is fn()->i64; pass data via globals for full JIT use).
    var result: int = *f;
    return 0;
}
```

**Example — JIT from raw bytes (x86-64)**:
```omscript
fn main() -> int {
    // x86-64: mov rax, 42 ; ret
    var code: int[] = [0xB8, 42, 0, 0, 0, 0xC3];
    var f: funcptr = funcptr_new(code, 6);
    var result: int = *f;   // Calls the JIT'd bytes → 42
    println(result);         // 42
    return 0;
}
```

**Platform notes**:
- `funcptr_new` requires the process to map a page with both `PROT_WRITE` and `PROT_EXEC` (W^X; two mmap calls). This may be blocked by system policies (e.g., SELinux, `sysctl vm.mmap_min_addr`).
- `funcptr_from` uses LLVM's function address — works for any function visible at link time.
- The `funcptr` type is only available in OmScript; it has no automatic C FFI bridge.

### 4.5 Type Inference

OmScript supports **limited type inference**:

**Where inference is allowed**:
- Function parameter types may be omitted if unused in the function body (rare; not recommended)
- Lambda parameters: types are inferred from context (e.g., `|x| x * 2` infers `x` from usage)
- Expression types: intermediate expression types are inferred (e.g., `var x: int = 1 + 2;` infers `1 + 2` as `int`)

**Where inference is forbidden**:
- Variable declarations without initializers: **explicit type annotation required** (a bare `var x;` with no type and no initializer is an error)
- Function return types: **explicit annotation recommended** (omission may infer `void`)

**Type propagation in multi-variable declarations**: In multi-variable declarations, the type annotation on the first variable propagates to subsequent variables without explicit annotation (see §5.3).

---

### 4.6 Type Aliases

Declare a name as an alias for an existing type with the `type` keyword. Aliases are resolved transitively and fully at compile time — they have zero overhead and may be used anywhere a type annotation is accepted (variable declarations, struct fields, function parameters, return types, `sizeof()`).

**Syntax:**
```omscript
type AliasName = ExistingType;
```

**Rules:**
- Aliases are module-scoped (top-level only).
- Aliases are resolved transitively: if `type A = B` and `type B = int`, then using `A` is exactly the same as using `int`.
- Up to 32 hops are followed; self-referential or cyclic aliases are silently capped at the current value.
- Aliases for SIMD vector types may use comptime-sized lane counts: `type V8 = u64x{8};`.

**Example:**
```omscript
import std;

type Score  = int;
type Weight = f64;
type Name   = string;

// Transitive alias — Alias → Middle → int
type Middle = int;
type Alias  = Middle;

struct Player {
    name:  Name,
    score: Score,
}

fn rank(a: Score, b: Score) -> Score {
    return a - b;
}

fn main() {
    var sc: Score  = 100;
    var wt: Weight = 2.5;

    // Aliases resolve identically to the underlying type.
    var p: Player = Player { name: "alice", score: sc };
    var al: Alias = 42;     // same as int

    println(sc + 1);        // 101
    println(al * 2);        // 84
    println(sizeof(Score)); // 8  (same as sizeof(int))
    println(sizeof(Weight));// 8  (same as sizeof(f64))
}
```

---



## 5. Variables, Constants, and Comptime

This section defines declaration forms (`var`, `const`, qualifiers like `register`/`atomic`/`volatile`/`global`), compile-time evaluation via `comptime {}`, assignment semantics, destructuring, scope behavior, and predefined integer constants.

The declaration rule from §4.2 applies throughout this section: a declaration MUST include either an explicit type annotation or an initializer expression.

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

**Scope**: Variables are block-scoped. See §5.12 for scope rules.

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

**Compile-time constants**: `const` variables may be evaluated at compile time if the initializer is a constant expression (see §5.9 for `comptime`).

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

### 5.5 `atomic var` — Atomic Variable Qualifier

**Syntax**: `atomic var name: type = initializer;`

**Semantics**:
- Every **load** of `name` compiles to an LLVM `load atomic … seq_cst` instruction.
- Every **store** to `name` (including the initializer) compiles to an LLVM `store atomic … seq_cst` instruction.
- `++` / `--` compile to a single `atomicrmw add/sub … seq_cst` (indivisible read-modify-write).
- Compound assignments `+=`, `-=`, `&=`, `|=`, `^=` compile to `atomicrmw add/sub/and/or/xor … seq_cst` when the pattern `x = x OP rhs` is recognised; otherwise a seq-cst load → compute → seq-cst store sequence is emitted.
- The variable's `alloca` is given natural ABI alignment so the hardware can perform the atomic operation without a fallback lock.
- The AST-level **CopyProp** and **CSE** passes treat atomic variables as *opaque* — their values are never forwarded or hoisted, because any read may return a value written by another thread.
- `!invariant.load` and `!noundef` metadata are suppressed on atomic loads.

**Use cases**:
- Shared counters incremented by multiple threads without a mutex.
- Lock-free flags checked by multiple threads.
- Any integer variable that is written by one thread and read by another.

**Example**:
```omscript
global atomic var counter: i64 = 0;

fn worker() {
    counter++;         // atomicrmw add i64*, i64 1, seq_cst
    return 0;
}

fn main() {
    var t1 = thread_create("worker");
    var t2 = thread_create("worker");
    thread_join(t1);
    thread_join(t2);
    println(counter);  // always 2 — no data race, no mutex required
    return 0;
}
```

**Combining with `volatile`**: `atomic volatile var` (or `volatile atomic var`) applies both qualifiers; every access is simultaneously atomic (seq-cst) and volatile (prevents compiler elision/reordering at the IR level).

**Supported types**: `int` / `i64`, `i32`, `i16`, `i8`, `u64`, `u32`, `u16`, `u8`, `float`, `f64`, and pointer types. Non-scalar types (arrays, strings, structs) fall back to a plain seq-cst store; use a mutex for complex aggregate operations.

**Limitations**:
- Does **not** compose with `const` (a constant has no storage to protect).
- Does **not** compose with `register var` (register variables have no address).

### 5.6 `volatile var` — Volatile Load/Store Qualifier

**Syntax**: `volatile var name: type = initializer;`

**Semantics**:
- Every **load** of `name` compiles to an LLVM `load volatile i64 …` instruction.
- Every **store** to `name` (including the initializer) compiles to an LLVM `store volatile i64 …` instruction.
- Increment/decrement loads and stores are both marked volatile.
- The compiler is forbidden from eliding, caching, reordering, or merging volatile accesses — each source-level read and write produces a distinct memory instruction in the output binary.
- The AST-level **CopyProp** and **CSE** passes treat volatile variables as *opaque* — no read is ever forwarded to a prior value, and no expression involving a volatile load is hoisted.
- `!invariant.load` and `!noundef` metadata are suppressed on volatile loads.

**Use cases**:
- Memory-mapped I/O registers whose value changes without any visible write in the program.
- Variables written by a signal handler or hardware interrupt.
- Spin-wait loops where the condition must be re-read every iteration.
- Debugging: prevent the optimizer from removing a variable load so it remains observable in debuggers and profilers.

**Example**:
```omscript
// Spin-wait for hardware status register
volatile var hw_status: i64 = 0;

fn wait_ready() -> i64 {
    while (hw_status == 0) {}  // hw_status re-loaded every iteration
    return hw_status;
}
```

**Note**: `volatile` alone does **not** provide atomicity. If `hw_status` can be written by another thread, combine with `atomic`:
```omscript
atomic volatile var hw_status: i64 = 0;
```

**Limitations**:
- Does **not** compose with `const`.
- Does **not** provide ordering guarantees between different volatile variables; use `atomic` for sequentially consistent cross-thread communication.

### 5.7 `global var` / `global const`

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

### 5.8 `frozen` / `freeze` — Read-Only After Initialization

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

### 5.9 `comptime { ... }` Blocks — Compile-Time Evaluation

OmScript's `comptime` keyword has two distinct but related forms:

1. **Top-level `comptime {}` block** — defines compile-time constants, provides conditional compilation, and replaces preprocessor patterns.
2. **Expression-context `comptime { ... }` block** — evaluates code at compile time via CF-CTRE and replaces the block with the resulting value.

---

#### 5.9.1 Top-level `comptime {}` — Constant Definitions and Conditional Compilation

Placed at the top level of a source file (not inside a function), a `comptime {}` block is processed entirely at parse time. Its primary use is to define compile-time constants and to select platform-specific code paths — the same role that `#define` and `#if` play in C.

**Supported statements inside a top-level comptime block:**

| Statement | Description |
| --- | --- |
| `const NAME [: TYPE] = VALUE;` | Define a compile-time constant |
| `if (COND) { ... } [else if (...) { ... }]* [else { ... }]` | Conditional inclusion |
| `error("message");` | Abort compilation with a diagnostic |
| `warning("message");` | Emit a diagnostic and continue |

**Supported value types** for `const`:

| Form | Example | Stored as |
| --- | --- | --- |
| Integer literal | `const MAX: int = 1024;` | `int` |
| Float literal | `const PI: float = 3.14159;` | `float` |
| String literal | `const GREETING: string = "hello";` | `string` |
| Boolean literal | `const DEBUG: bool = true;` | `bool` (stored as `int` 0/1) |
| Reference to comptime const | `const ALIAS = MAX;` | same type as source |

**Built-in comptime identifiers** (read-only):

| Name | Type | Example value | Equivalent preprocessor macro |
| --- | --- | --- | --- |
| `OS` | string | `"linux"` / `"windows"` / `"macos"` | `__OS__` |
| `ARCH` | string | `"x86_64"` / `"aarch64"` / `"arm"` | `__ARCH__` |
| `VERSION` | string | `"4.9.0"` | `__VERSION__` |
| `FILE` | string | `"src/main.om"` | `__FILE__` |

> **Note:** Built-in string comptime constants (`OS`, `ARCH`, `VERSION`, `FILE`) are only available inside `comptime {}` condition expressions. They are not emitted as runtime string globals.

**Condition expression syntax** for `if (COND)`:
- Comparison: `NAME == "value"`, `COUNT >= 100`, `FLAG != 0`
- Logical: `COND1 && COND2`, `COND1 || COND2`, `!COND`
- Existence test: `defined(NAME)` — true if `NAME` is a defined comptime constant (including CLI `-D` flags and built-in identifiers)
- Parenthesised: `(COND1 && COND2) || COND3`

Every integer or bool `const` that survives an active branch is also injected as a **global `const` variable** accessible inside function bodies — no extra annotation required.

**`comptime if COND { ... }` shorthand** (Phase 2):

For simple single-condition cases, the outer `comptime { }` braces may be omitted. The condition does not need parentheses, allowing bare boolean constants:

```omscript
comptime if BUILD_DEBUG {
    const LOG_LEVEL: int = 2;
}

comptime if OS == "linux" {
    const USE_EPOLL: int = 1;
} else {
    const USE_EPOLL: int = 0;
}
```

This is exactly equivalent to `comptime { if (COND) { ... } }` — a syntactic convenience.

**`-D NAME[=VALUE]` CLI flags** (Phase 2):

Comptime constants can be injected from the command line without modifying source code:

```sh
omsc compile main.om -DBUILD_DEBUG         # injects  comptime const BUILD_DEBUG: int = 1
omsc compile main.om -DLOG_LEVEL=3         # injects  comptime const LOG_LEVEL: int = 3
omsc compile main.om -DPLATFORM=embedded   # injects  comptime const PLATFORM: string = "embedded"
```

CLI-injected constants behave identically to constants defined with `comptime { const ... }` and are visible to `defined()`, `if (COND)`, and function bodies throughout the file.

**Example — platform constants and conditional selection:**
```omscript
comptime {
    const VERSION_MAJOR: int = 4;
    const VERSION_MINOR: int = 4;
    const GREETING: string = "Hello";

    if (OS == "linux") {
        const PLATFORM_NAME: string = "Linux";
        const USE_EPOLL: int = 1;
    } else if (OS == "windows") {
        const PLATFORM_NAME: string = "Windows";
        const USE_EPOLL: int = 0;
    } else {
        const PLATFORM_NAME: string = "Other";
        const USE_EPOLL: int = 0;
    }

    if (defined(DEBUG) && DEBUG) {
        warning("building in debug mode");
    }
}

fn main() -> int {
    println(PLATFORM_NAME);   // "Linux" / "Windows" / "Other"
    println(USE_EPOLL);       // 1 or 0
    return 0;
}
```

**Preprocessor migration guide:**

| C/C++ preprocessor | OmScript `comptime {}` equivalent | Notes |
| --- | --- | --- |
| `#define MAX 1024` | `comptime { const MAX: int = 1024; }` | Fully typed |
| `#define PI 3.14159` | `comptime { const PI: float = 3.14159; }` | |
| `#define NAME "hello"` | `comptime { const NAME: string = "hello"; }` | |
| `#define DEBUG` (flag) | `comptime { const DEBUG: bool = true; }` | |
| `#undef DEBUG` | not needed — omit the const or use `false` | |
| `#ifdef DEBUG` / `#endif` | `comptime { if (defined(DEBUG)) { ... } }` | |
| `#ifndef NDEBUG` | `comptime { if (!defined(NDEBUG)) { ... } }` | |
| `#if defined(__linux__)` | `comptime { if (OS == "linux") { ... } }` | |
| `#if defined(_WIN32)` | `comptime { if (OS == "windows") { ... } }` | |
| `#if MAJOR >= 4` | `comptime { if (VERSION_MAJOR >= 4) { ... } }` | |
| `#if X == 0 && Y != 0` | `comptime { if (X == 0 && Y != 0) { ... } }` | Full boolean logic |
| `#error "message"` | `comptime { error("message"); }` | |
| `#warning "message"` | `comptime { warning("message"); }` | |
| `#assert COND` | `comptime { if (!COND) { error("assertion failed"); } }` | |
| `__LINE__` | not available (use `@line` diagnostic annotation) | |
| `__FILE__` | not available (use `import` path instead) | |
| `__VERSION__` | `comptime { const VER = VERSION; }` where `VERSION` is built-in | |
| `__OS__` | built-in `OS` comptime string | `"linux"`, `"windows"`, `"macos"` |
| `__ARCH__` | built-in `ARCH` comptime string | `"x86_64"`, `"aarch64"`, `"arm"` |
| `#define MAX(a,b) ((a)>(b)?(a):(b))` | `fn max(a: int, b: int) -> int { ... }` | Use a typed function — no macros needed |
| `#define SQUARE(x) ((x)*(x))` | `fn square(x: int) -> int { return x*x; }` | Inlined at O1+ automatically |

**Why `comptime {}` is better:**
1. **Type safety** — `const MAX: int = 1024` is an `int`, not an untyped text substitution.
2. **Scope awareness** — comptime constants obey file scope, no global pollution.
3. **IDE support** — comptime constants appear in hover, completion, and cross-references.
4. **No order dependency** — preprocessor macros must be defined before use; comptime constants are visible to the entire file regardless of order.
5. **Readable diagnostics** — error messages use the constant name, not the substituted text.
6. **Integration with CF-CTRE** — comptime constants feed directly into the compile-time reasoning engine, enabling deeper constant folding across function call boundaries.

---

#### 5.9.2 Expression-context `comptime { ... }` — Compile-Time Value Computation

Used as an **expression** (inside a function or initialiser), a `comptime { ... }` block is evaluated at compile time by the CF-CTRE engine and replaced with the resulting constant value.

**Syntax**: `comptime { statements; return expr; }`

**Semantics**:
- Executes code at **compile time** and replaces the block with the resulting value
- The block must end with `return expr;` where `expr` is the compile-time value
- Only constant expressions and control flow (no I/O, no heap allocation, no calls to runtime-only functions) are allowed

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
- If evaluation fails (e.g., infinite loop, runtime-only operation), the expression remains dynamic

**Use cases**:
- Precompute lookup tables
- Generate repetitive code patterns
- Compile-time configuration checks

### 5.10 Assignment and Compound Assignment

**Simple assignment**: `variable = expression;`

```omscript
var x: int = 10;
x = 20;
```

**Compound assignment operators**:
| Operator | Meaning | Example | Equivalent |
| --- | --- | --- | --- |
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

### 5.11 Destructuring Assignment

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

### 5.11.2 Tuple Destructuring

**Syntax**: `var (a, b [, c ...]) = expr;` or `const (a, b) = expr;`

**Semantics**:
- Declares multiple variables by unpacking a value returned from an expression
- The parenthesized form accesses fields `.0`, `.1`, `.2`, … — these are the **integer field indices** of the OmScript tuple/struct representation. The compiler synthesises these indexed-field accesses; the struct does not need to declare fields literally named `0`, `1`, etc.
- Underscore `_` as a placeholder skips an element
- Works with both `var` and `const`

**Desugaring**:
```omscript
var (x, y) = get_coords();
// ↓ desugars to:
var __tdestr_0 = get_coords();
var x = __tdestr_0.0;
var y = __tdestr_0.1;
```

**Example**:
```omscript
fn get_point() -> Point {
    return Point { x: 3, y: 4 };
}

var (px, py) = get_point();
println(px);  // 3
println(py);  // 4

// Skip an element with _:
var (first, _, third) = get_triple();
```

### 5.12 Scope Rules

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

### 5.13 Predefined Integer Constants

OmScript provides a set of built-in identifier constants for the minimum and maximum values of every integer type. These identifiers are resolved at code-generation time and do **not** require any import.

#### 5.13.1 Canonical names

| Constant | Type | Value |
| --- | --- | --- |
| `I1_MAX` | signed 1-bit | `0` (only representable non-negative value in signed 1-bit two's complement) |
| `I1_MIN` | signed 1-bit | `-1` |
| `I8_MAX` | signed 8-bit | `127` |
| `I8_MIN` | signed 8-bit | `-128` |
| `I16_MAX` | signed 16-bit | `32767` |
| `I16_MIN` | signed 16-bit | `-32768` |
| `I32_MAX` | signed 32-bit | `2147483647` |
| `I32_MIN` | signed 32-bit | `-2147483648` |
| `I64_MAX` | signed 64-bit | `9223372036854775807` |
| `I64_MIN` | signed 64-bit | `-9223372036854775808` |
| `I128_MAX` | signed 128-bit | `2^127 - 1` |
| `I128_MIN` | signed 128-bit | `-2^127` |
| `I256_MAX` | signed 256-bit | `2^255 - 1` |
| `I256_MIN` | signed 256-bit | `-2^255` |
| `U1_MAX` | unsigned 1-bit | `1` |
| `U1_MIN` | unsigned 1-bit | `0` |
| `U8_MAX` | unsigned 8-bit | `255` |
| `U8_MIN` | unsigned 8-bit | `0` |
| `U16_MAX` | unsigned 16-bit | `65535` |
| `U16_MIN` | unsigned 16-bit | `0` |
| `U32_MAX` | unsigned 32-bit | `4294967295` |
| `U32_MIN` | unsigned 32-bit | `0` |
| `U64_MAX` | unsigned 64-bit | `18446744073709551615` |
| `U64_MIN` | unsigned 64-bit | `0` |
| `U128_MAX` | unsigned 128-bit | `2^128 - 1` |
| `U128_MIN` | unsigned 128-bit | `0` |
| `U256_MAX` | unsigned 256-bit | `2^256 - 1` |
| `U256_MIN` | unsigned 256-bit | `0` |

The pattern `I{N}_MAX`, `I{N}_MIN`, `U{N}_MAX`, `U{N}_MIN` extends to **any bit width from 1 to 256** (e.g., `I3_MAX = 3`, `U7_MAX = 127`).

#### 5.13.2 Convenience aliases

| Alias | Equivalent |
| --- | --- |
| `INT_MAX` | `I64_MAX` |
| `INT_MIN` | `I64_MIN` |
| `UINT_MAX` | `U64_MAX` |
| `UINT_MIN` | `U64_MIN` |
| `BOOL_MAX` | `U1_MAX` = `1` |
| `BOOL_MIN` | `U1_MIN` = `0` |

#### 5.13.3 C-style aliases (INT8_MAX, UINT32_MAX, …)

The C standard-library naming convention is also accepted:

| C-style alias | Equivalent |
| --- | --- |
| `INT8_MAX` / `INT8_MIN` | `I8_MAX` / `I8_MIN` |
| `INT16_MAX` / `INT16_MIN` | `I16_MAX` / `I16_MIN` |
| `INT32_MAX` / `INT32_MIN` | `I32_MAX` / `I32_MIN` |
| `INT64_MAX` / `INT64_MIN` | `I64_MAX` / `I64_MIN` |
| `UINT8_MAX` | `U8_MAX` |
| `UINT16_MAX` | `U16_MAX` |
| `UINT32_MAX` | `U32_MAX` |
| `UINT64_MAX` | `U64_MAX` |

#### 5.13.4 Usage examples

```omscript
fn main() -> int {
    var a: int = INT_MAX;          // 9223372036854775807
    var b: int = I32_MAX;          // 2147483647
    var c: int = U8_MAX;           // 255
    var d: int = BOOL_MAX;         // 1
    var e: i128 = I128_MAX;        // 2^127 - 1

    // Use in comparisons / guards
    if (a == I64_MAX) { println("maxed out"); }

    // Boundary checks
    var x: int = 200;
    if (x > U8_MAX) { println("overflow u8"); }

    return 0;
}
```

**Note on narrow types**: Wide constants like `I128_MAX` have their native LLVM bit-width (128 or 256 bits) so they can be used in comparisons with `i128`/`i256` variables. All constants with width ≤ 64 are returned as 64-bit integers (`i64`) so they work seamlessly in ordinary arithmetic.

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

### 6.3.1 Named Call Arguments

**Syntax**: `function(name: value, name2: value2, ...)`

**Semantics**:
- Individual arguments may be named using `name: value` syntax at the call site.
- Named arguments are reordered to match the function's **declaration parameter order** at compile time.
- Positional and named arguments may be mixed: positional arguments must appear **before** named arguments.

**Example**:
```omscript
fn create_rect(width: int, height: int, filled: int) -> int {
    return width * height + filled;
}

// Named arguments — any order:
var r1 = create_rect(height: 5, width: 4, filled: 0);   // → create_rect(4, 5, 0)
var r2 = create_rect(width: 4, height: 5, filled: 1);   // → create_rect(4, 5, 1)

// Mixed positional + named (positional first):
var r3 = create_rect(4, height: 5, filled: 0);  // width=4 positional, rest named
```

**Restrictions**:
- Named arguments are resolved using the parameter names from the **declaration** (`fn` definition) in the current translation unit. When the function declaration is known, unrecognised argument names produce a compile-time error.
- When calling a function whose declaration is not visible (e.g., a built-in or a function resolved by string-literal forwarding), named-argument labels are silently ignored and arguments are passed in the order they appear at the call site — i.e., the call degrades to positional.
- Duplicate argument names at the same call site cause a compile-time error.

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

**Status**: **Reserved** — Generic functions (type parameters on function declarations) are not implemented in the current parser or type system. The `<T>` syntax for generic parameter lists is not yet a parser token sequence. This is a planned future language feature.

**Anticipated syntax** (not yet accepted): `fn name<T>(param: T) -> T { ... }`

**Workaround today**: Use explicit typed overloads (e.g. `fn process_int(x: int)` + `fn process_float(x: float)`), or pass values through `ptr<T>` with manual casting for type-erased algorithms.

### 6.6 Function Annotations

Functions may be preceded by **annotation attributes** (introduced by `@`) that modify compilation and optimization behavior.

OmScript provides three **compound annotation forms** that group related hints into a single, namespaced annotation.  Multiple options may appear comma-separated inside the parentheses, and multiple compound annotations may be stacked on separate lines before `fn`.

```omscript
@opt(hot, inline, vectorize)
@semantics(pure, nounwind)
fn compute(x: int) -> int {
    return x * 2
}
```

---

#### `@opt(...)` — Optimization Hints

Controls how the compiler optimizes this function.

**Syntax**: `@opt(option, option, ...)` where options are:

| Option | Effect |
| --- | --- |
| `inline` | Force-inline at all call sites (`alwaysinline`) |
| `noinline` | Never inline (`noinline`) |
| `hot` | Mark as frequently executed; prioritizes optimization effort |
| `cold` | Mark as rarely executed; deprioritizes and may move off hot paths |
| `vectorize` | Hint SIMD vectorization for loops in this function |
| `novectorize` | Disable auto-vectorization |
| `unroll` | Request loop unrolling |
| `nounroll` | Disable loop unrolling |
| `parallel` | Hint that loops may be parallelized across threads |
| `noparallel` | Prevent loop parallelization |
| `flatten` | Aggressively unroll and flatten control flow |
| `minsize` | Optimize for code size over speed (`minsize` + `optsize`) |
| `optnone` | Disable all optimizations (useful for debugging IR) |
| `align=N` | Align function entry to `N` bytes (power of two) |
| `align=AUTO` | Auto-align to cache-line optimal (64 bytes); also aligns all local allocas |
| `align` | Same as `align=AUTO` (bare form) |

```omscript
@opt(hot, inline)
fn inner_loop(n: int) -> int {
    var sum: int = 0
    for (i: int in 0...n) { sum = sum + i }
    return sum
}

@opt(cold, minsize)
fn error_handler(code: int) {
    println("Error: ", code)
}

@opt(align=32)
fn simd_kernel(x: int) -> int {
    return x * 2
}

@opt(align=AUTO)
fn cache_friendly_kernel(arr: int[], n: int) -> int {
    // All local allocas are 64-byte (cache-line) aligned.
    var sum: int = 0
    for (i: int in 0...n) { sum = sum + arr[i] }
    return sum
}
```

---

#### `@semantics(...)` — Semantic / ABI Attributes

Declares behavioral contracts that the optimizer may rely on.

**Syntax**: `@semantics(attr, attr, ...)` where attrs are:

| Attribute | Effect |
| --- | --- |
| `pure` | No side effects; same inputs always produce same outputs (`readonly`/`readnone`) |
| `speculatable` | May be hoisted across branches and into loop preheaders without observable cost |
| `noreturn` | Function never returns (e.g., calls `exit()`, throws unconditionally) |
| `nounwind` | Never throws; enables more aggressive inlining and frame omission |
| `restrict` | Pointer parameters do not alias each other (C `restrict` on all ptr params) |
| `noalias` | Same as `restrict` |
| `const_eval` | Compile-time foldable when all arguments are constants |
| `willreturn` | Function always terminates in finite time (no infinite loops or unbounded recursion). Enables DSE and load-forwarding across the call. |
| `nosync` | No synchronization, mutex, or blocking I/O. Enables call reordering and speculative CSE. |
| `nofree` | Function never deallocates memory. Alias analysis can prove that pointers live across the call remain valid. |

```omscript
@semantics(pure, nounwind)
fn square(x: int) -> int {
    return x * x
}

@semantics(noreturn)
fn fatal(msg: str) {
    println(msg)
    exit(1)
}

@semantics(restrict, pure)
fn dot_product(a: ptr<int>, b: ptr<int>, n: int) -> int {
    var s: int = 0
    for (i: int in 0...n) { s = s + a[i] * b[i] }
    return s
}

@semantics(willreturn, nosync, nofree)
fn compute_hash(data: ptr<int>, n: int) -> int {
    var h: int = 0
    for (i: int in 0...n) { h = h ^ (data[i] * 2654435761) }
    return h
}
```

**Combining `pure` + `speculatable`** gives the strongest optimization signal: `pure` proves read-only semantics; `speculatable` additionally permits the call to be executed speculatively before all guards are evaluated.

**`willreturn` + `nosync` + `nofree`** is a strong combination for pure compute functions: it tells LLVM the call can be sunk/hoisted, reordered freely, and all pointers passed in remain alive after the call returns.

---

#### `@memory(...)` — Memory Model Attributes

Provides memory-access level, aliasing, and allocator metadata so the optimizer can reason about pointer provenance, alias analysis, and reordering safety.

**Syntax**: `@memory(option, option, ...)` — options may be combined freely.

##### Memory-Access Level (mutually exclusive)

| Option | LLVM effect | Description |
| --- | --- | --- |
| `none` | `memory(none)` | No memory access at all. Implies `nounwind`, `nosync`, `willreturn`. |
| `readonly` | `memory(read)` | Only reads memory; never writes. |
| `writeonly` | `memory(write)` | Only writes memory; never reads. |
| `readwrite` | *(explicit default)* | Documents that both reads and writes may occur (no-op). |
| `argmem` | `memory(argmem: readwrite)` | Only reads/writes through its own pointer arguments. |
| `argmem_ro` | `memory(argmem: read)` | Only reads through its own pointer arguments. |
| `inaccessiblemem` | `memory(inaccessiblemem: readwrite)` | Only accesses memory the caller cannot see (e.g., global errno, thread-local state). |
| `inaccessiblemem_or_argmem` | `memory(inaccessiblemem: rw, argmem: rw)` | Combines inaccessible-memory and arg-memory access. |

##### Aliasing Hints

| Option | LLVM effect | Description |
| --- | --- | --- |
| `noalias_ret` | `noalias` on return | Return pointer is guaranteed not to alias any pointer visible to the caller. Use for factory functions that return fresh objects but are not full allocators. |

##### Allocator Metadata

| Option | LLVM effect | Description |
| --- | --- | --- |
| `allocator` | `allocsize(0)` + `noalias` return | Return pointer is a freshly allocated region; default size param is index 0. |
| `size=N` | Overrides allocsize size-param index | Parameter at index `N` (0-based) holds the allocation byte count. |
| `count=M` | Overrides allocsize count-param index | Parameter at index `M` is a count multiplied by `size`. |

```omscript
// Pure math — no memory access at all.
@memory(none)
@semantics(pure)
fn clamp(v: int, lo: int, hi: int) -> int {
    if (v < lo) { return lo }
    if (v > hi) { return hi }
    return v
}

// Reads only through its pointer arguments.
@memory(argmem_ro)
fn sum(data: ptr<int>, n: int) -> int {
    var s: int = 0
    for (i: int in 0...n) { s = s + data[i] }
    return s
}

// Returns a fresh object — not a full allocator but still non-aliasing.
@memory(noalias_ret)
fn make_point(x: int, y: int) -> ptr {
    // builds and returns a Point
}

// Full allocator: arg 0 is the byte count.
@memory(allocator, size=0)
fn my_alloc(bytes: int) -> ptr {
    return malloc(bytes)
}

// Calloc-style: arg 0 is count, arg 1 is element size.
@memory(allocator, size=1, count=0)
fn my_calloc(count: int, size: int) -> ptr {
    return calloc(count, size)
}

// Only touches memory inaccessible to the caller (e.g., errno).
@memory(inaccessiblemem_or_argmem)
fn safe_sqrt(x: f64) -> f64 {
    return sqrt(x)
}
```

**Combining with `@semantics`**: `@memory(none)` and `@semantics(pure, nounwind)` produce identical LLVM attributes; using both is redundant. `@memory(readonly)` is weaker than `@semantics(pure)` (pure also implies `nosync` and `willreturn` for non-recursive functions). The compiler warns when contradictory combinations are used.

---

#### `@static` — Internal Linkage

Marks the function as module-private (not exported). May enable additional interprocedural optimizations.

```omscript
@static
fn helper(x: int) -> int { return x + 1 }
```

---

#### `@optmax` / `@optmax(...)` — Maximum Optimization

Applies the OPTMAX optimization profile (aggressive inlining, loop transforms, etc.). See §18.3 for configuration options.

```omscript
@optmax
fn hot_kernel(n: int) -> int {
    var s: int = 0
    for (i: int in 0...n) { s = s + i }
    return s
}
```

---

#### `@deprecated` / `@deprecated("message")` — Call-Site Deprecation Warning

Marks a function as deprecated. Calls still compile, but each call site emits a warning.

```omscript
@deprecated
fn old_api(x: int) -> int { return x * 2; }

@deprecated("use new_api() instead")
fn legacy(x: int) -> int { return x + 1; }
```

**Semantics:**
- Warning is emitted at call sites, not only at declaration.
- `@deprecated("message")` appends the custom message to the warning text.
- Works with regular calls and desugared call forms.

---

#### Deprecated Flat Annotations

The individual flat forms (`@hot`, `@cold`, `@inline`, `@pure`, `@noreturn`, etc.) are **deprecated**. They still compile — the compiler emits a warning and applies the same effect — but all new code should use the compound forms above.

| Deprecated | Replacement |
| --- | --- |
| `@inline` | `@opt(inline)` |
| `@noinline` | `@opt(noinline)` |
| `@hot` | `@opt(hot)` |
| `@cold` | `@opt(cold)` |
| `@vectorize` | `@opt(vectorize)` |
| `@novectorize` | `@opt(novectorize)` |
| `@unroll` | `@opt(unroll)` |
| `@nounroll` | `@opt(nounroll)` |
| `@parallel` | `@opt(parallel)` |
| `@noparallel` | `@opt(noparallel)` |
| `@flatten` | `@opt(flatten)` |
| `@minsize` | `@opt(minsize)` |
| `@optnone` | `@opt(optnone)` |
| `@align(N)` | `@opt(align=N)` / `@opt(align=AUTO)` |
| `@pure` | `@semantics(pure)` |
| `@speculatable` | `@semantics(speculatable)` |
| `@noreturn` | `@semantics(noreturn)` |
| `@nounwind` | `@semantics(nounwind)` |
| `@restrict` | `@semantics(restrict)` |
| `@noalias` | `@semantics(noalias)` |
| `@const_eval` | `@semantics(const_eval)` |
| `@allocator(size=N)` | `@memory(allocator, size=N)` |


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

### 6.9 First-Class Functions / `funcptr`

OmScript provides the `funcptr` type for storing and calling native function pointers at runtime. See §4.4.10 for the complete type description.

**Summary of operations**:

| Operation | Syntax | Semantics |
| --- | --- | --- |
| Obtain address of a named function | `funcptr_from("name")` | Resolves the function `name` visible at link time and returns its address as a `funcptr`. |
| Create from raw machine-code bytes | `funcptr_new(byteArr, n)` | Allocates a W^X page, copies `n` bytes from the `i64[]` array, returns a `funcptr`. |
| Call | `*f` | Invokes the function as `fn() → i64`. |

**Passing functions to higher-order builtins**: Higher-order functions such as `array_map`, `array_filter`, and `array_reduce` accept functions by **name** (a string literal), not as `funcptr` values. Pass a lambda expression or a quoted function name:

```omscript
var doubled: int[] = array_map(arr, |x| x * 2);
var doubled2: int[] = array_map(arr, "my_doubler");
```

**Quick example**:
```omscript
fn square() -> int { return 7 * 7; }  // funcptr call passes no args

fn main() -> int {
    var f: funcptr = funcptr_from("square");
    var result: int = *f;  // 49
    println(result);
    return 0;
}
```

For a complete description including JIT usage and platform notes, see §4.4.10.

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

### 7.8b `@range[lo, hi] expr` — Internal Range-Bound Hint

**Syntax**: `@range[lo, hi] expression`

Asserts to the optimizer that `expression` evaluates to an integer within the inclusive range `[lo, hi]`. Both bounds must be integer literals (an optional unary `-` is allowed) and `lo <= hi` is enforced at parse time. The annotation binds at unary-operator precedence, so `@range[0, 9] x + 1` parses as `(@range[0, 9] x) + 1`.

**Semantics**:
- **Compile-time check**: if `expression` folds to a known integer constant outside `[lo, hi]`, the compiler emits a hard error and produces no IR.
- **Runtime hint**: otherwise, codegen emits `@llvm.assume(val >= lo && val <= hi)` and attaches `!range !{lo, hi+1}` metadata to load/call results so LLVM's LVI / CorrelatedValuePropagation / SCEV / InstCombine can propagate the bound.
- **Non-negativity**: when `lo >= 0` the value is also recorded in the codegen's non-negative set, so OmScript-level passes (foreach-range fusion, array-length CSE, sign-bit elision) can skip their own non-negativity guards.
- **Pure hint**: never affects observable behaviour — only optimization. Works on any expression that produces an integer (loads, calls, arithmetic, etc.).

**Example**:
```omscript
fn lookup(table:[i64], idx:i64) {
    // We've externally guaranteed idx ∈ [0, 255]; tell the optimizer.
    var safe = @range[0, 255] idx;
    return table[safe];               // bounds-check elimination on safe
}

fn main() {
    var n = input();
    var bucket = @range[0, 15] (n & 15);   // mask result is always 0..15
    return bucket;
}

// Compile error — caught at compile time, no IR generated:
//   var bad = @range[0, 9] 100;
```

**Effect**: Emits `llvm.assume` calls + `!range` metadata on the inner value. No new instructions on hot paths once LLVM has propagated the bound.

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
assume(b != 0);                     // statement
var q: int = a / assume(b != 0);    // expression position also accepted
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
| --- | --- | --- | --- |
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

### 7.11 `jmp` / `label` — Deprecated Unconditional Jump

> **⚠ Deprecated.** `jmp` emits a compile-time deprecation warning every time it is used. It will be removed in a future version. Always prefer structured control flow (`if`, `while`, `for`, `break`, `continue`).

`jmp` performs an unconditional branch to a named **label** in the **same function**. Labels are declared with the `label` keyword.

**Syntax:**
```
jmp label_name;
...
label label_name:
```

**Semantics:**
- Execution transfers immediately to the statement following `label name:`.
- Code between a `jmp` and its target label is unreachable (skipped entirely).
- A `label` declaration does not end the current block; control falls through from the preceding statement unless there is a `jmp` or other terminator above it.
- Labels are function-scoped. You cannot jump to a label in a different function.
- Both forward jumps (label appears later in source) and backward jumps (label appears earlier — manual loop) are supported.

**Example — forward jump (skip a block):**
```omscript
fn main() -> int {
    jmp done;          // ⚠ deprecated warning emitted here
    var x: int = 42;   // error — 'jmp done' jumps forward over 'x'
    label done:
    return 0;
}
```

> The compiler **errors** if a forward `jmp` skips over a `var` declaration at the same block level, because the initializer would be bypassed. Move all declarations before the `jmp`, or declare them after the label.

**Correct pattern — declarations before the jump:**
```omscript
fn main() -> int {
    var x: int = 0;
    jmp done;       // ⚠ deprecated
    x = 42;         // skipped; x remains 0
    label done:
    return x;       // returns 0
}
```

**Example — backward jump (manual counted loop):**
```omscript
fn sum_to_four() -> int {
    var i: int = 1;
    var total: int = 0;
    label loop_top:        // ← loop entry
    total = total + i;
    i = i + 1;
    if (i <= 4) {
        jmp loop_top;      // ⚠ deprecated — prefer `for i in 1..=4 { total += i; }`
    }
    return total;  // 10
}
```

**Compile-time safety checks:**

| Violation | Severity | Message |
| --- | --- | --- |
| Label not defined in current function | **Error** | `'jmp foo': label 'foo' is not defined in this function` |
| Forward jump over `var` declaration | **Error** | `'jmp after' at line N jumps forward over declaration of variable 'x'...` |
| Any use of `jmp` | **Warning** | `'jmp' is deprecated; prefer structured control flow (if / while / for / break / continue)` |

**Migration guide:**
| `jmp` pattern | Preferred replacement |
| --- | --- |
| Skip optional block | `if (!condition) { ... }` |
| Retry / retry loop | `while (condition) { ... }` |
| Counted loop | `for i in start..end { ... }` |
| Early exit from nested loops | Labeled `break` / `continue` (see §8.15; e.g. `outer: while (cond) { /* statements */ continue outer; /* statements */ break outer; }`) |

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
- `for (var in start..end step N) { body }` — Ascending loop with explicit step
- `for (var in start downto end step N) { body }` — Descending loop with explicit step

**`downto` semantics**: Decrements iterator from `start` to `end` (inclusive).

**`step` keyword**: An optional `step N` suffix controls the iteration increment. `N` must be a positive integer expression. Negative counts and zero are runtime errors. For descending loops, the user supplies a positive step value and the compiler negates it automatically.

**Example (descending)**:
```omscript
for (i: int in 10 downto 0) {
    println(i);  // Prints: 10, 9, 8, ..., 1, 0
}
```

**Example (step)**:
```omscript
for i in 0..20 step 2 {
    println(i);  // Prints: 0, 2, 4, ..., 18
}

for i in 10 downto 0 step 2 {
    println(i);  // Prints: 10, 8, 6, 4, 2, 0
}
```

**Alternative (`range_step`)**: The `range_step` built-in generates an array for use with `foreach` or `for … in`:
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

#### Labeled `break` / `continue`

Any loop can be given a **label** by prefixing it with `label_name:`. Labeled `break label;` and `continue label;` then target the labeled loop rather than the nearest enclosing one. This avoids flag variables for breaking out of deeply nested loops.

**Syntax**:
```
label_name: while COND { ... break label_name; ... }
label_name: for x in arr { ... continue label_name; ... }
label_name: for i in 0..n { ... }
```

**Example — labeled break**:
```omscript
outer: for i in 0..10 {
    for j in 0..10 {
        if i * 10 + j == 23 {
            break outer;   // exit both loops when found
        }
    }
}
```

**Example — labeled continue**:
```omscript
outer: for i in 0..4 {
    for j in 0..4 {
        if j == 2 {
            continue outer;  // skip rest of outer iteration
        }
        process(i, j);       // only reached for j == 0 and j == 1
    }
}
```

**Rules**:
- Labels are identifiers (any valid identifier that is not a keyword).
- A label must immediately precede a loop keyword (`while`, `for`, `foreach`, `forever`, `loop`, `repeat`, `do`, `until`).
- `break label;` and `continue label;` are parse errors if no enclosing loop has that label.
- Unlabeled `break;` and `continue;` still target the nearest enclosing loop.

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
| --- | --- | --- | --- |
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
| --- | --- | --- | --- |
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
| --- | --- | --- | --- |
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
| --- | --- | --- |
| `&` | `a & b` | Bitwise AND |
| `|` | `a | b` | Bitwise OR |
| `^` | `a ^ b` | Bitwise XOR |
| `~` | `~a` | Bitwise NOT (one's complement) |
| `<<` | `a << b` | Left shift |
| `>>` | `a >> b` | Right shift |

**Type rules**: Operands must be integers. Result is integer of the same width.

**Shift semantics**:
- **Left shift `<<`**: Fills vacated bits with zeros
- **Right shift `>>`**: Always **logical** — fills vacated bits with zeros regardless of whether the value is signed or unsigned. OmScript does not expose an arithmetic right shift at the language level.

**Note**: SIMD vector right-shifts are an exception — element-wise `>>` on vector types (e.g., `i32x4`) uses arithmetic shift (`CreateAShr`). The logical-only guarantee applies to scalar integers.

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

### 9.6.1 `not in` Operator (Negated Membership)

**Syntax**: `element not in collection`

**Semantics**: `x not in collection` is sugar for `!array_contains(collection, x)`. Works for integer and string arrays. Combines naturally with `if`, `while`, `unless`, and other control flow.

**Example (array)**:
```omscript
var arr: int[] = [1, 2, 3];
if 5 not in arr {
    println("5 is absent");  // prints
}
```

**Example (string check)**:
```omscript
var banned: string[] = ["foo", "bar"];
if "baz" not in banned {
    println("safe");  // prints
}
```

**Result type**: `bool` (`1` = not present, `0` = present)

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

**Spread in function calls**: `...arr` can be used in function call argument position to expand a fixed-size array into individual positional arguments:

```omscript
fn add3(a, b, c) { return a + b + c; }

var args = [10, 20, 30];
var r = add3(...args);      // → add3(10, 20, 30) → 60

var pair = [5, 7];
var r2 = add3(1, ...pair);  // → add3(1, 5, 7)   → 13

// Inline literal spread:
var r3 = add3(...[100, 200, 300]);  // → 600
```

The spread array's length must be statically known at compile time (the callee has fixed arity). Arrays initialized from literal expressions qualify; dynamically-grown arrays (e.g. the result of `push()`) do not.

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
| --- | --- | --- | --- |
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

### 9.16 Type Cast Operator `as`

**Syntax**: `expr as TargetType`

**Semantics**: Explicitly converts `expr` to `TargetType`. The conversion is decided at codegen time based on the source and target types. This is a *coercion* — not a runtime function call — and emits a single LLVM instruction.

**Operator precedence**: `as` binds tighter than most binary operators. `x + y as i32` parses as `x + (y as i32)`.

#### 9.16.1 Integer ↔ Integer

| Source width vs. Target width | Direction | LLVM instruction | Signedness rule |
| --- | --- | --- | --- |
| Narrower → wider | Widening | `sext` (signed source) or `zext` (unsigned source) | Determined by the **source** variable's declared type (`u*` = unsigned → `zext`; `i*` or unqualified = signed → `sext`) |
| Wider → narrower | Truncation | `trunc` | Wraps (no trap) |
| Same width | Identity | None (SSA forwarded) | — |

```omscript
var a: i8  = 100;
var b: i32 = a as i32;  // sign-extend → 100

var c: u8  = 200;        // unsigned: 200 as signed = -56
var d: i32 = c as i32;  // zero-extend → 200 (not -56)

var e: i64 = 0xFFFF_FFFF_FFFF;
var f: i8  = e as i8;   // truncate → low 8 bits
```

#### 9.16.2 Integer ↔ Float

| Conversion | LLVM instruction | Notes |
| --- | --- | --- |
| signed int → float | `sitofp` | Default for `i*`-typed / unqualified variables |
| unsigned int → float | `uitofp` | When source variable is declared with `u*` annotation |
| float → signed int | `fptosi` | Default for `i*` target types |
| float → unsigned int | `fptoui` | When target type starts with `u` (e.g., `u32`, `u8`) |

```omscript
var i: int   = 42;
var f: float = i as f64;   // sitofp → 42.0

var u: u32   = 200;
var g: float = u as f64;   // uitofp → 200.0  (not -56.0)

var pi: float = 3.14;
var n: int   = pi as i64;  // fptosi → 3
var m: u8    = pi as u8;   // fptoui → 3
```

#### 9.16.3 Float ↔ Float

| Conversion | LLVM instruction |
| --- | --- |
| `f32` → `f64` (widening) | `fpext` |
| `f64` → `f32` (narrowing) | `fptrunc` |
| Same width | Identity |

```omscript
var a: f32 = 1.5;
var b: f64 = a as f64;  // fpext → 1.5 (double precision)
var c: f32 = b as f32;  // fptrunc → 1.5 (single precision)
```

#### 9.16.4 Any → `string`

`expr as string` formats the value to a heap-allocated OmScript string using `snprintf`:

| Source type | Format used | Example result |
| --- | --- | --- |
| Integer (`i*` / `u*`) | `%lld` | `42 as string` → `"42"` |
| Float (`f32` / `f64`) | `%g` | `3.14 as f64 as string` → `"3.14"` |
| Pointer (`ptr`) | `%llu` (address as unsigned decimal) | `p as string` → `"140732…"` |

```omscript
var n: int = -99;
var s: string = n as string;    // "-99"

var f: float = 2.718;
var t: string = f as string;    // "2.718"

var p: ptr<int> = &n;
var addr: string = p as string; // unsigned decimal address
```

#### 9.16.5 Pointer ↔ Integer

| Conversion | LLVM instruction |
| --- | --- |
| `ptr` → integer | `ptrtoint` |
| integer → `ptr` | `inttoptr` |
| `ptr` → `ptr` (any) | identity (opaque pointers) |

```omscript
var p: ptr<int> = &n;
var addr: int = p as int;        // ptrtoint
var q: ptr<int> = addr as ptr;   // inttoptr (unsafe!)
```

#### 9.16.6 Complete cast table

| Source | Target | Instruction |
| --- | --- | --- |
| `iN` (signed) | `iM` (M > N) | `sext` |
| `uN` (unsigned) | `iM`/`uM` (M > N) | `zext` |
| `iN`/`uN` | `iM`/`uM` (M < N) | `trunc` |
| `i*`/`u*` | `f32`/`f64` | `sitofp` / `uitofp` |
| `f32`/`f64` | `i*` | `fptosi` |
| `f32`/`f64` | `u*` | `fptoui` |
| `f32` | `f64` | `fpext` |
| `f64` | `f32` | `fptrunc` |
| any | `string` | `snprintf` into heap buffer |
| `ptr` | `int` | `ptrtoint` |
| `int` | `ptr` | `inttoptr` |
| `ptr` | `ptr` | identity |
| same type | same type | identity |

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

**Type annotation**: `dict` or `dict[K, V]` (the type parameters are informational; all keys and values are stored as `i64` internally).

**Access**: Use `map_get(d, key, default)` (see Part 2 for dict functions), or the subscript operator `d[key]` (missing keys return `0`).

```omscript
var age: int = map_get(d, "age", 0);  // 30
var same:int = d["age"];              // 30
```

**Mutation**: Dicts support both `map_set(d, key, val)` (returns a new dict) and the in-place subscript assignment `d[key] = val` (updates the dict in-place and writes back the potentially-reallocated pointer):

```omscript
d = map_set(d, "city", "NYC");  // functional style
d["zip"] = 10001;               // subscript assignment — equivalent
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

**Layout control**: Prefix the struct declaration with `@repr(...)` to control memory layout (see §4.4.6).

```omscript
@repr(packed)
struct Header { tag: i8, len: i32 }

var h: Header = Header { tag: 1, len: 42 };
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

- **Header:** 8-byte signed integer at slot 0 storing the logical element count.
- **Element storage:** Contiguous i64 slots at offsets 8, 16, 24, … from the base pointer.
- **Element type:** All array elements are i64 by default (integers, floats stored as IEEE 754 bits, string pointers cast to i64).
- **Zero-initialization:** Array elements are NOT zero-initialized by default unless `array_fill(n, 0)` is used (which emits `calloc`) or the literal path detects all-zero elements.
- **Pointer representation:** Arrays are passed as `i64` (pointer-to-integer) across function call boundaries. Callers convert back via `IntToPtr` before accessing elements.
- **Minimum heap capacity:** An empty heap array (`var a = []`) pre-allocates **16 slots = 128 bytes** so the first 15 `push()` calls never trigger a `realloc`. This eliminates the LLVM `dereferenceable(8)`/`allocsize` mismatch that breaks alias analysis when `malloc(8)` is immediately followed by `realloc`.

**Metadata tracking:**
- `arrayLenRangeMD_`: LLVM `!range [0, 2^63)` metadata attached to all length loads, proving to the optimizer that lengths are always non-negative (enables unsigned comparisons for loop bounds).
- `tbaaArrayLen_`, `tbaaArrayElem_`: distinct TBAA tags on the header slot vs element slots; lets the optimizer reorder element stores past length loads (they can't alias).

**Address calculations:**
- Element at index `i` (0-based) resides at `basePtr + (i + 1) * 8`.
- Negative indices are NOT supported — indexing with `i < 0` is undefined behavior.

**`push()` growth strategy:**
- Capacity is tracked implicitly: the number of allocated slots = the next power-of-2 ≥ `length + 1`, or `kMinArrayCapacity` (16) if smaller.
- On every `push(arr, v)`, the code checks whether `oldSlots` is a power-of-2 AND ≥ 16. If so, the buffer is doubled using `ctlz`-based `nextPow2(newLen + 1)` and `realloc`. Otherwise the existing buffer is reused at zero cost.
- `realloc` is called at most O(log n) times for n pushes. The grow path is weighted 1:99 (predicted not-taken) for branch predictor hinting.

### 11.2 Construction

#### Literal syntax

```omscript
var a = [1, 2, 3, 4];            // 4-element array
var b = [10 * 2, 30, sum(x)];    // expressions allowed
var c = [1, ...a, 5];            // spread — creates a new 6-element array
```

**Three-tier allocation strategy** (decided at the variable-declaration site):

| Tier | Condition | IR emitted |
| --- | --- | --- |
| **Read-only global** (O2+) | All elements are compile-time integer constants AND the variable has only read-only uses | `private unnamed_addr constant [n+1 x i64]` global; pointer returned as `ptrtoint`. Zero runtime cost; length + data in a single cache line. |
| **Stack alloca** (O1+) | Element count ≤ 512 AND variable doesn't escape its scope (or is `const`) | `alloca [n+1 x i64]` in function entry block; 16-byte aligned. Freed automatically on function exit. |
| **Heap malloc** | Everything else (dynamic expressions, escaping variables, > 512 elements) | `malloc((n+1)*8)` with `nonnull` + `dereferenceable((n+1)*8)` return attributes. For empty arrays (`n=0`), pre-allocates `kMinArrayCapacity * 8 = 128` bytes. |

**Const integer literal fast path (heap tier):** When all elements are compile-time integer constants but the variable escapes (so a heap allocation is needed), the code generator builds a `private unnamed_addr constant` global for the data and emits a single `memcpy` into the malloc'd buffer — no per-element stores.

**Spread literals:** A spread expression `[a, ...b, c]` computes the total element count dynamically (summing `len()` of each spread source), allocates exactly `(totalLen+1)*8` bytes, and copies elements using typed loops with `inbounds` GEPs and TBAA-tagged stores. The malloc call carries `nonnull` and, when `totalLen` is a compile-time constant, `dereferenceable(totalBytes)`.

#### `array_fill(n, val)`

**Signature:** `array_fill(i64, any) → array`  
**Semantics:** Allocate an array of length `n` and initialize every element to `val`.  
**Time complexity:** O(n)

**Allocation fast paths:**

1. **Read-only global (O2+):** When both `n` and `val` are compile-time constants, `2 ≤ n ≤ 1024`, and the variable has only read-only uses, the compiler emits a `private unnamed_addr constant` LLVM global initialized with `[n, val, val, …]` — zero runtime allocation.
2. **Zero-fill heap:** When `val` is 0 at compile time, emits `calloc(n+1, 8)` — avoids a `malloc` + fill loop, lets the OS/allocator supply pre-zeroed pages.
3. **General heap:** For dynamic `val`, emits `malloc((n+1)*8)` followed by a vectorizable fill loop with `llvm.loop.vectorize.enable` metadata.

In all heap paths the returned pointer carries `nonnull` and `dereferenceable((n+1)*8)` (exact bytes when `n` is constant, conservative `dereferenceable(8)` otherwise).

**Example:**
```omscript
var a = array_fill(100, 42);   // [42, 42, ..., 42] (100 elements)
println(len(a));                // 100
println(a[0]);                  // 42
```

#### Type-annotated literals

`[]T{elem1, elem2, ...}` — typed array literal syntax. The type annotation (`T`) is informational and does not affect code generation (all array elements are i64 at runtime). Elements are parsed identically to an untyped `[elem1, elem2, ...]` array literal.

```omscript
var nums = []int{10, 20, 30};  // same as [10, 20, 30]
var ptrs = []ptr<Node>{a, b};  // same as [a, b]
```

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

#### `swap(array, i, j) → i64`

**Signature:** `swap(array, i64, i64) → i64`  
**Semantics:** Swap the elements at indices `i` and `j` in the array IN-PLACE. Returns 0. Runtime error if either index is out of bounds.  
**Time:** O(1)  
**Side effects:** Mutates the array.  

> **Note:** `swap a, b;` is a separate statement syntax (§8.14) that swaps two scalar variables. `swap(arr, i, j)` is the built-in function for swapping array elements.

**Example:**
```omscript
var a = [10, 20, 30, 40];
swap(a, 0, 3);
println(a[0]);  // 40
println(a[3]);  // 10
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

#### `array_sum(array) → i64`

**Signature:** `array_sum(array) → i64`  
**Semantics:** Return the sum of all integer elements in `array`. Returns `0` for an empty array. Compile-time folded when all elements are constant literals.  
**Time:** O(n)

**Example:**
```omscript
var a = [3, 1, 4, 1, 5];
println(array_sum(a));  // 14
```

---

#### `array_sorted(array) → array`

**Signature:** `array_sorted(array) → array`  
**Semantics:** Return a new array containing the same elements as `array`, sorted in ascending order. The original array is **not** modified. Works for both integer and string arrays. Uses `qsort` internally.  
**Time:** O(n log n)

**Example:**
```omscript
var a = [5, 3, 1, 4, 2];
var s = array_sorted(a);
println(s[0]);   // 1
println(a[0]);   // 5  (original unchanged)
```

---

#### `array_reverse(array) → array`

**Signature:** `array_reverse(array) → array`  
**Semantics:** Return a new array with elements in reverse order. The original array is **not** modified.  
**Time:** O(n)

**Example:**
```omscript
var a = [1, 2, 3, 4, 5];
var r = array_reverse(a);
println(r[0]);   // 5
println(a[0]);   // 1  (original unchanged)
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

#### `array_first(array) → i64`

**Signature:** `array_first(array) → i64`  
**Semantics:** Return the first element. Runtime error (with message) if array is empty. Counterpart to `array_last`.  
**Time:** O(1)

**Example:**
```omscript
var a = [10, 20, 30];
println(array_first(a));  // 10
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

#### `array_zip_with(array, array, fn) → array`

**Signature:** `array_zip_with(array, array, function) → array`  
**Semantics:** Return a NEW array where `result[i] = fn(a[i], b[i])`. Length is `min(len(a), len(b))`.  
**Time:** O(min(len(a), len(b)) * cost(fn))

**Example:**
```omscript
fn add(a: int, b: int) -> int { return a + b; }

var xs = [1, 2, 3];
var ys = [10, 20];
var sums = array_zip_with(xs, ys, add);  // [11, 22]
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

#### `array_find(array, value) → i64`

**Signature:** `array_find(array, value) → i64`  
**Semantics:** Return the **index** of the first element equal to `value`, or `-1` if no element matches. Comparison is integer equality on each element.  
**Time:** O(n) (short-circuits on first match)

**Example:**
```omscript
var a = [5, 15, 25];
var idx = array_find(a, 15);  // 1  (element 15 is at index 1)
var miss = array_find(a, 99); // -1 (not found)
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

#### `array_find_index(array, fn) → i64`

**Signature:** `array_find_index(array, function) → i64`  
**Semantics:** Return the **index** of the first element for which `fn(element)` is truthy, or `-1` if no element matches. Short-circuits on first match.  
**Time:** O(n × cost(fn))

**Example:**
```omscript
fn gt_10(x: int) -> int { return x > 10; }

var a = [5, 15, 25];
var idx = array_find_index(a, gt_10);  // 1  (index of first element where gt_10 is true)
var miss = array_find_index([1, 2, 3], gt_10); // -1 (none pass)
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

#### `array_min_by(array, fn) → i64`

**Signature:** `array_min_by(array, function) → i64`  
**Semantics:** Return the element `e` in `array` for which `fn(e)` produces the smallest value. If multiple elements produce the same minimum key, the first one encountered is returned. Returns `0` if the array is empty.  
**Time:** O(n * cost(fn))

```omscript
fn last_digit(x: int) -> int { return x % 10; }

var a = [15, 3, 27, 9, 42];
// last digits: 5, 3, 7, 9, 2  →  minimum key is 2  →  element is 42
var m = array_min_by(a, last_digit);   // 42

// Block-body lambda form:
var m2 = array_min_by(a, |x: int| {
    var d = x % 10;
    return d;
});  // 42
```

---

#### `array_max_by(array, fn) → i64`

**Signature:** `array_max_by(array, function) → i64`  
**Semantics:** Return the element `e` in `array` for which `fn(e)` produces the largest value. If multiple elements produce the same maximum key, the first one encountered is returned. Returns `0` if the array is empty.  
**Time:** O(n * cost(fn))

```omscript
fn last_digit(x: int) -> int { return x % 10; }

var a = [15, 3, 27, 9, 42];
// last digits: 5, 3, 7, 9, 2  →  maximum key is 9  →  element is 9
var m = array_max_by(a, last_digit);   // 9
```

---

#### `array_flatten(array) → array`

**Signature:** `array_flatten(array) → array`  
**Semantics:** Flatten one level of array nesting. The outer array's elements must be integer values holding pointers to inner arrays (obtained by casting with `as int`). Returns a new array containing all elements from all inner arrays concatenated in outer-array order. Returns an empty array if the outer array is empty; empty inner arrays contribute zero elements.  
**Time:** O(total elements across all inner arrays)

```omscript
var a: int[] = [1, 2, 3];
var b: int[] = [4, 5];
var c: int[] = [6];

// Build outer array of pointers
var nested: int[] = [a as int, b as int, c as int];

var flat: int[] = array_flatten(nested);
// flat == [1, 2, 3, 4, 5, 6], len(flat) == 6
println(flat[0]);  // 1
println(flat[5]);  // 6
```

---

#### `array_chunk(array, i64) → array`

**Signature:** `array_chunk(array, chunk_size) → array`  
**Semantics:** Split an array into chunks of `chunk_size`. Returns an outer `int[]` where each element stores a sub-array pointer encoded as an integer; the final chunk may be shorter. `chunk_size` must be `>= 1`.  
**Time:** O(n)

**Example:**
```omscript
var a = [1, 2, 3, 4, 5];
var chunks = array_chunk(a, 2);
var flat = array_flatten(chunks);   // [1, 2, 3, 4, 5]
```

**Notes:**
- Use `array_flatten(chunks)` (or pass `chunks` into other array built-ins) when you need element-level access across all chunks.
- This representation is low-level by design: each outer element is an encoded pointer to a sub-array.

---

#### `array_flat_map(array, fn) → array`

**Signature:** `array_flat_map(array, function) → array`  
**Semantics:** Apply `fn` to every element of `array`, treat each result as a pointer to an inner array, then concatenate all inner arrays into a single output array. Equivalent to `array_flatten(array_map(array, fn))` but avoids the intermediate outer array allocation. Two-pass: pass 1 measures total output length, pass 2 copies elements.  
**Time:** O(n × cost(fn) + total output length)

```omscript
fn make_range(n: int) -> int {
    var arr = [n, n + 1];
    return arr as int;
}

var inp: int[] = [1, 2, 3];
var flat = array_flat_map(inp, make_range);
// flat == [1, 2, 2, 3, 3, 4], len(flat) == 6
```

---

#### `array_scan(array, init, fn) → array`

**Signature:** `array_scan(array, init, function) → array`  
**Semantics:** Prefix scan (running aggregate). Returns a new array of the same length as `array` where `result[0] = fn(init, array[0])`, `result[i] = fn(result[i-1], array[i])`. Useful for running totals, cumulative products, and any fold that retains intermediate values. Returns an empty array for empty input.  
**Time:** O(n × cost(fn))

```omscript
fn add(a: int, b: int) -> int { return a + b; }

var ns: int[] = [1, 2, 3, 4, 5];
var running = array_scan(ns, 0, add);
// running == [1, 3, 6, 10, 15]  (prefix sums)

fn mul(a: int, b: int) -> int { return a * b; }
var factorials = array_scan([1, 2, 3, 4], 1, mul);
// factorials == [1, 2, 6, 24]
```

---



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
- `\b` — backspace (0x08)
- `\f` — form feed (0x0C)
- `\v` — vertical tab (0x0B)
- `\\` — backslash
- `\"` — double quote
- `\xHH` — hex escape (two hex digits)

**Not supported in string literals**:
- `\0` (null byte) is rejected
- `\x00` (null byte via hex escape) is rejected

**Storage:** String literals are stored as LLVM global constants (`.rodata` section) and referenced via `GlobalString`.

---

### 12.3 String interpolation full rules

**Syntax:** Interpolation is supported with `$"..."` and `f"..."` forms. Expressions are embedded as `{...}`:
```omscript
var x = 42;
var s1 = $"The answer is {x}";
var s2 = f"The answer is {x}";  // alias form
```

**Implementation:**
1. The lexer/parser rewrites `$"..."` / `f"..."` into concatenation + conversion steps.
2. Multiple interpolations are chained left-to-right.
3. Nested expressions inside `{...}` are evaluated left-to-right.

**Supported expression types:**
- Integers → converted via `snprintf("%lld", ...)`
- Floats → converted via `snprintf("%g", ...)`
- Strings → used directly
- Other → calls `to_string()` if available

**Example:**
```omscript
var x = 10;
var y = 20;
println($"x={x}, y={y}, sum={x+y}");  // "x=10, y=20, sum=30"
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

#### `str_is_empty(string) → i64`

**Semantics:** Return `1` if the string has zero characters, `0` otherwise. Compile-time folded for literal arguments. Equivalent to `str_len(s) == 0` but more readable.  
**Time:** O(n) — delegates to `strlen()` to determine the length; no cached length header exists for strings.

**Example:**
```omscript
println(str_is_empty(""));       // 1
println(str_is_empty("hello"));  // 0
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

#### `str_capitalize(string) → string`

**Semantics:** Return a NEW string with the **first character** converted to uppercase and all remaining characters converted to lowercase. Compile-time folded for literal arguments. Empty string returns empty string.  
**Time:** O(n)

**Example:**
```omscript
println(str_capitalize("hello world"));  // "Hello world"
println(str_capitalize("HELLO"));        // "Hello"
println(str_capitalize(""));             // ""
```

---

#### `str_title(string) → string`

**Semantics:** Return a NEW string in "title case": the first character of each whitespace-separated word is uppercased, and all other characters are lowercased. Compile-time folded for literal arguments.  
**Time:** O(n)

**Example:**
```omscript
println(str_title("hello world"));  // "Hello World"
println(str_title("HELLO WORLD"));  // "Hello World"
```

---

#### `str_swapcase(string) → string`

**Semantics:** Return a NEW string with uppercase ASCII letters converted to lowercase and lowercase ASCII letters converted to uppercase. Non-alphabetic characters are unchanged. Compile-time folded for literal arguments.  
**Time:** O(n)

**Example:**
```omscript
println(str_swapcase("Hello World"));  // "hELLO wORLD"
println(str_swapcase("abc123"));       // "ABC123"
```

---

#### `str_words(string) → array`

**Semantics:** Split `string` on any whitespace (spaces, tabs, newlines), skipping empty tokens, and return the words as a `string[]`. Equivalent to Python's `str.split()` with no argument. Returns an empty array for an all-whitespace or empty string.  
**Time:** O(n)

**Example:**
```omscript
var words = str_words("  hello   world  ");
println(len(words));   // 2
println(words[0]);     // "hello"
println(words[1]);     // "world"
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

#### `str_to_lines(string) → string[]`

**Signature:** `str_to_lines(string) → string[]`  
**Semantics:** Split `string` at newline characters and return the resulting lines as an array of strings. Both Unix (`\n`) and Windows (`\r\n`) line endings are handled — any trailing `\r` is stripped from each line before it is stored. A trailing newline at the end of the input does **not** produce an empty final element (matches Python's `str.splitlines()` behaviour). Compile-time folded for string literals.  
**Time:** O(n)

**Example:**
```omscript
var text = "hello\nworld\nfoo";
var lines: string[] = str_to_lines(text);
println(len(lines));       // 3
println(lines[0]);         // hello
println(lines[2]);         // foo

// Trailing newline excluded:
var t2 = "alpha\nbeta\n";
var l2: string[] = str_to_lines(t2);
println(len(l2));          // 2  (no empty final element)

// CRLF line endings:
var t3 = "one\r\ntwo\r\nthree";
var l3: string[] = str_to_lines(t3);
println(len(l3));          // 3
println(l3[0]);            // one  (no trailing \r)
```

---

#### `str_remove_prefix(string, prefix) → string`

**Signature:** `str_remove_prefix(string, prefix) → string`  
**Semantics:** If `string` starts with `prefix`, return a copy of `string` with the first `len(prefix)` characters removed. Otherwise return `string` unchanged. Compile-time folded when both arguments are string literals.  
**Time:** O(len(prefix) + len(result)) — one `memcmp` + one `memcpy`

```omscript
println(str_remove_prefix("hello world", "hello "));  // "world"
println(str_remove_prefix("hello world", "bye "));    // "hello world" (no match)
println(str_remove_prefix("abc", "abcdef"));          // "abc" (prefix too long)
```

---

#### `str_remove_suffix(string, suffix) → string`

**Signature:** `str_remove_suffix(string, suffix) → string`  
**Semantics:** If `string` ends with `suffix`, return a copy of `string` with the last `len(suffix)` characters removed. Otherwise return `string` unchanged. Compile-time folded when both arguments are string literals.  
**Time:** O(len(suffix) + len(result)) — one `memcmp` + one `memcpy`

```omscript
println(str_remove_suffix("hello world", " world"));  // "hello"
println(str_remove_suffix("hello world", "xyz"));     // "hello world" (no match)
println(str_remove_suffix("abc", "abcdef"));          // "abc" (suffix too long)
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

### 12.9 String ↔ number conversion

#### `str_to_int(string) → i64`

**Signature:** `str_to_int(string) → i64`  
**Semantics:** Parse the string as a decimal (or hex with `0x`/`0X` prefix) integer. Returns 0 if the string is not a valid number.  
**Time:** O(n)

**Example:**
```omscript
var n: int = str_to_int("42");      // 42
var h: int = str_to_int("0xFF");    // 255
var bad: int = str_to_int("abc");   // 0
```

---

#### `str_to_float(string) → f64`

**Signature:** `str_to_float(string) → f64`  
**Semantics:** Parse the string as a 64-bit float. Returns 0.0 if not valid.  
**Time:** O(n)

**Example:**
```omscript
var f: float = str_to_float("3.14");   // 3.14
var g: float = str_to_float("1e5");    // 100000.0
```

---

#### `str_hex(i64) → string`

**Signature:** `str_hex(number) → string`  
**Semantics:** Format the integer as a lowercase hexadecimal string (no `0x` prefix). Value is formatted using 64-bit integer bits.  
**Time:** O(1)

**Example:**
```omscript
println(str_hex(255));   // "ff"
println(str_hex(16));    // "10"
println(str_hex(0));     // "0"
```

---

#### `str_bin(i64) → string`

**Signature:** `str_bin(number) → string`  
**Semantics:** Format the integer as a binary string with no leading zeros (except `0` itself).  
**Time:** O(64)

**Example:**
```omscript
println(str_bin(10));    // "1010"
println(str_bin(255));   // "11111111"
println(str_bin(0));     // "0"
```

---

#### `str_oct(i64) → string`

**Signature:** `str_oct(number) → string`  
**Semantics:** Format the integer as an octal string with no leading zeros (except `0` itself).  
**Time:** O(1)

**Example:**
```omscript
println(str_oct(8));     // "10"
println(str_oct(511));   // "777"
println(str_oct(0));     // "0"
```

---

### 12.10 String interning (when applies)

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

#### Literal syntax — `dict { ... }`

**Syntax:** `dict { key1: val1, key2: val2, ... }`  
**Status:** Fully implemented. The `dict { ... }` form is a keyword-prefixed dict literal equivalent to `{ key1: val1, key2: val2, ... }`. Both forms produce the same map value.

```omscript
var d = dict { "x": 10, "y": 20 };
println(map_get(d, "x", 0));  // 10
```

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

#### `map_copy(dict) → dict`

**Semantics:** Return a fully independent shallow copy of `dict`. All live key-value pairs are re-inserted into a newly allocated map. Modifications to the original after copying do not affect the copy and vice versa.  
**Time:** O(n)

**Example:**
```omscript
var m = map_new();
map_set(m, 1, 100);
var c = map_copy(m);
map_set(m, 2, 200);      // modifying original...
println(map_has(c, 2));  // 0  (copy is independent)
println(map_get(c, 1));  // 100
```

---

#### `map_clear(dict) → dict`

**Semantics:** Returns a fresh, independently allocated empty dictionary. Does **not** modify the input — the input dict and its memory are unaffected. The intended usage pattern is `m = map_clear(m);`, which rebinds `m` to the new empty map (the old map becomes unreachable). Semantically equivalent to `map_new()` but makes the "reset this variable" intent explicit at the call site.  
**Time:** O(1)

**Example:**
```omscript
var m = map_new();
map_set(m, 1, 100);
m = map_clear(m);
println(map_size(m));  // 0
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

**Attributes:** `@packed` is a supported shorthand for `@repr(packed)` — it emits a C-compatible packed LLVM struct type with no padding between fields.

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

#### Access: `s.field` or `p->field`

**Semantics:** Return the value of `field` in struct `s` (or via pointer `p`).  
**Implementation:** Load via `GEP` (GetElementPtr) with offset calculated from field index.

`p->field` is the C-style pointer member access operator, identical to `p.field` — OmScript automatically dereferences struct pointer variables in both cases.

**Example:**
```omscript
struct Point { x, y }
var p = Point { x: 10, y: 20 };
println(p.x);   // 10

var q: ptr<Point> = new Point { x: 3, y: 4 };
println(q->x);  // 3  (identical to q.x)
println(q->y);  // 4
invalidate q;
```

#### Mutation: `s.field = value` or `p->field = value`

**Semantics:** Store `value` into `field` of struct `s` (or via pointer `p`).  
**Implementation:** Store via `GEP`. Compound assignment (`p->x += n`) is also supported.

**Example:**
```omscript
p.x = 30;
println(p.x);   // 30

var r: ptr<Point> = new Point { x: 1, y: 2 };
r->x = 10;      // same as r.x = 10
r->y += 5;      // compound assignment through arrow
println(r->x);  // 10
println(r->y);  // 7
invalidate r;
```

#### Arrow method calls: `p->method(args)`

`p->method(args)` is identical to `p.method(args)` — both desugar to `method(p, args)`.

```omscript
fn Counter::increment(self) { self.count = self.count + 1; }

var c: ptr<Counter> = new Counter { count: 0 };
c->increment();     // same as c.increment()
println(c->count);  // 1
invalidate c;
```

---

### 14.5 Methods

**Syntax:** Methods are parsed as free functions with the struct name prefix:
```omscript
fn StructName::method(self, args...) {
    // body
}
```

**Status**: Methods are fully supported. Define with `fn StructName::method(self, args...)` and call with `obj.method(args)`.

Method calls (`obj.method(args)`) are **automatically desugared** to `StructName::method(obj, args)` — the receiver is inserted as the first argument.

```omscript
struct Counter { count }

fn Counter::increment(self) {
    self.count = self.count + 1;
}

fn Counter::add(self, n) {
    self.count = self.count + n;
}

fn Counter::get(self) {
    return self.count;
}

var c: ptr<Counter> = new Counter { count: 0 };
c.increment();   // desugars to Counter::increment(c)
c.add(5);        // desugars to Counter::add(c, 5)
println(c.get()); // desugars to Counter::get(c) → 6
```

Both call styles are accepted:
- `obj.method(args)` — dot-call syntax (recommended)
- `StructName::method(obj, args)` — explicit qualified call

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

**Semantics:** `invalidate s;` schedules a deferred free for the struct's heap allocation; the physical `free()` is emitted just before each function return.  
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

### 16.3 `assert(cond)` / `assert(cond, msg)`

**Syntax:** Built-in function, **1 or 2 arguments**:
```ebnf
assert_call ::= 'assert' '(' expression ')'
              | 'assert' '(' expression ',' expression ')'
```

**Semantics:**
1. Evaluates `cond` and coerces to a 1-bit boolean.
2. If true: returns `1` (the call evaluates as an expression to `1`).
3. If false:
   - **1-arg form**: prints `Runtime error: assertion failed at line N\n` to stdout.
   - **2-arg form**: prints `Runtime error: assertion failed at line N: <msg>\n` to stdout (where `<msg>` is the second argument evaluated as a string).
   - Calls `abort()` in both cases.

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
    assert(b != 0, "divisor must be non-zero");  // custom message on failure
    return a / b;
}

fn sqrt_safe(x: int) -> int {
    assert(x >= 0);   // line-number-only message
    return isqrt(x);
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
| Assertion failure | `assert(false)` / `assert(false, "msg")` | `Runtime error: assertion failed at line N[: msg]` |
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
| Semantic | Undefined variable / function, duplicate `catch` key, invalid `thread_create` target/signature, OPTMAX violation (§18.2) |
| Type | OPTMAX type-annotation requirement, integer-cast on incompatible type |
| Resource | Source file > 100 MB, parser nesting depth > 256, IR > 1,000,000 instructions |

**Runtime errors** — produced by checks inserted into the generated code. All of them are program-fatal (see §16.4).

**Recoverable vs non-recoverable:**
- **Recoverable (within a function):** `throw N;` matched by a local `catch(N)` block.
- **Non-recoverable:** every other runtime error class. Bounds, divide-by-zero, asserts, and unmatched throws all abort the process.

---

## 17. Memory and Ownership System

> **Ω Ownership & Memory System — Spec v1.0**
>
> A pure compile-time memory management model with deterministic ownership tracking, full pointer arithmetic support, CFG-based lifetime resolution, and zero runtime overhead memory safety (in safe mode).
>
> Core principle: **All memory safety is resolved at compile time** unless explicitly disabled by compiler flags.

---

### 17.0 Overview and Design Philosophy

OmScript's ownership system is designed as:

> *"C-level performance with Rust-level safety, but fully resolved at compile time using CFG-based lifetime computation and explicit invalidation semantics."*

**Key guarantees (safe mode):**
- No use-after-free
- No double-free (E019 compile error)
- No invalid dereference
- No memory leaks (when `invalidate` is used)
- Deterministic memory lifetime
- **Zero runtime overhead** — all ownership/safety checks eliminated before code generation

---

### 17.1 Ownership States

Every variable is in one of **seven** ownership states, tracked per-variable by the borrow checker:

| State | Description | Read | Write | Move |
| --- | --- | --- | --- | --- |
| `Owned` | Unique owner — full read/write access | ✓ | ✓ | ✓ |
| `Borrowed` | Has ≥1 immutable borrows — readable but not writable | ✓ | ✗ | ✗ |
| `MutBorrowed` | Has one mutable alias — source is completely locked | ✗ | ✗ | ✗ |
| `Shared` | Read-only aliasable ownership (Ω spec §3.1) | ✓ | ✗ | ✗ |
| `Frozen` | Permanently immutable — LLVM `!invariant` loads | ✓ | ✗ | ✓¹ |
| `Moved` | Ownership transferred out — **compile error on use** | ✗ | ✗ | ✗ |
| `Invalidated` | Explicitly killed — **compile error on use** | ✗ | ✗ | ✗ |

¹ Frozen variables may be moved; the destination becomes the new frozen owner.

**Per-variable tracking struct** (from `borrow_checker.h`):
```cpp
struct VarBorrowState {
    int  immutBorrows   = 0;    // number of active immutable borrows
    bool mutBorrowed    = false; // mutable borrow active
    bool moved          = false; // ownership transferred
    bool invalidated    = false; // explicitly freed
    bool frozen         = false; // permanently immutable
    bool shared         = false; // shared ownership state (Ω spec §3.1)
    int  reborrows      = 0;    // reborrow alias count
};
```

**State transitions:**
- `Owned` → `Borrowed` (via `borrow var r = x`)
- `Owned` → `MutBorrowed` (via `borrow mut var r = x`)
- `Owned` → `Shared` (via `shared x;`)
- `Owned`/`Shared` → `Frozen` (via `freeze x;`)
- `Owned` → `Moved` (via `move var y = x;`)
- Any live state → `Invalidated` (via `invalidate x;`)
- `Shared` → `Owned` (via `own x;`)

---

### 17.2 Move Semantics — `move var x = expr;`

**Syntax:**
```omscript
move var x = expr;   // declaration form
var y = move x;      // expression form
```

**Semantics:**
1. Evaluate `expr` and bind to `x` (or transfer `x` to `y`).
2. Mark the source as `Moved` — any subsequent use is a compile-time error.
3. No runtime cost: the value is bitwise-copied; no destructor runs.

**Example:**
```omscript
var a = 42;
move var b = a;
println(b);     // OK: 42
// println(a);  // [E015] use of moved variable 'a'
```

---

### 17.3 Borrow — `borrow var x = expr;`

**Syntax:**
```omscript
borrow var x = expr;
```

**Semantics:**
1. Create an immutable alias `x` for the source variable.
2. Increment `immutBorrows` of the source.
3. Source remains readable but NOT writable while the borrow is active.
4. When `x` goes out of scope, `immutBorrows` is decremented.

**Example:**
```omscript
var a = 42;
{
    borrow var ref = a;
    println(ref);   // 42
    println(a);     // 42 (source still readable)
    // a = 99;      // [E016] cannot write to 'a' — active immutable borrow
}
a = 99;  // OK — borrow ended
```

---

### 17.4 Borrow Mut — `borrow mut var x = expr;`

**Syntax:**
```omscript
borrow mut var x = expr;
```

**Semantics:**
1. Create a mutable alias `x` for the source variable.
2. Set `mutBorrowed = true` — source is completely locked (no reads or writes).
3. When `x` goes out of scope, `mutBorrowed` is cleared.

**Example:**
```omscript
var a = 42;
{
    borrow mut var ref = a;
    ref = 99;
    // println(a);  // [E016] cannot read mutably-borrowed variable 'a'
}
println(a);  // 99 — borrow ended; source reflects the write
```

---

### 17.5 Shared Ownership — `shared x;`

**Syntax:**
```omscript
shared x;          // transition to shared ownership
own x;             // restore unique ownership
```

**Semantics of `shared x;`** (Ω spec §3.1):
- Transitions `x` to the `Shared` state.
- Multiple immutable reads are allowed.
- Any write to `x` is a **compile-time error** (E020).
- Mutable borrow of `x` is forbidden.
- `shared x;` on a moved or invalidated variable is a compile-time error.

**Semantics of `own x;`** (Ω spec §3.1):
- Restores `x` to the `Owned` state (clears `shared = false`).
- Requires that no active borrows exist (E018 if borrows remain).
- `freeze` is stronger than `shared`: a frozen variable cannot be un-frozen by `own`.

**Example:**
```omscript
fn main() -> int {
    var x = 10;
    shared x;         // x is now shared (read-only)

    var r1 = x;       // OK: read
    var r2 = x;       // OK: second read
    // x = 99;        // [E020] cannot write to 'x' — shared ownership

    own x;            // restore unique ownership
    x = 35;           // OK: write
    return r1 + r2 + x;  // 55
}
```

---

### 17.6 `freeze x;` — Permanent Immutability

**Syntax:**
```omscript
freeze x;
```

**Semantics:**
1. Mark `x` permanently immutable (`frozen = true`).
2. All subsequent loads from `x` are annotated with LLVM `!invariant` metadata.
3. Any write to `x` is a compile-time error — even after scope re-entry.
4. **No runtime cost** — purely a compile-time annotation.
5. **`own x;` on a frozen variable is a compile-time error** (E021) — freeze is irreversible.

**Example:**
```omscript
var a = 42;
freeze a;
println(a);   // 42
// a = 99;    // [E005] cannot modify constant/frozen variable 'a'
// own a;     // [E021] cannot assert unique ownership of 'a' — it is frozen
```

**Difference from `shared`:**

| Property | `freeze` | `shared` |
| --- | --- | --- |
| Write blocked | ✓ permanent | ✓ until `own` |
| LLVM `!invariant` | ✓ | ✗ |
| Reversible | ✗ | ✓ via `own` |
| `own` allowed | ✗ (E021) | ✓ |
| `invalidate` allowed | ✓ | ✓ |

If you need a mutable copy of a frozen variable, declare a new variable:
```omscript
var x = 42;
freeze x;
var y = x;   // copy — y is independently owned and writable
y = 99;      // OK: y is a separate variable
```

---

### 17.7 `invalidate x;` — Explicit Deallocation

**Syntax:**
```omscript
invalidate x;
```

**Semantics:**
- Marks `x` as logically dead immediately (borrow checker removes it from the live set).
- Schedules `free(x)` at the optimal CFG exit point (deferred-free queue).
- Any subsequent use of `x` is a compile-time error (E015).
- **Double-invalidate** is a compile-time error (E019).
- **Invalidate while borrowed** is a compile-time error (E022) — freeing memory with active aliases would dangle the borrow.

**Loop safety:** `invalidate` is safe to call inside a loop body. The compiler captures the variable's *alloca* (which dominates the entire function) in the deferred-free queue rather than a loaded IR value. The physical `load` + `free()` pair is emitted at the function-exit block, where the alloca is always in scope. This avoids the LLVM IR domination violation that would occur if a loaded pointer defined in a loop body were used outside that body.

**Implementation note (deferred-free queue design):**
The queue is a flat `vector<{AllocaInst*, Type*}>` — a contiguous array of 16-byte pairs. At function exit, each entry is drained with a single `load`/`inttoptr`/`free` sequence. This gives:
- No per-invalidation heap allocations.
- No pointer chasing.
- Deterministic cleanup: all `free()` calls for a function are emitted together, giving the allocator a dense batch it can coalesce.
- Performance ≈ immediate free, with the added benefit of batched allocator coalescing.

**Which types are freed:**
| Type | Action |
| --- | --- |
| String | `free()` on string pointer |
| Heap array | `free()` on array pointer |
| Dict/map | Destructor + `free()` on wrapper |
| Struct | `free()` on struct pointer |
| Heap `ptr<T>` | `free()` if allocated via `malloc`/`alloc<T>` |
| BigInt | GMP `mpz_clear()` + `free()` on wrapper |
| Stack `ptr<T>` | `llvm.lifetime.end` intrinsic (no `free()`) |

**Example:**
```omscript
fn main() -> int {
    var p: ptr<i64> = alloc<i64>(1);
    *p = 99;
    var val = *p;
    invalidate p;
    // *p = 1;       // [E015] use of invalidated variable 'p'
    // invalidate p; // [E019] double invalidation of 'p'
    return val;      // 99
}
```

**Borrow constraint (E022):**
```omscript
fn main() -> int {
    var p: ptr<i64> = alloc<i64>(1);
    borrow var r = p;   // active borrow
    invalidate p;        // [E022] cannot invalidate 'p' — 1 immutable borrow(s) active
    return 0;
}
```

To correctly invalidate after borrowing, ensure all borrows end first (go out of scope):
```omscript
fn main() -> int {
    var p: ptr<i64> = alloc<i64>(1);
    {
        borrow var r = p;    // borrow is active in this block
        var val = *r;
    }                        // r goes out of scope → borrow released
    invalidate p;            // OK: no active borrows
    return 0;
}
```

---

### 17.8 `reborrow` — Chained Borrows

**Syntax:**
```omscript
reborrow var ref = source;
```

**Semantics:** Create a new immutable borrow from an existing borrow, incrementing `reborrows` of the source variable.

**Example:**
```omscript
var a = 42;
borrow var r1 = a;
reborrow var r2 = r1;
println(r2);  // 42
```

---

### 17.9 Pointer Types (`ptr`, `ptr<T>`)

#### 17.9.1 Core Pointer Operations

| Operation | Syntax | Description |
| --- | --- | --- |
| Declare | `var p: ptr<T> = ...` | Typed pointer variable |
| Address-of | `&x` | Take address of `x`; produces `ptr<T>` |
| Allocate | `alloc<T>(n)` / `new T(n)` | Allocate `n` elements of type `T` |
| Allocate 1 | `alloc<T>()` / `new T` | Allocate exactly 1 element (Ω spec §4.1) |
| Allocate + init | `new T { field: val, ... }` | Allocate one `T` and initialise its fields |
| Dereference read | `*p` | Load value through pointer |
| Dereference write | `*p = v` | Store value through pointer (Ω spec §4.2) |
| Arithmetic | `p + n`, `p - n` | Advance by `n * sizeof(T)` (Ω spec §4.4) |
| Null | `null`, `nullptr` | Zero-address pointer (Ω spec §2.2) |
| Free | `invalidate p` | Deferred `free()` at CFG exit |
| In-place init | `construct p { field: val, ... };` | Initialise fields of already-allocated `ptr<T>` |

#### 17.9.2 `alloc<T>` — Raw Compile-Time Smart Allocator

`alloc<T>(n)` decides allocation strategy entirely at **compile time** — no runtime branching is emitted. Returns **raw (uninitialised)** memory. Three tiers:

| Tier | Condition | Strategy | Notes |
| --- | --- | --- | --- |
| **T1 Stack** | Constant `n` AND `n × sizeof(T) ≤ 8 KiB` | `alloca` in entry block | SROA/mem2reg eligible; `lifetime.start`/`end` scoped |
| **T2 Arena** | Constant `n` AND fits in 64 KiB per-function slab | GEP into shared static slab | Zero heap involvement; all sub-allocations are compile-time GEPs |
| **T3 Heap** | Dynamic `n` OR size exceeds slab | `malloc` / `aligned_alloc` | `nonnull` + `dereferenceable(N)` + alignment `llvm.assume` annotations emitted for LLVM AA |

`alloc<T>()` (no argument) allocates exactly 1 element.

**Safety improvements in T3 (heap)**:
- Return pointer annotated `nonnull` — LLVM may assume malloc succeeds.
- When `n` is a compile-time constant, `dereferenceable(n × sizeof(T))` is emitted — enables load-store forwarding and alias analysis.
- `llvm.assume(ptr != null)` and `llvm.assume(ptr % alignof(T) == 0)` are emitted — vectorizer and IndVars can exploit alignment without runtime checks.

```omscript
var arr: ptr<i64> = alloc<i64>(4);   // T1: stack alloca, raw (uninitialised)
var big: ptr<i64> = alloc<i64>(2048); // T2: arena GEP, raw
var one: ptr<i64> = alloc<i64>();    // 1 element, raw
```

---

#### 17.9.2a `new T(n)` — Zero-Initialised Allocation

`new T(n)` is semantically distinct from `alloc<T>(n)`. It uses the same three-tier compile-time allocation strategy but **guarantees zero-initialised memory** — all bytes are set to zero before the pointer is returned.

For **struct element types**, `new T(n)` goes one step further than a flat `memset`: it emits explicit per-field typed stores (with per-field TBAA metadata) so the optimizer can see each field write individually and apply SROA, copy propagation, and alias analysis more aggressively.

| Tier | Zero-init mechanism |
| --- | --- |
| **T1 Stack** | `alloca` + typed per-field `store zero` for structs; `memset(ptr, 0, n×sizeof(T))` for scalars |
| **T2 Arena** | arena GEP + same field-by-field zero stores for structs; `memset` for scalars |
| **T3 Heap** | `calloc(n, sizeof(T))` — OS-level zeroing, no extra call |

`new T` (no parens) zero-initialises exactly 1 element.

```omscript
var arr: ptr<i64> = new i64(4);    // T1: zero-initialised (all 0)
fn dyn(n: int) -> ptr<i64> {
    return new i64(n);             // T3: calloc (n is dynamic)
}
var one: ptr<i64> = new i64;       // 1 element, zero-initialised

struct Point { x, y }
var p: ptr<Point> = new Point;     // T1: both fields zero-initialised via typed stores
var pts: ptr<Point> = new Point(3);// T1: all 3 elements, all fields zero
```

**`sizeof(T)` for struct types** uses the full LLVM struct layout (all fields + padding), not pointer size. `new Point(1)` where `Point = {i64, i64}` allocates 16 bytes, not 8.

**When to use `alloc<T>` vs `new T`:**

| Need | Use |
| --- | --- |
| Raw speed, you will write every field before reading | `alloc<T>(n)` |
| Safety by default — zero is a valid sentinel / default | `new T(n)` |
| Allocate + initialise specific fields immediately | `new T { field: val, ... }` |
| Allocate many + initialise one at a time | `alloc<T>(n)` + `construct p { ... };` |

---

#### 17.9.2b `construct` Statement — In-Place Field Initialisation

`construct ptr { field: val, ... };` initialises the fields of an already-allocated struct pointer in place. It lowers with **zero abstraction cost** to a sequence of typed `GEP` + `store` pairs — exactly what writing `ptr->x = val; ptr->y = val;` by hand would produce, but with automatic per-field TBAA metadata and correct alignment from the struct definition.

**Syntax:**
```
construct TARGET { FIELD: EXPR [, FIELD: EXPR]* [,] };
```

Where `TARGET` is any expression returning a `ptr<T>` and `FIELD: EXPR` pairs provide field initialisers.  Trailing commas are allowed.  Fields not listed are **left as-is** (uninitialised if from `alloc<T>`, zero if from `new T`).

**Example:**
```omscript
struct Point { x: int; y: int; }

fn make_point(x: int, y: int) -> ptr<Point> {
    var p: ptr<Point> = alloc<Point>(1);  // raw memory
    construct p {
        x: x,
        y: y,
    };
    return p;
}
```

**Lowering** (no extra IR, identical to hand-written field stores):
```
%construct.field.ptr = getelementptr inbounds %Point, ptr %p, i32 0, i32 0
store i64 %x, ptr %construct.field.ptr, align 8, !tbaa !...
%construct.field.ptr1 = getelementptr inbounds %Point, ptr %p, i32 0, i32 1
store i64 %y, ptr %construct.field.ptr1, align 8, !tbaa !...
```

**Any expression is valid as `TARGET`:**
```omscript
// Construct into the result of pointer arithmetic
construct (arr + i) { x: 5, y: 10 };
```

---

#### 17.9.2c `new T { ... }` Expression — Allocation + Field Construction

`new T { field: val, ... }` is an **expression** that combines `alloc<T>(1)` (raw allocation) with an immediate `construct` statement.  It returns a `ptr<T>` with exactly the listed fields initialised and unlisted fields **uninitialised**.

Use `new T { ... }` when you want to initialise exactly the fields you need and skip the rest for maximum performance. Use `new T(1)` + `construct` if you prefer separate alloc and init steps.

**Allocation + field comparison:**

| Form | Allocates | Zero-fills all | Initialises specific fields | Count |
| --- | --- | --- | --- | --- |
| `alloc<T>(n)` | ✓ | ✗ | ✗ | `n` |
| `new T(n)` | ✓ | ✓ | ✗ | `n` |
| `new T { f:v, ... }` | ✓ | ✗ | ✓ listed only | 1 |
| `construct p { f:v, ... };` | ✗ | ✗ | ✓ listed only | existing |

**Example:**
```omscript
struct Point { x: int; y: int; }

fn make_point(x: int, y: int) -> ptr<Point> {
    return new Point { x: x, y: y };
}

fn main() -> int {
    var p: ptr<Point> = new Point { x: 3, y: 4 };
    println(p->x);   // 3
    println(p->y);   // 4
    invalidate p;
    return 0;
}
```

**Lowering** (identical to `alloc<Point>(1)` + `construct`):
```
%ptr = call nonnull ptr @malloc(i64 16)
%construct.field.ptr = getelementptr inbounds %Point, ptr %ptr, i32 0, i32 0
store i64 3, ptr %construct.field.ptr, align 8, !tbaa !...
%construct.field.ptr1 = getelementptr inbounds %Point, ptr %ptr, i32 0, i32 1
store i64 4, ptr %construct.field.ptr1, align 8, !tbaa !...
; %ptr is the result — no extra overhead
```

#### 17.9.3 Pointer Arithmetic

`ptr<T>` arithmetic is type-aware: `p + n` advances by `n * sizeof(T)` bytes.

```omscript
fn main() -> int {
    var arr: ptr<i64> = alloc<i64>(3);
    *arr       = 10;
    *(arr + 1) = 20;
    *(arr + 2) = 47;
    var result = *arr + *(arr + 1) + *(arr + 2);  // 77
    invalidate arr;
    return result;
}
```

#### 17.9.4 Null Pointers

Both `null` and `nullptr` produce a zero-address `ptr<T>`:
```omscript
var p: ptr<i64> = null;     // zero pointer
var q: ptr<i64> = nullptr;  // identical (Ω spec §2.2)
if (p == q) { println("both null"); }
```

#### 17.9.5 Allowed Pointer Initializers

| Initializer | Example | Valid |
| --- | --- | --- |
| Address-of | `&x` | ✓ |
| `alloc<T>()` | `alloc<i64>(4)` | ✓ |
| `malloc`/`calloc`/`realloc` | `malloc(100)` | ✓ |
| Another ptr variable | `q` | ✓ |
| Function returning ptr | `get_buf()` | ✓ |
| `null` / `nullptr` | `null` | ✓ |
| Integer literal | `42` | ✗ (error) |

---

### 17.10 `prefetch x;`

**Variable-level prefetch:** Emit `llvm.prefetch` intrinsic to load the address of `x` into cache.

**Use-site annotation:** `@prefetch` on a loop annotation (see §8.12).

---

### 17.11 No-Aliasing Guarantee

When ownership analysis proves a pointer does not alias any other, LLVM `noalias` metadata is attached to loads/stores and function arguments:

- `Owned` variables: always `noalias` (no aliases exist).
- `MutBorrowed` variables: exclusive access (`noalias` with all other pointers).

---

### 17.12 Scope-Based Drop

**Not automatically implemented.** OmScript does NOT automatically call `invalidate` at scope exit. Heap allocations persist until explicitly invalidated or the program terminates.

The compiler does, however, emit `llvm.lifetime.end` for stack-allocated `alloc<T>` at scope exit as an LLVM optimization hint.

---

### 17.13 Pointer Conversions

| Conversion | Legality | Notes |
| --- | --- | --- |
| `ptr` → `int` | Implicit | Pointer values stored as i64 |
| `int` → `ptr` | Explicit (`IntToPtr`) | Reconstruct pointer from i64 |
| `ptr<T>` → `ptr<U>` | Allowed | All ptrs are opaque at LLVM IR level |

---

### 17.14 Borrow Checker Error Codes

| Code | Name | Trigger |
| --- | --- | --- |
| E015 | `USE_AFTER_MOVE` | Read/write of moved or invalidated variable |
| E016 | `BORROW_WRITE_CONFLICT` | Write to variable with active immutable borrow(s) |
| E017 | `DOUBLE_MUT_BORROW` | Mutable borrow of already mutably-borrowed variable |
| E018 | `MOVE_WHILE_BORROWED` | Move/`own` of variable with active borrow(s) |
| E019 | `DOUBLE_INVALIDATE` | `invalidate` on already-invalidated variable |
| E020 | `WRITE_TO_SHARED` | Write to variable in `shared` ownership state |
| E021 | `OWN_ON_FROZEN` | `own` on a `frozen` variable — freeze is irreversible |
| E022 | `INVALIDATE_WHILE_BORROWED` | `invalidate` while active borrow(s) exist on the variable |

**Design notes:**
- E019 and E022 together make `invalidate` fully safe: no double-free (E019) and no dangling alias (E022).
- E021 enforces that `freeze` is strictly stronger than `shared`: once frozen, ownership cannot be reclaimed via `own`.
- All error codes are compile-time only — zero runtime overhead.

---

### 17.15 Ownership Constraint Summary

The following matrix summarizes which operations are permitted in each ownership state:

| Operation | `Owned` | `Borrowed` | `MutBorrowed` | `Shared` | `Frozen` | `Moved` | `Invalidated` |
| --- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| Read | ✓ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ |
| Write | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| `move` | ✓ | ✗ | ✗ | ✗ | ✓¹ | ✗ | ✗ |
| `invalidate` | ✓ | ✗ | ✗ | ✓ | ✓ | ✗ | ✗ |
| `freeze` | ✓ | ✗ | ✗ | ✓ | —² | ✗ | ✗ |
| `shared` | ✓ | ✓ | ✗ | —² | ✓ | ✗ | ✗ |
| `own` | —² | ✗ | ✗ | ✓ | ✗³ | ✗ | ✗ |
| `borrow` | ✓ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ |
| `borrow mut` | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |

¹ Frozen variables may be moved; the destination is a new frozen owner.  
² No-op / already in that state.  
³ E021 — freeze is irreversible; `own` cannot downgrade a frozen variable.

**Error triggered on invalid operation:**
- Write to `Borrowed` → E016; write to `Shared` → E020; write to `Frozen`/`Moved`/`Invalidated` → E005/E015
- `invalidate` on `Moved` → E015; on `Invalidated` → E019; on `Borrowed`/`MutBorrowed` → E022
- `own` on `Frozen` → E021; `own` on `Borrowed` → E018

---

### 17.16 Safety Mode Flags

#### `--no-ownership-checks` (Ω spec §6.2)

Disables the borrow checker entirely. The compiler treats memory as a raw C-like model — no borrow tracking, no invalidation checks, no safety diagnostics.

```
omsc myprogram.om --no-ownership-checks -o out
```

**Use case:** Interoperating with C-style code, performance benchmarking, or code that manages memory manually.

#### `--mem-sanitize` (Ω spec §7)

Enables the compile-time path-sensitive memory sanitizer. Detects and reports:
- Use-after-invalidate
- Invalid dereference paths
- Potential out-of-bounds pointer arithmetic
- Double free
- Null dereference possibilities

Output format:
```
MEM-SANITIZER ERROR
file.om:42

use-after-invalidate detected
variable: p
control path:
  line 38 -> invalidate p
  line 41 -> *p (invalid use)
```

**Properties:**
- Compile-time only — zero runtime cost
- Path-sensitive (CFG-based): reports *possible* UB, not just certain UB
- Does not affect code generation; purely diagnostic

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

Print value followed by newline to stdout (same behavior as `print`). Returns 0. Both `print` and `println` add a trailing newline; use `write` to print without one.

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

Core math built-ins. All functions operate on `i64` (integer) or `f64` (float) arguments unless noted; see §19.5.2 for type-conversion functions.

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

Bit-manipulation built-ins for `i64` operands. All return `i64`.

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

#### `typeof(any) → int` *(deprecated)*

**Deprecated** — emit a compile-time integer tag based on the static type of the argument: `1` = integer, `2` = float, `3` = string. This function is resolved entirely at compile time from static type information; it does not perform any runtime type query. Use explicit type annotations instead.

> **Migration**: Replace `if (typeof(x) == 2)` guards with `type_name(x)` comparisons or properly typed function overloads. `typeof` will be removed in a future version.

**Example (legacy):**
```omscript
println(typeof(42));      // 1  (compile-time constant; argument evaluated for side effects only)
println(typeof(3.14));    // 2
println(typeof("hello")); // 3
```

---

#### `type_name(any) → string`

Return a human-readable **compile-time string** describing the OmScript type of the argument expression. Resolved purely from static LLVM IR type information — zero runtime overhead. The argument is evaluated for side-effects but the produced value is not used at runtime.

| Return value | Meaning |
| --- | --- |
| `"int"` | Any integer type: `i64`, `i32`, `i16`, `i8`, `u64`, `u32`, `u8`, `byte`, … |
| `"float"` | Double-precision float (`f64`) |
| `"f32"` | Single-precision float |
| `"bool"` | Boolean (`bool`-annotated variable or `i1` value) |
| `"string"` | OmScript fat-pointer string |
| `"array"` | Array / slice pointer |
| `"dict"` | Hash-map pointer |
| `"ptr"` | Raw or struct pointer |
| `"simd"` | LLVM fixed-width vector (SIMD type) |
| `"void"` | Void / no-value |
| `"unknown"` | Anything not in the above categories |

**Example:**
```omscript
import std;

var iv: int    = 7;
var fv: float  = 1.5;
var sv: string = "hello";
var bv: bool   = true;

println(type_name(42));   // "int"
println(type_name(3.14)); // "float"
println(type_name("hi")); // "string"
println(type_name(iv));   // "int"
println(type_name(fv));   // "float"
println(type_name(sv));   // "string"
println(type_name(bv));   // "bool"

if str_eq(type_name(iv), "int") {
    println("iv is an integer");
}
```

---

#### `sizeof(type_name) → i64`

Return the byte size of a type as a compile-time constant.

**Example:**
```omscript
println(sizeof(int));    // 8
println(sizeof(float));  // 8
println(sizeof(f32));    // 4
println(sizeof(i32));    // 4
println(sizeof(i128));   // 16
println(sizeof(bool));   // 1
println(sizeof(byte));   // 1
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

### 19.5.2 Value conversion functions

#### `to_string(any) → string`

Convert any value to its string representation.

- Integer → decimal string via `snprintf("%lld", ...)`.
- Float → string via `snprintf("%g", ...)`.
- String → returned as-is.

**Example:**
```omscript
var s: string = to_string(42);       // "42"
var f: string = to_string(3.14);     // "3.14"
```

---

#### `to_int(any) → i64`

Convert a value to an integer.

- Float → truncates via `fptosi`.
- String → parse as decimal (same as `str_to_int`).
- Integer → identity.

**Example:**
```omscript
var n: int = to_int(3.7);     // 3 (truncated)
var m: int = to_int("42");    // 42
```

---

#### `to_float(any) → f64`

Convert a value to a 64-bit float.

- Integer → exact conversion via `sitofp`.
- String → parse as float (same as `str_to_float`).
- Float → identity.

**Example:**
```omscript
var f: float = to_float(10);    // 10.0
var g: float = to_float("3.14"); // 3.14
```

---

#### `to_char(i64) → string`

Convert an ASCII code to a single-character string.

**Example:**
```omscript
var c: string = to_char(65);   // "A"
var d: string = to_char(97);   // "a"
```

---

#### `char_code(string) → i64`

Return the ASCII integer code of the first character of the string.

**Example:**
```omscript
var code: int = char_code("A");   // 65
var code2: int = char_code("hello"); // 104 (code of 'h')
```

---

#### `number_to_string(i64) → string`

Format an integer as a decimal string. Equivalent to `to_string` for integer inputs.

**Example:**
```omscript
var s: string = number_to_string(12345);   // "12345"
```

---

#### `string_to_number(string) → i64`

Parse a decimal string as an integer. Equivalent to `str_to_int` for decimal strings.

**Example:**
```omscript
var n: int = string_to_number("12345");   // 12345
```

---

#### `str_to_int(string) → i64`

Parse a decimal (or hex with `0x` prefix) string as a signed 64-bit integer. Returns 0 if the string is not a valid number.

**Example:**
```omscript
var n: int = str_to_int("42");     // 42
var h: int = str_to_int("0xFF");   // 255
var z: int = str_to_int("abc");    // 0
```

---

#### `str_to_float(string) → f64`

Parse a string as a 64-bit floating-point value. Returns 0.0 if not valid.

**Example:**
```omscript
var f: float = str_to_float("3.14");   // 3.14
var g: float = str_to_float("1e5");    // 100000.0
```

---

### 19.5.3 Random number generation

#### `random() → i64`

Return a pseudo-random non-negative integer. Uses the C `rand()` function, seeded once per process with `srand(time(NULL))`. The seed is set on the first call.

**Range:** `[0, RAND_MAX]` (typically `[0, 2147483647]` on POSIX).

**Example:**
```omscript
var r: int = random();
println(r);

var die: int = random() % 6 + 1;   // simulated dice roll [1, 6]
```

---

### 19.5.4 Character classification predicates

These functions test a single character passed as an ASCII integer code (as returned by `char_code` or `char_at`). All return `1` (true) or `0` (false).

#### `is_alpha(i64) → bool`

Return 1 if the character is a letter (`a-z` or `A-Z`).

---

#### `is_digit(i64) → bool`

Return 1 if the character is a decimal digit (`0-9`).

---

#### `is_upper(i64) → bool`

Return 1 if the character is an uppercase letter (`A-Z`).

---

#### `is_lower(i64) → bool`

Return 1 if the character is a lowercase letter (`a-z`).

---

#### `is_space(i64) → bool`

Return 1 if the character is whitespace (space, tab, newline, carriage return, etc.).

---

#### `is_alnum(i64) → bool`

Return 1 if the character is alphanumeric (`a-z`, `A-Z`, or `0-9`).

**Example:**
```omscript
var c: int = char_code("A");
println(is_alpha(c));    // 1
println(is_upper(c));    // 1
println(is_digit(c));    // 0
println(is_alnum(c));    // 1

var d: int = char_code("5");
println(is_digit(d));    // 1
println(is_alpha(d));    // 0
```

---

### 19.5.5 Range array constructors

#### `range(start: i64, end: i64) → array`

Return a new array `[start, start+1, ..., end-1]` (exclusive upper bound). Length is `max(0, end - start)`.

**Example:**
```omscript
var a: int[] = range(0, 5);    // [0, 1, 2, 3, 4]
var b: int[] = range(3, 7);    // [3, 4, 5, 6]
```

---

#### `range_step(start: i64, end: i64, step: i64) → array`

Return a new array `[start, start+step, start+2*step, ...]` stopping before `end`. `step` must be positive.

**Example:**
```omscript
var evens: int[] = range_step(0, 10, 2);   // [0, 2, 4, 6, 8]
var tens: int[] = range_step(10, 50, 10);  // [10, 20, 30, 40]
```

---

### 19.5.6 HTTP client builtins

The HTTP builtins perform synchronous HTTP requests via libcurl (linked at compile time when the `http_*` builtins are used). All functions return a heap-allocated string.

#### `http_get(url: string) → string`

Perform an HTTP GET request and return the response body.

**Example:**
```omscript
var body: string = http_get("https://example.com/api/data");
println(body);
```

---

#### `http_post(url: string, body: string) → string`

Perform an HTTP POST request with `body` as the request body (Content-Type: `application/x-www-form-urlencoded`). Returns the response body.

**Example:**
```omscript
var resp: string = http_post("https://example.com/submit", "key=value");
println(resp);
```

---

#### `http_post(url: string, body: string, content_type: string) → string`

Overload with explicit Content-Type header.

**Example:**
```omscript
var resp: string = http_post("https://api.example.com/json", "{\"key\":\"val\"}", "application/json");
```

---

#### `http_request(method: string, url: string, body: string, headers: string) → string`

Full-control HTTP request. `method` is the HTTP verb (`"GET"`, `"POST"`, `"PUT"`, `"DELETE"`, etc.). `headers` is a newline-separated list of `Header: Value` strings. Returns the response body.

**Example:**
```omscript
var resp: string = http_request("PUT", "https://api.example.com/item/1",
                                "{\"name\":\"test\"}", "Content-Type: application/json");
```

---

#### `http_status(url: string) → i64`

Perform an HTTP GET and return only the HTTP status code (e.g., `200`, `404`).

**Example:**
```omscript
var code: int = http_status("https://example.com");
if code == 200 {
    println("OK");
} else {
    println("Error: " + to_string(code));
}
```

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

**Built-in namespace:** Every standard library function is accessible as `std::name`. Standard library functions are also accessible by their **bare names without any import statement** — `import std;` is optional and stylistic.

**Dispatch rules:**
- `std::abs(x)` resolves to the same function as `abs(x)`.
- Bare names always work without `import std;` because builtins are registered globally.
- `import std;` is a no-op for the standard library — it is provided only as a readable marker of intent in files that exclusively use stdlib.

**List of std:: symbols:**

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
| --- | --- |
| String / array / dict / struct | The managed type (no manual free; see §17) |
| Many short-lived allocations with a clear lifetime | `newRegion` + `alloc` (bulk free at scope exit) |
| Long-lived, irregular lifetime | `malloc` + `free` |
| Inter-op with C ABI requiring a raw buffer | `malloc` |

---

## 20. Concurrency

OmScript's concurrency model is a thin layer over the host's POSIX threading primitives (`pthread_*`). All thread and mutex handles are passed around as plain `i64` values that wrap the underlying `pthread_t` / `pthread_mutex_t*`. There is no managed thread pool, no async runtime, no green threads, and no garbage-collection-aware safe-point machinery — threads run to completion, and the user is responsible for joining them and freeing mutexes.

### 20.0 Threading keyword sugar

OmScript provides direct keyword sugar for common thread and mutex operations:

| Keyword form | Desugars to |
| --- | --- |
| `spawn target()` | `thread_create(target)` |
| `spawn target(arg)` | `thread_create(target, arg)` |
| `spawn(target[, arg])` | `thread_create(target[, arg])` |
| `join x` | `thread_join(x)` |
| `detach x` | `thread_detach(x)` |
| `lock m` | `mutex_lock(m)` |
| `unlock m` | `mutex_unlock(m)` |
| `trylock m` | `mutex_try_lock(m)` |

`target` follows the same validation rules as `thread_create`: identifier or string literal naming an existing top-level function with arity 0 or 1.

`spawn` intentionally requires **call-shaped syntax** for compact target forms:

- ✅ `spawn worker()`
- ✅ `spawn worker(arg)`
- ❌ `spawn worker`

**Example:**
```omscript
var m = mutex_new();
lock m;
var t = spawn worker_add(5);
var r = join t;
var busy = trylock m;
unlock m;
detach spawn worker();
mutex_destroy(m);
```

**Model summary:**

| Concept | Backing primitive | Handle type |
| --- | --- | --- |
| Thread | `pthread_create` / `pthread_join` | `i64` (pthread_t) |
| Mutex | `pthread_mutex_*` | `i64` (`pthread_mutex_t*` cast to int) |
| Parallel loop | `@parallel for` → loop-parallelism metadata | n/a |
| Atomics | `atomic var` qualifier | `i64` (seq-cst loads/stores/RMW) |

### 20.1 Threads

#### 20.1.1 `thread_create(fn_name: string|identifier [, arg: int]) → i64`

**Description:** Spawn a new OS thread that calls a top-level OmScript function. `fn_name` can be either:
- a string literal (`"worker"`)
- a function identifier (`worker`)

The target function must accept **0 or 1 parameter(s)**:
- 0-arg target: call as `thread_create(worker)`
- 1-arg target: call as `thread_create(worker, arg)`

`thread_create` checks the target signature at compile time and emits an error for unsupported arity.

**Returns:** A thread handle (`pthread_t` reinterpreted as `i64`). Pass this value to `thread_join`.

**Errors:**
- *Compile-time:* `thread_create requires a function name (identifier or string literal) as argument 1`
- *Compile-time:* `thread_create: unknown function 'foo'` if the function does not exist in the current translation unit.
- *Compile-time:* arity mismatch errors when target takes 0 or 1 arg but call shape does not match.
- *Runtime:* fatal diagnostic + abort if `pthread_create` fails.

**Example:**
```omscript
global var counter: int = 0;
global var lock: int = 0;  // initialized in main()

fn worker() -> int {
    mutex_lock(lock);
    counter = counter + 1;
    mutex_unlock(lock);
    return 1;
}

fn worker_add(v: int) -> int {
    return v + 10;
}

fn main() {
    lock = mutex_new();
    var t1 = thread_create(worker);
    var t2 = thread_create("worker_add", 32);
    var r1 = thread_join(t1);     // 1
    var r2 = thread_join(t2);     // 42
    mutex_destroy(lock);
    println(counter);   // 1
    println(r1 + r2);   // 43
    return 0;
}
```

---

#### 20.1.2 `thread_join(tid: i64) → i64`

**Description:** Block the calling thread until the thread identified by `tid` has terminated.

**Returns:** The worker function's integer return value (`i64`).

**Errors:** Runtime fatal diagnostic + abort if `pthread_join` fails (invalid/already-joined handle, etc.).

**Example:**
```omscript
var t = thread_create("worker");
var result = thread_join(t);
```

---

#### 20.1.3 `thread_detach(tid: i64) → i64`

**Description:** Mark a thread as detached. Detached threads release their resources automatically on completion and must not be joined.

**Returns:** `0` on success.

**Errors:** Runtime fatal diagnostic + abort if `pthread_detach` fails.

**Example:**
```omscript
var t = thread_create(worker);
thread_detach(t);
```

---

### 20.2 Mutexes

Mutexes are heap-allocated `pthread_mutex_t` structures, returned as opaque `i64` handles. The allocation is sized at **64 bytes** — enough to cover `pthread_mutex_t` on every supported platform (40 bytes on Linux x86_64, 64 bytes on macOS) — and initialized via `pthread_mutex_init` with default attributes. Default-attribute mutexes are **non-recursive**: a thread that locks the same mutex twice without unlocking deadlocks.

> **Note:** OmScript now exposes `mutex_try_lock` in addition to lock/unlock/destroy. There are still no read/write locks or condition variables in the core builtins.

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

#### 20.2.4 `mutex_try_lock(m: i64) → i64`

**Description:** Attempt to acquire `m` without blocking.

**Returns:**
- `1` if the mutex was acquired
- `0` if the mutex is currently busy

**Errors:** Runtime fatal diagnostic + abort for non-busy pthread errors.

---

#### 20.2.5 `mutex_destroy(m: i64) → i64`

**Description:** Destroy the mutex via `pthread_mutex_destroy` and free its backing allocation. After this call the handle `m` is invalid and **must not be used**.

**Returns:** Always `0`.

**Errors:** Destroying a locked mutex is undefined behaviour at the pthread level.

---

### 20.3 Atomics

OmScript provides first-class **atomic variables** via the `atomic` variable qualifier. An atomic variable's every load, store, and read-modify-write operation is emitted as a sequentially-consistent (seq-cst) LLVM atomic instruction — no mutex required.

#### 20.3.1 `atomic var` Declaration

**Syntax**: `atomic var name: type = initializer;`

**IR mapping**:

| OmScript source | LLVM IR |
| --- | --- |
| `atomic var x: i64 = 0;` | `store atomic i64 0, ptr %x seq_cst` (initializer) |
| `x` (load) | `load atomic i64, ptr %x seq_cst` |
| `x = v` | `store atomic i64 %v, ptr %x seq_cst` |
| `x++` / `x--` | `atomicrmw add/sub ptr %x, i64 1 seq_cst` |
| `x += v` | `atomicrmw add ptr %x, i64 %v seq_cst` |
| `x -= v` | `atomicrmw sub ptr %x, i64 %v seq_cst` |
| `x &= v` | `atomicrmw and ptr %x, i64 %v seq_cst` |
| `x \|= v` | `atomicrmw or ptr %x, i64 %v seq_cst` |
| `x ^= v` | `atomicrmw xor ptr %x, i64 %v seq_cst` |

**Memory ordering**: All operations use `seq_cst` (sequentially consistent), the strongest LLVM ordering. This ensures a single total order of all seq-cst atomic operations across all threads — the same guarantee as `std::memory_order_seq_cst` in C++.

**Example — shared counter without a mutex**:
```omscript
global atomic var hits: i64 = 0;

fn worker() {
    hits++;   // atomicrmw add seq_cst — safe from multiple threads
    return 0;
}

fn main() {
    var t1 = thread_create("worker");
    var t2 = thread_create("worker");
    thread_join(t1);
    thread_join(t2);
    println(hits);   // always 2
    return 0;
}
```

**Example — lock-free flag**:
```omscript
global atomic var ready: i64 = 0;

fn producer() {
    // … do work …
    ready = 1;    // store atomic seq_cst — visible to all threads
    return 0;
}

fn consumer() {
    while (ready == 0) {}   // load atomic seq_cst every iteration
    // … safe to proceed …
    return 0;
}
```

#### 20.3.2 `volatile var` and `atomic volatile var`

`volatile var` (see §5.6) prevents the compiler from eliding, hoisting, or CSE-ing loads and stores, but does **not** provide multi-thread atomicity. It is intended for memory-mapped I/O, signal handlers, and debugger-visible variables.

`atomic volatile var` (or `volatile atomic var`) applies both qualifiers simultaneously: operations are seq-cst atomic *and* volatile. This is the right choice for hardware registers that are also shared between an interrupt handler and main code.

---

### 20.4 Memory model

OmScript offers three tiers of shared-memory semantics:

| Mechanism | IR semantics | Ordering guarantee | Use case |
| --- | --- | --- | --- |
| Plain `var` | Unordered load / store | None — data race is UB | Single-threaded, or reads protected by a mutex |
| `atomic var` | `seq_cst` atomic load / store / RMW | Sequentially consistent — total order of all atomic ops | Lock-free counters, flags, producer/consumer handshakes |
| `mutex_lock` / `mutex_unlock` | Acquire / release barrier | Acquire-release — publishes all prior writes to the next lock holder | Critical sections protecting complex invariants (structs, arrays, etc.) |

**Plain variables and data races**:
Plain loads and stores compile to LLVM unordered memory operations. The compiler is permitted to reorder, cache, or eliminate them. Accessing a plain variable from multiple threads without synchronization is a **data race** and is undefined behaviour. Protect plain shared state with a mutex.

**`atomic var` and seq-cst ordering**:
`atomic var` operations use `seq_cst` (sequentially consistent) ordering — the strongest LLVM memory ordering. All seq-cst operations across all threads occur in a single total order visible to every thread. This is sufficient for most lock-free patterns. It is **not** necessary to use a mutex to protect a plain integer if it is declared `atomic`.

**`mutex_lock` / `mutex_unlock` and acquire/release**:
`mutex_lock` has acquire semantics (all writes by the previous lock-holder become visible before the lock is acquired). `mutex_unlock` has release semantics (all writes made inside the critical section are visible to the next lock-holder). This is inherited from the underlying `pthread_mutex_*` implementation and is sufficient to publish writes made inside a critical section.

**Choosing between `atomic var` and a mutex**:
- Use `atomic var` for a single integer or pointer that is updated by one or more threads (counters, flags, indices, generation numbers).
- Use a mutex for multi-field invariants or any aggregate state where several fields must be updated atomically together.

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

OmScript lambdas are anonymous functions with a lightweight `|params| body` or `(params) => body` syntax, designed primarily for use with the higher-order array built-ins (`array_map`, `array_filter`, `array_reduce`, `array_any`, `array_every`, `array_count`, `array_find_index`, `array_min_by`, `array_max_by`).

**Implementation model — important:** Lambdas are *not* runtime closures. The parser desugars every lambda into a top-level named function (`__lambda_N`) and replaces the lambda expression with an identifier referring to that function. The higher-order built-ins receive that function reference at code-gen time. The consequences are:

- Lambdas **cannot capture** variables from the enclosing scope. Reference any non-parameter identifier and you reference a top-level / `global` symbol of the same name, not a local.
- A lambda's runtime *value* is a function pointer. You can store it in a `fn(T)->R` typed variable and call it directly.
- All lambdas with the same body produce distinct generated functions; there is no deduplication.

### 22.1 Syntax

Two lambda forms are supported. Both accept either an **expression body** (implicit return) or a **block body** (explicit `return`):

```ebnf
pipe_lambda       ::= '|' [params] '|' ( expression | block )
arrow_lambda      ::= param_list '=>' ( expression | block )
params            ::= param { ',' param }
param             ::= identifier [ ':' type ]
param_list        ::= identifier
                    | '(' [ params ] ')'
block             ::= '{' { statement } '}'
```

**Expression-body (single expression, implicit `return`):**
- The expression after `|` (pipe form) or `=>` (arrow form) is implicitly returned.
- No `return` keyword is written.

**Block-body (multi-statement, explicit `return`):**
- When `{` immediately follows the closing `|` (pipe form) or `=>` (arrow form), the parser reads a full block statement.
- The block may contain variable declarations, conditionals, loops, and any other statements.
- Results must be returned explicitly with `return`.
- An implicit `return` is NOT added — a block lambda that falls off the end returns `0`.

- Parameters with no type annotation default to `i64`.
- Annotate explicitly when the element type is anything else: `|x:float| x * 2.0`, `|s:string| str_len(s)`.
- Empty parameter list is written as `||` (pipe form) or `()` (arrow form): `var make_zero: fn()->int = || 0;`.

**Examples — expression body:**

```omscript
|x| x * 2                    // i64 → i64
|x:float| x * 2.0            // float → float
|x, y| x + y                 // (i64, i64) → i64
|acc, x| acc + x             // for array_reduce
|s:string| str_len(s)        // string → i64
|| 42                        // () → i64

x => x * 2                   // single-param shorthand
(x: int) => x * 2
(x: int, y: int) => x + y
() => 42
```

**Examples — block body:**

```omscript
// Pipe lambda with block body
var doubled = array_map(arr, |x: int| {
    var y = x * 2;
    return y;
});

// Arrow lambda with block body
var transformed: int[] = array_map(arr, (x: int) => {
    if x < 0 {
        return -x;
    }
    return x * 2;
});

// Stored in a funcptr variable
var adder: fn(int)->int = (x: int) => {
    var bump = 100;
    return x + bump;
};
println(adder(7));   // 107
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
3. **Write a named helper function** and reference it by name:
   ```omscript
   fn double(x) { return x * 2; }
   var doubled = array_map(arr, double);    // ✅ pass the identifier
   ```

---

### 22.3 First-class function values

Lambdas evaluate to a `fn(T)->R` function reference that can be stored in a typed variable and invoked directly:

```omscript
fn square(n: int) -> int { return n * n; }

var f: fn(int)->int = |x: int| x * x;    // store lambda
println(f(5));                             // 25 ✅

var g: fn(int)->int = square;             // store named function
println(g(5));                             // 25 ✅
```

---

### 22.4 Higher-order built-in interaction

The following built-ins accept a lambda or a named function reference. See §11.7 for full signatures.

| Built-in | Lambda shape |
| --- | --- |
| `array_map(arr, fn)` | `|x| → T` |
| `array_filter(arr, fn)` | `|x| → bool` |
| `array_reduce(arr, fn, init)` | `|acc, x| → acc` |
| `array_any(arr, fn)` | `|x| → bool` |
| `array_every(arr, fn)` | `|x| → bool` |
| `array_count(arr, fn)` | `|x| → bool` |
| `array_find(arr, value)` | plain value (not a function) — returns index of first equal element, or `-1` |
| `array_find_index(arr, fn)` | `|x| → bool`, returns index of first matching element, or `-1` |
| `array_zip_with(a, b, fn)` | `|x, y| → value`, returns element-wise map over `min(len(a), len(b))` |
| `array_min_by(arr, fn)` | `|x| → key`, returns element with minimum key |
| `array_max_by(arr, fn)` | `|x| → key`, returns element with maximum key |

---

### 22.5 Pipe-forward and spread interaction

Lambdas stored in `fn(T)->R` variables can be used as pipe targets:

```omscript
var double: fn(int)->int = x => x * 2;
var y = 5 |> double;                           // ✅ 10
```

The spread operator `...arr` expands an array as positional arguments to a function call, independently of any lambdas:

```omscript
var a = [1, 2, 3];
var s = sum(...a);   // sum(1, 2, 3)
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

**`import std;` — optional namespace import:**

Standard library functions are **always accessible by their bare names** without any import. `import std;` is provided as a convention for files that want to signal explicit stdlib use, but it is a no-op for the standard library.

```omscript
// import std;  ← optional, not required

fn main() {
    println("hello"); // works without import std;
    var x = abs(-5);  // works without import std;
    return 0;
}
```

Alternatively, fully qualify every call with `std::` (always works, no import required):

```omscript
// No import std; needed when using std:: prefix
fn main() {
    std::println("hello");   // always works
    var x = std::abs(-5);    // always works
    return 0;
}
```

Both forms are equivalent in semantics; the choice is stylistic.

---

### 23.8 User-defined namespaces

OmScript supports user-defined namespace blocks that group functions, structs, and enums under a named scope.

**Syntax:**
```omscript
namespace Math {
    fn add(a, b) { return a + b; }
    fn mul(a, b) { return a * b; }

    struct Vec2 { x, y }
}
```

Declarations inside a `namespace` block are registered with their fully qualified names (`Math::add`, `Math::Vec2`) and must be called that way unless the namespace is imported.

**Qualified access (no import required):**
```omscript
var r = Math::add(3, 4);           // qualified function call
var v = Math::Vec2 { x: 1, y: 2 }; // qualified struct literal
```

**Bare access after `import NSName;`:**
```omscript
import Math;      // import namespace — enables bare access

var r = add(3, 4);           // equivalent to Math::add(3, 4)
var v = Vec2 { x: 1, y: 2 }; // equivalent to Math::Vec2 { x: 1, y: 2 }
```

**Allowed declarations inside a namespace block:**
- `fn` — function definitions
- `struct` — struct type definitions
- `enum` — enum definitions

Namespaces cannot be nested. Each namespace block contributes to a flat per-name registry; multiple `namespace Math { ... }` blocks in the same file are additive.

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
| --- | --- | --- |
| `help`               | `-h`, `--help`              | Display usage information                               |
| `version`            | `-v`, `--version`           | Print compiler version                                  |
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
| --- | --- | --- | --- |
| `-o <path>`       | —     | `./a.out`   | Output file path                                     |
| `--emit-obj`      | —     | `false`     | Emit object file (`.o`) only, skip linking           |
| `--dry-run`       | —     | `false`     | Validate and codegen, but don't write files          |
| `-V`, `--verbose` | `-V`  | `false`     | Print LLVM IR, pass timings, diagnostic details      |
| `-q`, `--quiet`   | `-q`  | `false`     | Suppress non-error output                            |
| `--time`          | —     | `false`     | Show compilation phase timing breakdown              |
| `--dump-ast`      | —     | `false`     | Dump parsed AST to stdout before codegen             |

#### Optimization

| Flag                   | Short | Default         | Description                                             |
| --- | --- | --- | --- |
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
| `-fsdr`                | —     | `true` (O2+)    | Speculative Devectorization & Revectorization (SDR)     |
| `-fipof`               | —     | `true` (O2+)    | Implicit Phase Ordering Fixer (IPOF)                    |

#### Target

| Flag              | Short | Default    | Description                                          |
| --- | --- | --- | --- |
| `-march=<cpu>`    | —     | `native`   | Target CPU architecture (e.g., `x86-64-v3`, `znver3`) |
| `-mtune=<cpu>`    | —     | (same as `-march`) | CPU model for scheduling tuning            |
| `-fpic`           | —     | `true`     | Generate position-independent code                   |

#### Features

| Flag              | Short | Default    | Description                                          |
| --- | --- | --- | --- |
| `-flto`           | —     | `false`    | Link-time optimization (whole-program analysis)      |
| `-ffast-math`     | —     | `false`    | Unsafe floating-point optimizations (reassociation, reciprocals, ignore NaN/Inf) |
| `-fstack-protector` | —   | `false`    | Stack canary protection against buffer overflows     |
| `-static`         | —     | `false`    | Static linking (embed runtime, no shared libs)       |

#### Debug

| Flag              | Short | Default    | Description                                          |
| --- | --- | --- | --- |
| `-g`, `--debug`   | `-g`  | `false`    | Emit DWARF debug info for GDB/LLDB                   |
| `-s`, `--strip`   | `-s`  | `false`    | Strip symbols from output binary                     |
| `-k`, `--keep-temps` | `-k` | `false` | Keep temporary files (for `run` command only)        |

#### Diagnostics

The compiler emits structured diagnostics to stderr. Errors use exit code 1; warnings do not halt compilation.

| Flag                       | Short | Default    | Description                                                        |
| --- | --- | --- | --- |
| `--color`                  | —     | auto-detect | Force ANSI colors in diagnostics (`--color=always`)               |
| `--no-color`               | —     | —          | Disable ANSI colors in diagnostics (`--color=never`)               |
| `--color=auto`             | —     | —          | Auto-detect TTY and enable colors if running in a terminal         |
| `--error-format=human`     | —     | (default)  | Rich diagnostic output with source snippet and caret (`^`)         |
| `--error-format=plain`     | —     | —          | Plain single-line diagnostics without ANSI or source context        |
| `--error-format=json`      | —     | —          | One JSON object per diagnostic for tooling integration              |
| `-Werror` / `--Werror`     | —     | `false`    | Promote all warnings to errors (non-zero exit if any warning emitted) |
| `--max-errors=N`           | —     | `0`        | Stop after `N` errors (`0` = unlimited)                            |

#### Memory Safety and Ownership

| Flag                      | Short | Default | Description                                                  |
| --- | --- | --- | --- |
| `--no-ownership-checks`   | —     | `false` | Disable borrow/ownership checks entirely (unsafe mode, Ω spec §6.2) |
| `--mem-sanitize`          | —     | `false` | Compile-time path-sensitive memory-safety diagnostics (Ω spec §7) |

**`--no-ownership-checks`**: Disables the borrow checker. The compiler treats memory as a raw C-like model — no E015–E020 errors emitted. Use for interop with C-style pointer code.

**`--mem-sanitize`**: Enables compile-time CFG-based analysis that detects and reports:
- Use-after-invalidate paths
- Potential null dereferences
- Double-free risks
- Out-of-bounds pointer arithmetic possibilities

Output uses the format:
```
MEM-SANITIZER ERROR
file.om:42

use-after-invalidate detected
variable: p
control path:
  line 38 -> invalidate p
  line 41 -> *p (invalid use)
```

#### PGO (Profile-Guided Optimization)

| Flag              | Short | Default    | Description                                          |
| --- | --- | --- | --- |
| `-fpgo-gen=<path>` | —    | —          | Instrument binary to write profile to `<path>` on exit |
| `-fpgo-use=<path>` | —    | —          | Use profile from `<path>` for guided optimization    |

Profile format: LLVM raw profile (`.profraw`), converted via `llvm-profdata merge`.

#### Miscellaneous

| Flag                | Short | Default | Description                                          |
| --- | --- | --- | --- |
| `--release`         | —     | —       | Shortcut for `--profile release`                     |
| `--profile <name>`  | —     | `debug` | Use named profile from `oms.toml`                    |
| `--profile=<name>`  | —     | —       | Alternate form of `--profile`                        |

### 24.4 Output formats and file extensions

| Mode           | Extension(s)         | Description                                             |
| --- | --- | --- |
| Executable     | (none), `.exe`       | Native binary (default on Unix/Windows)                 |
| Object file    | `.o`, `.obj`         | Produced with `--emit-obj`                              |
| LLVM IR        | `.ll`                | Human-readable text (via `emit-ir`)                     |
| LLVM Bitcode   | `.bc`                | Binary IR (when LTO enabled)                            |
| Assembly       | `.s`                 | Target assembly (not directly exposed; use `-S` via `clang`) |

### 24.5 Optimization levels

| Level | Passes Enabled                                                                 |
| --- | --- |
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

| Action                           | Aliases               | Description                                                        |
| --- | --- | --- |
| `install <name>`                 | `add <name>`          | Install a package from the registry into `om_packages/`            |
| `remove <name>`                  | `uninstall`, `rm`     | Remove an installed package from `om_packages/` and `oms.toml`     |
| `list`                           | `ls`                  | List all currently installed packages                              |
| `search [query]`                 | `find [query]`        | Search the registry for packages matching the query                |
| `info <name>`                    | `show <name>`         | Show metadata and description for a package                        |

**Dependency resolution**:
Dependencies are fetched from the default registry URL (GitHub `user-packages/` directory) or a custom registry set via `OMSC_REGISTRY_URL` environment variable.

**Example workflow**:
```bash
omsc pkg install http           # downloads http library to om_packages/http/
omsc pkg list                   # shows installed packages
omsc pkg info http              # shows metadata for http package
omsc pkg remove http            # uninstalls and removes from oms.toml
omsc pkg search json            # search registry for JSON-related packages
```

### 24.8 Project file format (oms.toml)

`oms.toml` is a minimal-TOML manifest supporting:

#### `[project]` section

| Key            | Type     | Description                                    |
| --- | --- | --- |
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
| --- | --- | --- | --- | --- |
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
| --- | --- |
| `OMSC_BINARY_PATH`   | Override compiler installation path (for self-update)              |
| `OMSC_REGISTRY_URL`  | Custom package registry URL (default: GitHub user-packages)        |
| `OMSC_DUMP_SCHEDULE` | When set, dump HGOE scheduling decisions to stderr                 |
| `HOME` / `USERPROFILE` | User home directory (for config and cache)                       |

### 24.10 Exit codes

| Code | Meaning                                                              |
| --- | --- |
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
   Mark functions as `@semantics(pure)` when they have no side effects and deterministic output. Propagates across call graph.

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
| --- | --- |
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
| --- | --- |
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
| --- | --- |
| `LoopSimplify` | Loops have dedicated preheaders and a single backedge |
| `LCSSA` | Loop-Closed SSA form — uses of loop-defined values exit through PHIs |
| `CanonicalIV` | Induction variables are in canonical form (IndVarSimplify completed) |
| `SimplifiedCFG` | Control-flow graph has been simplified (SimplifyCFGPass completed) |

`PassContract` is currently an adjunct to `PassMetadata`; the source comment at `opt_pass.h:223-225` notes that future work will migrate to `PassContract` as the sole pass descriptor.

### 25.2.3 AnalysisDependencyGraph (cascading invalidation)

`AnalysisDependencyGraph` (`opt_pass.h:283-343`) records "fact A depends on fact B" edges. When a transform invalidates fact B, every fact that transitively depends on B is also invalidated. Callers therefore only need to invalidate the *directly* affected fact; the cascade is computed automatically.

The standard OmScript dependency graph is built by `AnalysisDependencyGraph::createDefault()` (`src/optimization_manager.cpp`):

```
constant_returns    → (no dependencies)
purity              → constant_returns
effects             → purity
ersl                → effects
synthesis           → purity, effects
cfctre              → purity, effects, synthesis
egraph              → cfctre
range_analysis      → purity, effects, cfctre
rlc                 → effects
dce                 → cfctre
cse                 → dce
alg_simp            → cfctre, dce
copy_prop           → cfctre, dce, alg_simp
width_legalization  → range_analysis, copy_prop, alg_simp
width_opt           → width_legalization
hgoe_egraph         → egraph, cfctre
```

Read this as "the named fact depends on the listed facts": invalidating `purity` therefore cascades to `effects`, `synthesis`, `cfctre`, `egraph`, and `range_analysis`. Lookup is via `getAllDependents(key)`, which performs a BFS over the dependency edges and returns the fact itself plus every transitive dependent.

**Thread safety** (`opt_pass.h:279-282`): construction (`addDependency`) is **not** thread-safe and must happen during single-threaded static initialisation; subsequent reads from multiple compilation threads are safe.

### 25.2.4 PipelineStage (six-stage compilation pipeline)

`PipelineStage` (`include/optimization_manager.h:83-109`) provides the stable vocabulary the `OptimizationManager` uses to label passes, schedule requests, and emit progress diagnostics. The six stages and their fixed ordering are:

| Stage | Mandate |
| --- | --- |
| `AST_ANALYSIS` (0) | Read-only AST analyses: string/array type pre-analysis, constant-return detection, purity inference, effect inference |
| `AST_TRANSFORM` (1) | Semantics-preserving AST transforms: synthesis expansion, CF-CTRE, e-graph saturation, range analysis |
| `IR_CANONICALIZE` (2) | LLVM IR normalization that establishes invariants for later loop transforms: LoopSimplify, LCSSA, IndVarSimplify, SimplifyCFG |
| `LOOP_TRANSFORM` (3) | Polyhedral and structural loop transforms (interchange, tiling, skewing, reversal, fusion, fission) — all routed through `UnifiedLoopTransformer` (see §26.14) |
| `IR_MIDEND` (4) | LLVM midend scalar + vectorization: inlining, IPSCCP, GVN, DSE, loop vectorizer, SLP, post-vec cleanup |
| `LATE_SUPEROPT_HGOE` (5) | Late peephole + synthesis: superoptimizer (§26.2), HGOE hardware-guided emission (§26.3), post-pipeline simplification |

Stages run in numerical order and `PassMetadata::phase` (a `PassPhase` value) maps to the corresponding `PipelineStage`.

### 25.3 Per-O-level pass list

`PassKind` controls which passes run at each level:
- **`Analysis`** and **`SemanticTransform`** passes run at every level that needs the fact.
- **`CostTransform`** passes (DCE, CSE, AlgSimp, WidthOpt, etc.) are skipped at O0. At O0 the user expects minimal compile time and maximum AST fidelity for debugger step-accuracy; running rewrites would unpredictably modify the code structure.

#### O0 (debug)
1. Lexer
2. Parser
3. Type pre-analysis
4. Codegen (no optimization; `CostTransform` passes skipped)

**IR quality at O0**: All functions still receive `nounwind`, `mustprogress`, `nosync`, `nofree`, `willreturn`, `noundef` (on params/return), `nonnull` (on pointer return), `ZExt`/`SExt` signedness on integer params — these correctness-enabling attributes are unconditional and do not depend on O-level.

#### O1 (basic)
1. Lexer
2. Parser
3. Type pre-analysis
4. Purity inference (lightweight, no cross-function analysis)
5. CF-CTRE (same fuel/depth limits as O2 — see below)
6. DCE (`CostTransform`), CSE, AlgSimp, CopyProp
7. Codegen
8. LLVM: SimplifyCFG, Mem2Reg, SROA, EarlyCSE

#### O2 (standard)
1–6. All AST phases (Analysis + SemanticTransform passes)
7. **Synthesis expansion** (if `std::synthesize` present)
8. **CF-CTRE** (fuel limit `kMaxInstructions = 10,000,000`, depth limit `kMaxDepth = 128` — see `include/cfctre.h:483-484`)
9. **Abstract interpretation / range analysis**
10. **DCE, CSE, AlgSimp, CopyProp, WidthLegalization, WidthOpt** (`CostTransform` passes)
11. **E-graph optimization** (`SaturationConfig`: `maxNodes = 50,000`, `maxIterations = 30` — see `include/egraph.h:331-332`)
12. Codegen
13. LLVM canonicalization: LoopSimplify, LCSSA, IndVarSimplify
14. **Polyhedral optimizer** (tiling, interchange, skewing — see §26.13)
15. LLVM midend: Inlining, IPSCCP, GVN, LICM, DSE, Loop Vectorizer, SLP Vectorizer
16. **Superoptimizer** (idiom recognition + algebraic + branch→select + synthesis, level 2 default — see §26.2)
17. **HGOE** (only when a hardware profile is available — i.e. `-march=` or `-mtune=` resolves to a known microarch — see §26.3)
18. Post-pipeline cleanup: AggressiveDCE, GlobalDCE

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

### 25.6 LLVM IR quality guarantees

The code generator (`src/codegen.cpp`) applies a layered set of LLVM attributes and metadata to produce IR that LLVM's midend can optimize without guesswork.

#### Unconditional per-function attributes (all O-levels)

Every user-defined function receives the following attributes regardless of optimization level:

| Attribute | Why |
| --- | --- |
| `nounwind` | OmScript uses a flag-based error model, never C++ exceptions |
| `mustprogress` | Every loop in OmScript is finite (no `while(true)` without `break` or `return`); enables LICM and loop transforms |
| `prefer-vector-width=N` | Set to the target's preferred SIMD width for autovectorization hints |
| `nosync` | Applied to functions that do not use concurrency primitives; suppressed for functions that use thread_create, mutexes, or atomics |
| `nofree` | User functions never call `free()` directly |
| `willreturn` | Asserts finite termination; enables DSE and load-forwarding across the call |
| `noundef` (params + return) | OmScript always initializes variables before use |
| `ZExt`/`SExt` (integer params + return) | Signals correct signedness to calling-convention optimization |
| `nonnull` + `dereferenceable(8)` (pointer return, O1+) | OmScript functions that return arrays/strings always return non-null |

Exception: functions that contain concurrency primitives (e.g. explicit atomic operations) have `nosync`, `nofree`, and `willreturn` suppressed.

#### O2+ additions

| Attribute | When applied |
| --- | --- |
| `noalias` + `nonnull` + `dereferenceable(8)` + `align(16)` + `nocapture` (pointer params) | All pointer parameters, because OmScript's ownership model prevents aliasing across function boundaries |
| `nosync` (function-level reinforcement) | Explicit even for functions without concurrency primitives |
| `nonnull` (pointer params) | Ownership model guarantees non-null pointer arguments |
| `!range [lo, hi+1)` on CallInst | When the AST pre-pass proves a narrowed `ValueRange` for the function's return value |
| Function entry alignment (16 B default; 32 B for `@hot`; 64 B for `@align`) | I-cache alignment |

#### Call-site attribute propagation

At every user function call site, `generateCall` propagates the callee's
function-level attributes to the `CallInst`:

| Call-site attribute | Source |
| --- | --- |
| `speculatable` | Callee has `@semantics(speculatable)` |
| `willreturn` | Callee has `WillReturn` attribute |
| `nosync` | Callee has `NoSync` attribute |
| `nofree` | Callee has `NoFree` attribute |
| memory effects (`memory(none)`, `memory(read)`, …) | Copied from callee's `MemoryEffects`; enables LICM to hoist calls out of loops |
| `!range [lo, hi+1)` metadata | When the pre-pass `returnRange` fact provides a narrowed `ValueRange` |

These are redundant with the function definition in theory, but LLVM's LICM, DSE, and call-site devirtualization passes scan `CallInst` attributes directly; without them on the call instruction, passes that run before inlining cannot see them.

#### Load/store metadata

| Metadata | Location |
| --- | --- |
| `!noundef` | All local variable loads and array element loads |
| `!range` | Array-length loads and array-element loads of integer elements (range `[0, INT64_MAX)` for lengths) |
| `!nonnull` | `stdout` global pointer load; `malloc`/`calloc` return values |
| `!invariant.load` | Read-only globals and string literals |

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
var x: int = (a + 0) * 1;
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
| --- | --- |
| `name` | Human-readable rule name (used in diagnostics) |
| `lhs` | Left-hand side `Pattern` to match |
| `rhs` | `RhsBuilder` callback `ClassId(EGraph&, const Subst&)` that constructs the replacement |
| `guard` | Optional `RuleGuard` predicate `bool(const EGraph&, const Subst&)` |

The optional **guard** turns the engine from a purely syntactic rewriter into a *relational* one: a guard can inspect bound `EClass` analysis flags (e.g. `isPowerOfTwo`, `isNonNeg`) before allowing the RHS to be built. This is how rules like "rewrite `x * c` to `x << log2(c)` only when `c` is a power of two" stay sound.

#### Cost Model and Extraction

Once saturation completes, the engine **extracts** a single representative e-node per class to produce the final program. The cost is computed by `CostModel` (`egraph.h:306-323`) with two notable parameters beyond per-node latency:

| Field | Default | Meaning |
| --- | --- | --- |
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
var x: int = (3 + 5) * 2;
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
var y: int = x * 1 + 0 - 0;
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
| --- | --- | --- | --- | --- | --- |
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
| --- | --- | --- | --- | --- | --- | --- |
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
| --- | --- |
| `Strict` | Full IEEE-754 compliance. No reassociation, no NaN/Inf assumptions. |
| `Medium` | Allow limited reassociation and some vectorization; preserve NaN/Inf. |
| `Fast` | Equivalent to `-ffast-math`: reassociation, reciprocal transforms, ignoring NaN/Inf, fused operations all permitted. |

`FPPrecision` is an alternative to the global `-ffast-math` flag for cases where only some computations are safe to relax. When two operands carry different precisions they are merged via a *conservative meet* (`hardware_graph.h:68-71`): the **stricter** level wins, so combining a `Strict` operand with a `Fast` operand yields `Strict` semantics. The string names `"strict" / "medium" / "fast"` are exposed by `fpPrecisionName()` (`hardware_graph.h:56-63`).

#### Cache-Aware Optimization

HGOE includes a cache model used to choose tile sizes and software-prefetch distances. The `CacheModel` struct (`hardware_graph.h:80-90`, defaults shown) is built from the resolved `MicroarchProfile` via `buildCacheModel()`:

| Field | Default | Meaning |
| --- | --- | --- |
| `l1Size` / `l1Latency` / `l1LineSize` | 32 KB / 4 cyc / 64 B | L1D parameters |
| `l2Size` / `l2Latency` | 256 KB / 12 cyc | L2 parameters |
| `l3Size` / `l3Latency` | 8192 KB / 40 cyc | L3 parameters |
| `memLatency` / `memBandwidth` | 200 cyc / 40.0 GB/s | Main memory parameters |

Memory accesses inside loops are classified by an `AccessPattern` (`hardware_graph.h:98-104`): `Unknown`, `Sequential` (best locality), `Strided` (predictable, may miss cache), `Random` (poor locality), `Streaming` (write-once / read-once, bypass-friendly). The classification feeds prefetch-insertion and AoS→SoA layout decisions.

The cache-aware pass reports its work through `CacheOptStats` (`hardware_graph.h:107-112`):

| Counter | Meaning |
| --- | --- |
| `loopsTiled` | Loops with tiling metadata added |
| `loopsInterchanged` | Loops reordered for locality |
| `prefetchesInserted` | Software prefetch hints added |
| `layoutHints` | AoS→SoA suggestions emitted |

#### Operation Classes

| Class           | Example LLVM Ops                     | Typical Latency (cycles) |
| --- | --- | --- |
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
var x: int = 3;
var y: int = x * 10 + x * 5;
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

**Implemented** via the three-tier allocation strategy in `alloc<T>` and `new T(n)` (see §17.9.2):

- **T1 (Stack)**: when the allocation count is a compile-time constant and `count × sizeof(T) ≤ 8 KiB`, the compiler unconditionally emits a stack `alloca` in the function entry block — no escape analysis required because the decision is made entirely at the allocation site.
- **T2 (Arena)**: compile-time constant allocations that fit the 64 KiB per-function slab are GEP'd into a shared entry-block alloca. Zero heap involvement; the slab is lifetime-scoped.
- **T3 (Heap)**: dynamic counts or oversized allocations fall through to `malloc` / `calloc`.

Additionally, the LLVM mid-end's own escape analysis (`-O1`+) can promote surviving T3 heap allocations to stack once it proves the pointer does not escape the function (i.e. is not returned, stored into a global, or passed to an escaping callee). LLVM's analysis has no hard byte threshold.

**Compile-time array escape heuristic** (§11.5): arrays declared with literal-sized `array_fill` / `[...]` and proven not to escape are stack-promoted by the AST-level analysis in `codegen.cpp` (`optStats_.escapeStackAllocs` counter). The 4,096-byte threshold in §11.5 is specific to that array heuristic and is separate from `alloc<T>` / `new T`.

**`kStackAllocThreshold` = 8,192 bytes** (`include/codegen.h`). Allocations larger than this threshold remain on the heap even when the count is constant, to prevent stack overflow.

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
var p: Point = Point { x: 3, y: 4 };
var z: int = p.x + p.y;
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
var sum: int = 0;
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
| --- | --- |
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
const arr: int[] = comptime {
    var a: int[] = [1, 2, 3];
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
    var r1: int = newRegion();
    var data1: ptr = alloc(r1, 1024);
    // ... use data1 ...
    invalidate(r1);

    var r2: int = newRegion();          // RLC merges r2 into r1
    var data2: ptr = alloc(r2, 512);    // reuses r1's memory
    // ... use data2 ...
    invalidate(r2);
}
```

After RLC:
```omscript
fn process() {
    var r1: int = newRegion();
    var data1: ptr = alloc(r1, 1024);
    // ... use data1 ...
    // invalidate(r1) removed

    // r2 eliminated
    var data2: ptr = alloc(r1, 512);    // reuses r1
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
| --- | --- | --- |
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

OmScript provides explicit integer type casts as built-in functions. These are codegen-time operations that emit truncation or sign-extension instructions; the result is always a 64-bit integer at the LLVM IR level.

### 27.1 Overview Table

| Cast     | Signature        | Domain          | Result Type (LLVM) | Operation                          | Overflow Behavior |
| --- | --- | --- | --- | --- | --- |
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
var x: int = 42;
var y: u64 = u64(x);   // y == 42, no IR generated
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
var a: u32 = u32(-1);             // a = 4294967295 (0xFFFFFFFF)
var b: u32 = u32(0x1_0000_0000);  // b = 0 (wraps)
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
var c: i32 = i32(0x8000_0000);   // c = -2147483648 (sign bit set)
var d: i32 = i32(0x7FFF_FFFF);   // d = 2147483647
var e: i32 = i32(0x1_8000_0000); // e = -2147483648 (wraps, bit 31 set)
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
var f: u16 = u16(0x1_FFFF);   // f = 0xFFFF = 65535
var g: u16 = u16(-1);         // g = 65535 (0xFFFF)
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
var h: i16 = i16(0x8000);     // h = -32768 (0x8000 sign-extends to 0xFFFF_FFFF_FFFF_8000)
var i2: i16 = i16(0x7FFF);    // i2 = 32767
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
var j: u8 = u8(255);    // j = 255
var k: u8 = u8(256);    // k = 0 (wraps)
var l: u8 = u8(-1);     // l = 255 (0xFF)
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
var m: i8 = i8(127);    // m = 127
var n: i8 = i8(128);    // n = -128 (0x80 sign-extends to 0xFFFF_FFFF_FFFF_FF80)
var o: i8 = i8(-1);     // o = -1 (0xFF sign-extends to 0xFFFF_FFFF_FFFF_FFFF)
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
var p: bool = bool(42);   // p = 1
var q: bool = bool(0);    // q = 0
var r: bool = bool(-5);   // r = 1
```

### 27.7 Compile-time folding rules table

When the argument to a cast is a **compile-time constant**, CF-CTRE folds the cast:

| Cast     | Constant Folding Rule (C++ equivalent)                          |
| --- | --- |
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

Casts are **codegen-time operations**, not type annotations. OmScript has a single `int` type at the AST level for integer values. The cast functions are built-in functions that emit IR at codegen time.

**Contrast with C**:
- In C: `uint32_t x = (uint32_t)y;` — type-level constraint enforced at compile time.
- In OmScript: `var x: int = u32(y);` — codegen-time truncation + zero-extension; result is stored as `i64`.

### 27.9 Worked examples

#### Example 1: Bit manipulation with `u8`

```omscript
fn pack_rgb(r: int, g: int, b: int) -> int {
    var r8: int = u8(r);
    var g8: int = u8(g);
    var b8: int = u8(b);
    return (r8 << 16) | (g8 << 8) | b8;
}

var color: int = pack_rgb(255, 128, 64);   // color = 0xFF8040
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
var val: int = 0xFFFF_FFFF;

var unsigned: int = u32(val);   // unsigned = 4294967295 (0xFFFFFFFF, zero-extended)
var signed: int   = i32(val);   // signed   = -1 (0xFFFFFFFF, sign-extended to 0xFFFF_FFFF_FFFF_FFFF)

print(unsigned);  // 4294967295
print(signed);    // -1
```

#### Example 3: Overflow wrapping

```omscript
var big: int = 0x1_0000_0000;    // 2^32
var wrapped: int = u32(big);     // wrapped = 0 (wraps modulo 2^32)

var neg: int = -1;
var byte: int = u8(neg);         // byte = 255 (wraps: -1 mod 256 = 255)
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
| --- | --- |
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
- Mark function `@semantics(pure)` if:
  - Body contains no `print`, `input`, `file_read`, `file_write`, `sleep`, `random`, `time`.
  - All called functions are also `@semantics(pure)`.
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
| --- | --- | --- |
| `ADD`           | `lhs, rhs`     | `lhs + rhs` with int64/float64 semantics       |
| `SUB`           | `lhs, rhs`     | `lhs - rhs`                                    |
| `MUL`           | `lhs, rhs`     | `lhs * rhs`                                    |
| `DIV`           | `lhs, rhs`     | `lhs / rhs` (returns `symbolic()` if `rhs == 0`) |
| `MOD`           | `lhs, rhs`     | `lhs % rhs` (returns `symbolic()` if `rhs == 0`) |
| `NEG`           | `val`          | `-val`                                         |

**Overflow behavior**: Wraps modulo 2⁶⁴ (same as runtime).

#### Comparison Operations

| AST Node        | Operands       | Result                                         |
| --- | --- | --- |
| `LT`            | `lhs, rhs`     | `CTValue::fromBool(lhs < rhs)`                 |
| `LE`            | `lhs, rhs`     | `CTValue::fromBool(lhs <= rhs)`                |
| `GT`            | `lhs, rhs`     | `CTValue::fromBool(lhs > rhs)`                 |
| `GE`            | `lhs, rhs`     | `CTValue::fromBool(lhs >= rhs)`                |
| `EQ`            | `lhs, rhs`     | `CTValue::fromBool(lhs == rhs)`                |
| `NE`            | `lhs, rhs`     | `CTValue::fromBool(lhs != rhs)`                |

**String comparison**: Lexicographic order for `<`, `<=`, etc.

#### Logical Operations

| AST Node        | Operands       | Semantics                                      |
| --- | --- | --- |
| `LOG_AND`       | `lhs, rhs`     | Short-circuit: `lhs.isTruthy() && rhs.isTruthy()` |
| `LOG_OR`        | `lhs, rhs`     | Short-circuit: `lhs.isTruthy() || rhs.isTruthy()` |
| `LOG_NOT`       | `val`          | `!val.isTruthy()`                              |

**Truthy**: Non-zero int/float, non-empty string, valid array handle.

#### Bitwise Operations

| AST Node        | Operands       | Semantics                                      |
| --- | --- | --- |
| `BIT_AND`       | `lhs, rhs`     | `lhs & rhs` (64-bit)                           |
| `BIT_OR`        | `lhs, rhs`     | `lhs | rhs`                                    |
| `BIT_XOR`       | `lhs, rhs`     | `lhs ^ rhs`                                    |
| `BIT_NOT`       | `val`          | `~val`                                         |
| `SHL`           | `lhs, rhs`     | `lhs << rhs` (rhs clamped to [0, 63])          |
| `SHR`           | `lhs, rhs`     | Arithmetic right shift `lhs >> rhs`            |

#### Array Operations

| AST Node           | Operation                                      |
| --- | --- |
| `ARRAY_LITERAL`    | Allocate `CTArray` on `CTHeap`, return handle  |
| `INDEX_EXPR`       | `heap_.load(arr, idx)` — bounds-checked load   |
| `INDEX_ASSIGN`     | `heap_.store(arr, idx, val)`                   |
| `ARRAY_LEN`        | `heap_.length(arr)`                            |

**Bounds checking**: Out-of-bounds access returns `CTValue::uninit()` (not an error).

#### String Operations

| AST Node           | Operation                                      |
| --- | --- |
| `STRING_LITERAL`   | `CTValue::fromString(str)`                     |
| `STR_CONCAT`       | `lhs.str + rhs.str`                            |
| `STR_LEN`          | `str.size()`                                   |
| `CHAR_AT`          | `str[idx]` (returns `uninit()` if out-of-bounds) |

#### Control-Flow Operations

| AST Node        | Semantics                                      |
| --- | --- |
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
| --- | --- | --- |
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
5. Marks the function `@semantics(pure, const_eval)`.

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
| --- | --- | --- |
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
var x: int = 42;          // mutable, explicit type annotation
const y: int = 100;       // immutable, explicit type annotation
var z = 0;                // mutable, type inferred from initializer (i64)
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
} elif x < 0 {
    print("negative");
} else {
    print("zero");
}

var result: string = x > 0 ? "pos" : "neg";   // ternary
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
var s: int = a + b;      // arithmetic
var prod: int = a * b;
var rem: int = a % b;
var pw: int = a ** b;    // exponentiation

var eq: bool = a == b;   // comparison
var ne: bool = a != b;

var a2: bool = a && b;   // logical
var or2: bool = a || b;

var band: int = a & b;   // bitwise
var bor: int = a | b;
var shl: int = a << 2;
```

### Arrays
```omscript
var arr: int[] = [1, 2, 3];
var first: int = arr[0];
arr[1] = 42;
var length: int = len(arr);
push(arr, 4);
var last: int = pop(arr);
```

### Strings
```omscript
var s: string = "hello";
var slen: int = str_len(s);
var upper: string = str_upper(s);
var concat: string = s + " world";
var sub: string = str_substr(s, 0, 3);   // "hel"
```

### Ownership
```omscript
struct Node { value: int; next: ptr<Node>; }

// alloc<T>(n) — raw uninitialised allocation (T1: stack, T2: arena, T3: heap)
var p: ptr<Node> = alloc<Node>(1);
construct p { value: 42, next: nullptr };

// new T(n) — zero-initialised allocation
var arr: ptr<int> = new int(8);   // 8 zero-initialised i64 slots

// invalidate — explicit free (compiler enforces ownership rules)
invalidate p;
invalidate arr;
```

### Pointers and Construction
```omscript
struct Point { x: int; y: int; }

// Raw allocation — uninitialised memory (fastest when you write every field)
var p: ptr<Point> = alloc<Point>(1);

// In-place field initialisation on already-allocated memory (zero-cost GEP+store)
construct p {
    x: 10,
    y: 20,
};

// Zero-initialised allocation — all bytes zeroed before use
var zp: ptr<Point> = new Point;       // calloc(1, sizeof(Point))
var zi: ptr<i64>   = new i64(8);      // calloc(8, sizeof(i64))

// Allocate + initialise specific fields in one expression (alloc + field stores)
var q: ptr<Point> = new Point { x: 3, y: 4 };
println(q->x);  // 3

invalidate p;
invalidate zp;
invalidate zi;
invalidate q;
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
var x: int = 3;
var y: int = x * 10 + x * 5;   // optimized to x * 15 by OPTMAX
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
| --- | --- |
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
| **Ownership state**   | Per-variable state tracked by the borrow checker. One of: `Owned`, `Borrowed`, `MutBorrowed`, `Shared`, `Frozen`, `Moved`, `Invalidated`. See §17.1. |
| **Owned**             | Initial ownership state: variable has unique, exclusive read-write access. |
| **Borrowed**          | Variable has ≥1 active immutable borrows (`borrow var r = x`). Source is readable but not writable or moveable until all borrows end. |
| **MutBorrowed**       | Variable has exactly one active mutable borrow (`borrow mut var r = x`). Source is completely locked (no reads or writes) until the borrow ends. |
| **Shared**            | Variable transitioned via `shared x;`. Multiple reads allowed; writes and mutable borrows are compile-time errors (E020). Reversible via `own x;`. |
| **Frozen**            | Variable permanently immutable via `freeze x;`. LLVM `!invariant` metadata emitted on all loads. Irreversible — `own` is a compile-time error (E021). |
| **Moved**             | Ownership transferred via `move var y = x`. Reading/writing `x` afterwards is a compile-time error (E015). |
| **Invalidated**       | Variable killed via `invalidate x;`. Memory scheduled for deferred `free()`. Any subsequent use is E015. Double-invalidate is E019. |
| **Freeze**            | `freeze x;` — mark variable permanently read-only. LLVM `!invariant.load` emitted. `own` on frozen variable → E021. |
| **Invalidate**        | `invalidate x;` — deferred deallocation. Logical death is immediate; physical `free()` is batched at function exit. Cannot invalidate while borrowed (E022). |
| **Reborrow**          | `reborrow var ref = src;` — create a new non-owning immutable alias from an existing borrow. Increments the source's `reborrows` count. |
| **No-alias**          | Guarantee that two pointers do not refer to overlapping memory. Emitted as LLVM `noalias` on `Owned` and `MutBorrowed` variables. Enables aggressive optimization. |
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

**OmScript Compiler Version**: `4.9.0`

Defined in `include/version.h`:
```cpp
#define OMSCRIPT_VERSION_MAJOR 4
#define OMSCRIPT_VERSION_MINOR 9
#define OMSCRIPT_VERSION_PATCH 0
#define OMSC_VERSION "4.9.0"
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
- This reference targets the `4.9.0` compiler snapshot described above.
- Source files written for older v4.x releases generally remain close to valid, but should be revalidated against current diagnostics and deprecation warnings on `4.9.0`.
- v5.0 will require explicit migration (automated tool planned).

**LLVM compatibility**:
- **Primary**: LLVM 18 — CI-tested on every push.
- **LLVM 17**: expected to work; not CI-tested.
- **LLVM 19**: supported — `llvm::Intrinsic::getOrInsertDeclaration` replaces `getDeclaration` (`#if LLVM_VERSION_MAJOR >= 19`).
- **LLVM 20–21**: supported — `VirtualFileSystem.h` include path changed in 22 (`#if LLVM_VERSION_MAJOR < 22`); `Attribute::NoCapture` replaced by `captures(none)` in 21 (`#if LLVM_VERSION_MAJOR >= 21`).
- **LLVM 22+**: `llvm/Passes/PassPlugin.h` moved to `llvm/Plugins/PassPlugin.h` — handled via `#if LLVM_VERSION_MAJOR >= 22`; `VirtualFileSystem.h` removed; Polly plugin path updated. All known breaking API changes are guarded.
- **macOS ARM64 (Apple Silicon)**: fully supported. The data layout and target triple are initialised at the start of `generate()` (via `InitializeNativeTarget` + `createTargetMachine` + `setDataLayout`) before any IR is emitted, ensuring correct ABI alignment for all types including `i64` (align 8, not 4).

**End of Part 3**
