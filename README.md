# OmScript

A low-level, C-like programming language with dynamic typing and **automatic reference counting memory management**. Features a **heavily optimized AOT compiler** using LLVM, a **lightweight adaptive JIT runtime** that recompiles hot functions with aggressive optimizations, and a **three-layer optimization engine** (equality-saturation E-graph, superoptimizer, and hardware-graph-driven instruction scheduler) that produces near-optimal native machine code for each target CPU.

## Key Features

- **C-like Syntax**: Familiar syntax for C/JavaScript programmers
- **Dynamic Typing**: Variables are dynamically typed; optional type annotations for documentation and OPTMAX hot paths
- **Structs**: Lightweight named record types with field access and mutation
- **Modules / Import**: Split programs across files with `import "file.om";` — duplicate/circular imports are silently deduplicated
- **Aggressive AOT Compilation**: Multi-level LLVM optimization (O0–O3) for maximum performance
- **Reference Counted Memory**: Automatic memory management using malloc/free with deterministic deallocation; no GC pauses
- **Lambda Expressions**: Anonymous functions with `|x| x * 2` syntax for use with higher-order builtins
- **Pipe Operator**: Left-to-right function chaining with `expr |> fn`
- **Spread Operator**: Array unpacking in literals with `[1, ...arr, 2]`
- **For Loops with Ranges**: Modern range-based iteration with `for (i in start...end)` and `for (i in start...end...step)`
- **For-Each Loops**: Iterate over arrays with `for (x in array)`
- **Switch/Case**: Multi-way branching with `switch`/`case`/`default`
- **Do-While Loops**: Execute body at least once with `do { ... } while (cond);`
- **Error Handling**: `try`/`catch`/`throw` for structured error handling
- **Ownership System**: Optional `move`, `invalidate`, and `borrow` keywords for compile-time use-after-move/invalidate detection and LLVM optimization hints
- **Enum Declarations**: Named integer constants with auto-increment
- **Default Parameters**: Optional function parameters with default values
- **Null Coalescing Operator**: `??` for concise null/zero fallback expressions
- **Multi-line Strings**: Triple-quoted `"""..."""` strings with embedded newlines
- **121 Built-in Functions**: Math, array manipulation, strings, maps, file I/O, threading, character classification, type conversion, and system calls
- **Adaptive JIT Runtime**: Hot functions are automatically recompiled at higher optimization levels using runtime profiling data

## Optimization Pipeline

OmScript runs a **three-layer optimizer** on top of LLVM's standard passes:

### Layer 1 — E-Graph Equality Saturation (O2+)
Applied to the AST **before** LLVM codegen. Uses 600+ algebraic rewrite rules to find provably equivalent, cheaper expressions:
- Constant folding, strength reduction (e.g. `x*3 → (x<<1)+x`, `x*15 → (x<<4)-x`)
- Algebraic identities (commutativity, associativity, distributivity)
- Bitwise absorption, shift combination, comparison simplification

### Layer 2 — Superoptimizer (O2+)
Applied to **LLVM IR** after the standard LLVM pipeline. Four passes:
1. **Idiom recognition** — recognizes patterns like `sdiv x,pow2 → ashr`, `x%pow2 → and`
2. **Algebraic simplification** — 300+ peephole rewrites on IR
3. **Branch-to-select** — converts simple conditional branches to `select` (CMOV)
4. **Synthesis** — generates optimal shift/add sequences for constant multiplies

### Layer 3 — Hardware Graph Optimization Engine (HGOE, -march/-mtune)
Activated only when `-march` or `-mtune` is explicitly provided. Builds a detailed hardware graph for the target microarchitecture (15+ supported profiles: Skylake, Haswell, Alder Lake, Zen 3/4/5, Apple M1–M4, Neoverse N2/V2, RISC-V, …) and performs:
- **Instruction scheduling** — per-basic-block list scheduler with cycle-accurate port model; uses real HardwareGraph execution-unit node counts and throughput, per-opcode latencies (BitCast/PHI are free, integer multiply uses only `mulPortCount` ports, etc.)
- **Port-diversity issue** — two-pass dispatch maximises IPC by filling different execution units each cycle
- **Register-pressure tiebreaker** — prefers instructions that free registers, reducing spills
- **FMA generation** — `fadd(fmul(a,b),c) → fma(a,b,c)`
- **Integer strength reduction** — `imul → shift+add` for constant multipliers
- **Software pipelining** — loop headers get `llvm.loop.unroll.count` / `interleave.count` / `vectorize.width` metadata derived from ResourceMII
- **Target attributes** — sets `target-cpu` and `target-features` (`+avx2`, `+avx512f`, `+sve`, etc.) on every function so the LLVM backend selects the right ISA extensions

