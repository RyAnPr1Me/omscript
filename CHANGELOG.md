# Changelog

All notable changes to the OmScript compiler will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
