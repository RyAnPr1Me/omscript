# Changelog

All notable changes to the OmScript compiler will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