## Optimization Levels

| Level | Description |
|-------|-------------|
| `-O0` | No optimization (fastest compilation) |
| `-O1` | Basic optimizations (instruction combining, CFG simplification, mem2reg) |
| `-O2` | Moderate optimizations — **default**. Enables E-graph + Superoptimizer. Full LLVM PM pipeline. |
| `-O3` | Aggressive. All O2 plus loop fusion/interchange, hot/cold splitting, aggressive peephole, etc. |

The HGOE activates on top of any level when `-march` or `-mtune` is set.

## Language Syntax

### Functions
```omscript
fn add(a, b) {
    return a + b;
}

// Default parameters
fn greet(name, greeting = "Hello") {
    println(greeting + " " + name + "!");
    return 0;
}
```
Forward references are fully supported — a function may call any other function defined anywhere in the file (or in imported files). Recursive and mutually recursive calls are also supported.

### Variables
```omscript
var x = 10;           // mutable variable
const y = 20;         // constant (immutable)
var z: int = 30;      // optional type annotation
var h = 0xFF;         // hex literal (255)
var o = 0o17;         // octal literal (15)
var b = 0b1010;       // binary literal (10)
var n = null;         // null value
var t = true;         // boolean true (1)
var f = false;        // boolean false (0)
```
Type annotations are optional in general but **required** inside `OPTMAX` blocks.

### Structs
```omscript
struct Point { x, y }

fn main() {
    var p = Point { x: 10, y: 20 };
    println(p.x);     // 10
    p.x = 30;         // field assignment
    println(p.x);     // 30
    return 0;
}
```
Structs are lightweight named record types. Fields are dynamically typed. Structs can be passed to and returned from functions.

### Import
```omscript
// math_utils.om
fn square(n) { return n * n; }
fn cube(n)   { return n * n * n; }

// main.om
import "math_utils.om";

fn main() {
    println(square(5));   // 25
    println(cube(3));     // 27
    return 0;
}
```
Circular and duplicate imports are silently detected and skipped. Imported files are parsed and merged into the current translation unit before codegen.

### Control Flow
```omscript
// If-else
if (condition) {
    // code
} else {
    // code
}

// While loop
while (condition) {
    // code
}

// Do-while loop (body executes at least once)
do {
    // code
} while (condition);

// For loop with range (exclusive end)
for (i in 0...10) {         // i = 0, 1, ..., 9
    // code
}

// With step
for (i in 0...100...5) {    // i = 0, 5, 10, ..., 95
    // code
}

// For-each loop over array
var arr = [10, 20, 30];
for (x in arr) {
    println(x);
}

// Switch/case
switch (value) {
    case 1:  println(1);  break;
    case 2:  println(2);  break;
    default: println(0);
}

// Error handling
try {
    throw 42;
} catch (err) {
    println(err);  // 42
}
```

### Arrays
```omscript
var arr = [1, 2, 3, 4, 5];
push(arr, 6);                     // append
var top = pop(arr);               // remove last
var n = len(arr);                 // length
var s = sum(arr);                 // 15
var found = array_contains(arr, 3);  // true
var doubled = array_map(arr, |x| x * 2);
var evens  = array_filter(arr, |x| x % 2 == 0);
var total  = array_reduce(arr, |acc, x| acc + x, 0);
sort(arr);
reverse(arr);
```

### Maps
```omscript
var m = map_new();
map_set(m, "key", 42);
var v = map_get(m, "key");       // 42
var ok = map_has(m, "key");      // true (1)
map_remove(m, "key");
var n = map_size(m);             // 0
var keys = map_keys(m);          // array of keys
var vals = map_values(m);        // array of values
```

### Strings
```omscript
var s = "hello";
var n = str_len(s);                       // 5
var up = str_upper(s);                    // "HELLO"
var lo = str_lower(s);                    // "hello"
var sub = str_substr(s, 1, 3);            // "ell"
var pos = str_index_of(s, "ll");          // 2
var r = str_replace(s, "l", "r");         // "herro"
var parts = str_split("a,b,c", ",");      // ["a", "b", "c"]
var joined = str_join(parts, "-");            // "a-b-c"
var cnt = str_count("abcabc", "abc");         // 2
var joined2 = str_concat("foo", "bar");       // "foobar"
var trimmed = str_trim("  hi  ");         // "hi"
var ts = to_string(42);                   // "42"
var n2 = str_to_int("100");              // 100
var f = str_to_float("3.14");            // 3.14
```

### File I/O
```omscript
var ok    = file_write("out.txt", "hello\n");
var text  = file_read("out.txt");             // "hello\n"
var exists = file_exists("out.txt");          // 1
file_append("out.txt", "world\n");
write("log.txt", "entry\n");                  // alias for file_write
```

### Threading
```omscript
fn worker(arg) {
    println(arg);
    return 0;
}

fn main() {
    var t = thread_create(worker, 99);
    thread_join(t);
    return 0;
}
```
Mutex primitives: `mutex_new()`, `mutex_lock(m)`, `mutex_unlock(m)`, `mutex_destroy(m)`.

### Lambda Expressions
```omscript
var doubled = array_map([1, 2, 3], |x| x * 2);     // [2, 4, 6]
var sum     = array_reduce([1, 2, 3], |acc, x| acc + x, 0);  // 6
var evens   = array_filter([1, 2, 3, 4], |x| x % 2 == 0);   // [2, 4]
```
Lambdas are compile-time constructs; they do not capture variables from the enclosing scope.

### Pipe Operator
```omscript
fn double(x) { return x * 2; }
var result = 5 |> double;     // 10
var n      = len([1,2,3]);    // equivalent: [1,2,3] |> len → 3
```

### Spread Operator
```omscript
var a = [1, 2, 3];
var b = [0, ...a, 4];    // [0, 1, 2, 3, 4]
```

### Enums
```omscript
enum Color { RED, GREEN, BLUE }
// RED=0, GREEN=1, BLUE=2
var c = Color.GREEN;     // 1
```

### OPTMAX Blocks
Tag performance-critical functions with `OPTMAX=:` / `OPTMAX!:` to enable the maximum optimization stack (beyond O3), AST constant folding, and LLVM OPTMAX-only passes. Inside OPTMAX functions, all variables and parameters **must** carry type annotations, and only other OPTMAX functions may be called.

```omscript
OPTMAX=:
fn dot_product(n: int, xs: int, ys: int) {
    var total: float = 0.0;
    for (i: int in 0...n) {
        total = total + xs[i] * ys[i];
    }
    return total;
}
OPTMAX!:
```

### Expressions
| Category | Operators |
|----------|-----------|
| Arithmetic | `+` `-` `*` `/` `%` `**` (exponentiation) |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Logical | `&&` `\|\|` `!` |
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Ternary | `cond ? a : b` |
| Null coalescing | `value ?? fallback` |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |
| Increment/Decrement | `++x` `--x` (prefix) `x++` `x--` (postfix) |
| Pipe | `expr \|> fn` |
| Lambda | `\|x\| expr` |

### Comments
```omscript
// Single-line comment
/* Multi-line
   block comment */
var x = 10; /* inline */
```

## Built-in Functions (121 total)

### Math
| Function | Description |
|----------|-------------|
| `abs(x)` | Absolute value |
| `ceil(x)` | Ceiling (float → int) |
| `floor(x)` | Floor (float → int) |
| `round(x)` | Round to nearest integer |
| `sqrt(x)` | Integer square root |
| `pow(b, e)` | Integer exponentiation |
| `log2(x)` | Integer log base 2 |
| `log(x)` | Natural logarithm (float) |
| `log10(x)` | Base-10 logarithm (float) |
| `exp(x)` | Exponential e^x (float) |
| `sin(x)` | Sine (float) |
| `cos(x)` | Cosine (float) |
| `tan(x)` | Tangent (float) |
| `asin(x)` | Arc sine (float) |
| `acos(x)` | Arc cosine (float) |
| `atan(x)` | Arc tangent (float) |
| `atan2(y, x)` | Two-argument arc tangent (float) |
| `cbrt(x)` | Cube root (float) |
| `hypot(x, y)` | Hypotenuse sqrt(x²+y²) (float) |
| `gcd(a, b)` | Greatest common divisor |
| `min(a, b)` | Minimum of two values |
| `max(a, b)` | Maximum of two values |
| `clamp(x, lo, hi)` | Clamp x to [lo, hi] |
| `sign(x)` | Sign: -1, 0, or 1 |
| `sum(arr)` | Sum of array elements |

### Array
| Function | Description |
|----------|-------------|
| `len(arr)` | Array length |
| `push(arr, v)` | Append element |
| `pop(arr)` | Remove and return last element |
| `sort(arr)` | Sort in-place |
| `reverse(arr)` | Reverse in-place |
| `swap(arr, i, j)` | Swap two elements |
| `index_of(arr, v)` | First index of value, or -1 |
| `array_contains(arr, v)` | True if value is in array |
| `array_min(arr)` | Minimum element of array |
| `array_max(arr)` | Maximum element of array |
| `array_find(arr, v)` | Index of first match, or -1 |
| `array_any(arr, fn)` | True if any element matches predicate |
| `array_every(arr, fn)` | True if all elements match predicate |
| `array_count(arr, fn)` | Count elements matching predicate |
| `array_map(arr, fn)` | Map function over elements |
| `array_filter(arr, fn)` | Filter by predicate |
| `array_reduce(arr, fn, init)` | Left-fold with initial value |
| `array_slice(arr, start, end)` | Slice subarray |
| `array_concat(a, b)` | Concatenate two arrays |
| `array_copy(arr)` | Shallow copy |
| `array_fill(n, v)` | Create array of n copies of v |
| `array_remove(arr, i)` | Remove element at index |
| `range(n)` | Array `[0, 1, ..., n-1]` |
| `range_step(start, end, step)` | Array with step |

### String
| Function | Description |
|----------|-------------|
| `str_len(s)` | String length |
| `str_concat(a, b)` | Concatenate |
| `str_substr(s, start, len)` | Substring |
| `str_upper(s)` / `str_lower(s)` | Case conversion |
| `str_trim(s)` | Strip leading/trailing whitespace |
| `str_replace(s, old, new)` | Replace all occurrences |
| `str_contains(s, sub)` | Substring test |
| `str_starts_with(s, pre)` / `str_ends_with(s, suf)` | Prefix/suffix test |
| `str_index_of(s, sub)` / `str_find(s, sub)` | Find position |
| `str_split(s, delim)` | Split into array |
| `str_join(arr, delim)` | Join array of strings with delimiter |
| `str_count(s, sub)` | Count non-overlapping occurrences |
| `str_chars(s)` | Array of character codes |
| `str_repeat(s, n)` | Repeat n times |
| `str_reverse(s)` | Reverse string |
| `str_eq(a, b)` | String equality |
| `str_to_int(s)` / `str_to_float(s)` | Parse string to number |
| `to_string(x)` / `number_to_string(x)` / `string_to_number(s)` | Conversions |
| `char_at(s, i)` | Character code at index |
| `char_code(c)` | Character code of first char |
| `to_char(n)` | Integer to single-char string |

### Map
| Function | Description |
|----------|-------------|
| `map_new()` | Create empty map |
| `map_set(m, key, val)` | Insert/update key |
| `map_get(m, key)` | Get value (null if absent) |
| `map_has(m, key)` | Test for key |
| `map_remove(m, key)` | Remove key |
| `map_size(m)` | Number of entries |
| `map_keys(m)` | Array of keys |
| `map_values(m)` | Array of values |

### I/O
| Function | Description |
|----------|-------------|
| `print(x)` | Print value with newline |
| `println(x)` | Print value with newline (alias) |
| `print_char(n)` | Print character with given code |
| `input()` | Read integer from stdin |
| `input_line()` | Read line string from stdin |
| `write(path, text)` | Write text to file |
| `file_read(path)` | Read entire file as string |
| `file_write(path, text)` | Write text to file |
| `file_append(path, text)` | Append text to file |
| `file_exists(path)` | Test file existence |

### Threading
| Function | Description |
|----------|-------------|
| `thread_create(fn, arg)` | Spawn thread, returns handle |
| `thread_join(t)` | Wait for thread to finish |
| `mutex_new()` | Create mutex |
| `mutex_lock(m)` | Acquire mutex |
| `mutex_unlock(m)` | Release mutex |
| `mutex_destroy(m)` | Free mutex |

### Type / System
| Function | Description |
|----------|-------------|
| `typeof(x)` | Return type name string |
| `to_int(x)` / `to_float(x)` | Type conversion |
| `is_even(n)` / `is_odd(n)` | Parity test |
| `is_alpha(n)` / `is_digit(n)` | Character classification |
| `assert(cond)` | Abort if false |
| `exit_program(code)` | Exit with code |
| `random()` | Random integer |
| `sleep(ms)` | Sleep milliseconds |
| `time()` | Current Unix time |

## Building

### Prerequisites
- CMake 3.13 or higher
- C++17 compatible compiler (GCC or Clang)
- LLVM 17+ development libraries
- GCC (for linking)

### Build Instructions
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-18/cmake
make -j$(nproc)
```

## Usage

```bash
# Compile a source file to an executable
./build/omsc source.om -o output

# Compile and immediately run (hybrid JIT mode)
./build/omsc run source.om

# Validate syntax without compiling
./build/omsc check source.om

# Inspect tokens
./build/omsc lex source.om

# Parse and summarize the AST
./build/omsc parse source.om

# Emit LLVM IR
./build/omsc emit-ir source.om
./build/omsc emit-ir source.om -o output.ll

# Optimization levels
./build/omsc run source.om -O0   # No optimization
./build/omsc run source.om -O2   # Default
./build/omsc run source.om -O3   # Aggressive

# Target a specific CPU (enables HGOE instruction scheduler)
./build/omsc source.om -march=skylake -o out
./build/omsc source.om -march=znver4  -o out
./build/omsc source.om -march=native  -o out

# Other codegen flags
./build/omsc source.om -flto -ffast-math -fvectorize

# Diagnostics
./build/omsc run source.om --time        # Show timing breakdown
./build/omsc run source.om --keep-temps  # Keep temp files

# Package manager
./build/omsc pkg install <package>
./build/omsc pkg remove  <package>
./build/omsc pkg list
./build/omsc pkg search  <query>

# Install omsc to PATH
./build/omsc install
```

### Compiler Flags Reference

| Flag | Description | Default |
|------|-------------|---------|
| `-O0` / `-O1` / `-O2` / `-O3` | Optimization level | `-O2` |
| `-march=<cpu>` | Target CPU architecture; activates HGOE | `native` |
| `-mtune=<cpu>` | CPU scheduling tuning | same as `-march` |
| `-flto` | Link-time optimization | off |
| `-ffast-math` | Unsafe FP optimizations | off |
| `-fvectorize` | SIMD vectorization hints | on |
| `-funroll-loops` | Loop unrolling | on |
| `-floop-optimize` | Polyhedral loop optimizations (Polly) | on |
| `-fpic` | Position-independent code | on |
| `-foptmax` | OPTMAX block optimization | on |
| `-fjit` | Hybrid JIT for `omsc run` | on |
| `-fstack-protector` | Stack buffer overflow protection | off |
| `-static` | Static linking | off |
| `-s` / `--strip` | Strip debug symbols | off |
| `--pgo-gen=<path>` | Generate PGO instrumentation profile | — |
| `--pgo-use=<path>` | Use PGO profile for optimization | — |
| `-v` / `--verbose` | Show compilation details | off |
| `-q` / `--quiet` | Suppress non-error output | off |
| `--time` | Show timing breakdown | off |

Use `-fno-<flag>` to disable any `-f` flag (e.g. `-fno-vectorize`, `-fno-jit`).

## Examples

### Sum of a Range
```omscript
fn sum_range(n) {
    var total = 0;
    for (i in 0...n) {
        total = total + i;
    }
    return total;
}
fn main() { return sum_range(100); }  // 4950
```

### Factorial
```omscript
fn factorial(n) {
    if (n <= 1) { return 1; }
    return n * factorial(n - 1);
}
fn main() { return factorial(10); }
```

### Fibonacci (Iterative)
```omscript
fn fib(n) {
    if (n <= 1) { return n; }
    var a = 0; var b = 1;
    for (i in 2...n+1) {
        var t = a + b; a = b; b = t;
    }
    return b;
}
fn main() { return fib(30); }
```

### Struct Usage
```omscript
struct Vec2 { x, y }

fn dot(u, v) { return u.x * v.x + u.y * v.y; }

fn main() {
    var a = Vec2 { x: 3, y: 4 };
    var b = Vec2 { x: 1, y: 2 };
    println(dot(a, b));   // 11
    return 0;
}
```

### Map Usage
```omscript
fn word_count(words) {
    var counts = map_new();
    for (w in words) {
        if (map_has(counts, w)) {
            map_set(counts, w, map_get(counts, w) + 1);
        } else {
            map_set(counts, w, 1);
        }
    }
    return counts;
}
```

## Architecture

### Compiler Pipeline
```
Source (.om)
    │
    ├─ Lexer          → token stream
    ├─ Parser         → AST
    ├─ E-graph        → optimized AST   (O2+)
    ├─ CodeGen        → LLVM IR
    ├─ Superoptimizer → improved IR     (O2+)
    ├─ LLVM Pipeline  → optimized IR
    ├─ HGOE           → scheduled IR    (-march/-mtune)
    └─ LLVM Backend   → native object → executable
```

### Adaptive JIT Runtime
When using `omsc run`, the program executes through a hybrid AOT + tiered JIT:

- **Tier 1**: JIT-compiled at O2 via LLVM MCJIT; execution begins immediately
- **Runtime Profiling**: Call counts, branch probabilities, argument types, and observed constants are tracked
- **Tier 2 (Hot Recompile)**: Functions exceeding a call-count threshold are recompiled at O3 with profile-guided hints (PGO entry counts, branch weights)
- **Deoptimization**: Guard-based fallback to baseline code when speculative assumptions fail

### Runtime Safety Features
- **Array bounds checking**: All array accesses are bounds-checked
- **Division by zero**: Integer division and modulo operations check for zero divisor
- **For-loop zero step**: Aborts on zero-step for-loops
- **Wrapping integer arithmetic**: Two's-complement semantics (no undefined behaviour on overflow)
- **Parser nesting limit**: Maximum recursion depth of 256
- **IR instruction budget**: Compilation aborts above 1,000,000 IR instructions
- **File size limit**: Source files larger than 100 MB are rejected

## Type System
| Type | Description |
|------|-------------|
| `int` | 64-bit signed integer |
| `float` | 64-bit double-precision float |
| `string` | Reference-counted UTF-8 string |
| `array` | Dynamically-sized heterogeneous array |
| `map` | Hash map with string keys |
| `bool` | `true` (1) / `false` (0), stored as integer |
| `null` | Absence of value |

Types are determined at runtime; LLVM compiles each operation to native instructions.

## Testing

```bash
# Unit tests (requires libgtest-dev)
cd build && ctest --output-on-failure

# Integration tests (125 example programs)
bash run_tests.sh
```

## Project Structure

```
omscript/
├── CMakeLists.txt
├── include/
│   ├── ast.h               # AST node definitions
│   ├── codegen.h           # LLVM code generator
│   ├── compiler.h          # Compiler driver
│   ├── diagnostic.h        # Diagnostics
│   ├── egraph.h            # E-graph optimizer
│   ├── hardware_graph.h    # Hardware Graph Optimization Engine
│   ├── lexer.h
│   ├── parser.h
│   ├── superoptimizer.h    # Superoptimizer
│   └── version.h
├── src/
│   ├── ast.cpp
│   ├── codegen.cpp         # LLVM IR generation
│   ├── codegen_builtins.cpp
│   ├── codegen_expr.cpp
│   ├── codegen_opt.cpp     # Optimization pass pipeline
│   ├── codegen_stmt.cpp
│   ├── compiler.cpp
│   ├── egraph.cpp          # E-graph rewrite rules
│   ├── egraph_optimizer.cpp
│   ├── hardware_graph.cpp  # HGOE scheduler + profiles
│   ├── lexer.cpp
│   ├── main.cpp
│   ├── parser.cpp
│   └── superoptimizer.cpp
├── runtime/
│   ├── aot_profile.cpp     # Adaptive JIT / hot recompilation
│   ├── deopt.cpp           # Guard-based deoptimization
│   ├── jit_profiler.cpp    # Runtime profiling
│   ├── refcounted.h        # Reference-counted string type
│   └── value.cpp/h         # Dynamic value representation
├── tests/                  # GTest unit tests (14 suites)
├── examples/               # 125+ example programs
└── user-packages/          # Installable packages
```

## Cross-Platform Support
- **Linux** (x86_64, AArch64, ARM) — primary platform
- **macOS** (x86_64, Apple Silicon)
- **Windows** (x86_64 via MSVC)

## License

MIT License — see `LICENSE`.

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.  
Detailed documentation:
- [LANGUAGE_REFERENCE.md](LANGUAGE_REFERENCE.md) — full language, ownership system, optimizers, memory management, and stdlib reference
- [CHANGELOG.md](CHANGELOG.md) — version history
