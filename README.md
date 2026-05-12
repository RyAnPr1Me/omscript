# OmScript

A low-level, C-like systems programming language with **static typing**, **explicit memory management** (no garbage collector, no reference counting), and a **heavily optimized AOT compiler** built on LLVM. OmScript pairs a safe, ownership-aware surface syntax with a multi-layer optimization engine — AST pre-passes, equality-saturation E-graph, superoptimizer, and a hardware-graph-driven instruction scheduler — that produces near-optimal native machine code for each target CPU.

**Current version: 4.4.0** | [Language Reference](LANGUAGE_REFERENCE.md) | [Changelog](CHANGELOG.md)

---

## Key Features

- **C-like Syntax**: Familiar to C/C++ and JavaScript programmers
- **Static Typing**: Mandatory type annotations on all declarations; all types map 1-to-1 to native LLVM types — no runtime type tags
- **Structs**: Lightweight named record types with field access, `@repr` layout control, and field-level alignment attributes
- **Modules / Import**: Split programs across files with `import "file.om";` — duplicate/circular imports are silently deduplicated
- **Aggressive AOT Compilation**: Multi-level LLVM optimization (O0–O3) with five compiler-specific passes layered on top
- **Explicit Memory Management**:
  - `alloc<T>(n)` — compile-time smart allocator with three tiers (T1: stack alloca, T2: per-function arena, T3: heap). Returns **raw uninitialised** memory.
  - `new T(n)` — same tier selection as `alloc<T>(n)`, but **zero-initialises** the result (T1/T2: `memset(0)`, T3: `calloc`).
  - `new T { field: val, ... }` — allocate + initialise specific fields in one expression (zero abstraction cost: `alloc` + GEP/store pairs).
  - `construct p { field: val, ... };` — in-place field initialisation on already-allocated memory; lowers to typed GEP + store pairs with TBAA metadata.
  - `invalidate p;` — explicit `free()` / lifetime.end with borrow-checker enforcement.
- **Lambda Expressions**: Anonymous functions with `|x| x * 2` syntax for higher-order builtins
- **Pipe Operator**: Left-to-right function chaining with `expr |> fn`
- **Spread Operator**: Array unpacking in literals with `[1, ...arr, 2]`
- **Method-Call Syntax**: `obj.method(args)` desugars to `method(obj, args)` — OOP-style code with zero runtime overhead
- **Range-Based For Loops**: `for (i in start...end)` and `for (i in start...end...step)`
- **For-Each Loops**: `for (x in array)` over array elements
- **Switch/Case**: Multi-way branching with multi-value `case 1, 2, 3:`
- **Do-While Loops**: Execute body at least once with `do { ... } while (cond);`
- **Error Handling**: `throw N;` + per-function `catch(N) { ... }` — zero-cost integer-keyed dispatch, no stack unwinding
- **Ownership System**: `move`, `invalidate`, `borrow`, `borrow mut`, `freeze`, `reborrow` — compile-time lifetime tracking that feeds LLVM alias analysis
- **`atomic var`**: Every load, store, and RMW (`++`/`--`, `+=`, `&=`, etc.) becomes a sequentially-consistent LLVM atomic — lock-free shared state without mutex overhead
- **`volatile var`**: Every load and store is LLVM-volatile — prevents optimizer elision; essential for memory-mapped I/O and signal-handler variables
- **`comptime {}` Blocks** (preferred over preprocessor macros):
  - *Top-level*: define compile-time constants, conditional compilation, preprocessor migration — full type system, IDE-friendly, scope-aware
  - *Expression-context*: evaluated at compile time by CF-CTRE; result replaces the expression with a constant
- **`#define` / `#if` / `#ifdef` Preprocessor**: Traditional text-based macros and conditional compilation; `comptime {}` is recommended for new code
- **CF-CTRE (Cross-Function Compile-Time Reasoning Engine)**: Executes pure functions across call boundaries at compile time with memoisation, SIMD-tile semantics, and fixed-point purity analysis (§28)
- **Integer Type-Cast Syntax**: `u8(x)`, `u16(x)`, `u32(x)`, `u64(x)`, `i8(x)`, `i16(x)`, `i32(x)`, `i64(x)`, `bool(x)` — fold at compile time inside `comptime` blocks
- **`parallel` Loops**: `parallel for`/`while`/`foreach` emits parallelization metadata for auto-vectorization and parallel execution
- **Enum Declarations**: Named integer constants with auto-increment
- **Default Parameters**: Optional function parameters with default values
- **Null Coalescing Operator**: `??` for concise null/zero fallback expressions
- **String Interpolation**: `$"hello {name}, count = {n + 1}"` with auto type conversion
- **Multi-line Strings**: Triple-quoted `"""..."""` strings with embedded newlines
- **`byte` / `byte[]`**: `byte` is an alias for `u8`; `x as byte[]` emits the raw little-endian byte representation
- **140+ Built-in Functions**: Math, array manipulation, strings, maps, file I/O, threading, character classification, type conversion, and system calls
- **Optimization Feedback**: Run with `-v` to see what the compiler folded, inlined, stack-allocated, and fused

## Optimization Pipeline

OmScript runs a **three-layer optimizer** on top of LLVM's standard passes:

### Layer 0 — AST-Level Pre-Passes (O1+)
Run before LLVM codegen on the parsed AST:
- **Cross-function constant propagation** — zero-argument pure functions whose return value is always a compile-time constant are identified via fixed-point analysis and inlined as constants at every call site
- **CF-CTRE** — deterministic compile-time interpreter that executes pure functions across call boundaries, memoises results by `(fn, args_hash)`, and back-propagates results into the constant fold tables; enables `comptime { deep_call_chain(42) }` to evaluate entire function graphs at compile time
- **`comptime {}` evaluation** — compile-time blocks are fully executed by the interpreter at compile time; results replace the expression with a literal constant
- **`@fuse` loop fusion** — adjacent `for` loops over identical ranges are merged into a single loop body, reducing loop overhead and improving cache locality
- **`@independent` access groups** — emits `llvm.access.group` + `llvm.loop.parallel_accesses` to suppress loop-carried alias analysis conservatism
- **Escape analysis** — small integer-literal array allocations proven not to escape are stack-allocated (`alloca`) instead of heap-allocated, eliminating malloc/free overhead entirely

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

// Atomic variable — every load/store/RMW is seq-cst atomic (lock-free)
global atomic var counter: i64 = 0;
counter++;            // atomicrmw add … seq_cst — safe from multiple threads

// Volatile variable — every load/store is LLVM-volatile (never cached/elided)
volatile var status: i64 = 0;
while (status == 0) {} // re-reads status every iteration

// Combined
atomic volatile var hw_reg: i64 = 0;
```
Type annotations are optional in general but **required** inside `OPTMAX` blocks.

### comptime Blocks
```omscript
// Top-level comptime block — type-safe, IDE-friendly replacement for #define / #ifdef
comptime {
    const MAX_RETRIES: int = 3;
    const APP_NAME: string = "my_server";

    if (OS == "linux") {
        const USE_EPOLL: int = 1;
    } else {
        const USE_EPOLL: int = 0;
    }

    if (!defined(NDEBUG)) {
        warning("building with assertions enabled");
    }
}

// Expression-context comptime — evaluated at compile time by CF-CTRE
var lut: int[] = comptime {
    var result: int[] = array_fill(256, 0);
    for (i: int in 0...256) {
        result[i] = popcount(i);  // popcount lookup table, built at compile time
    }
    return result;
};
```

### Memory Management
```omscript
struct Vec2 { x: int; y: int; }

fn main() -> int {
    // alloc<T>(n) — raw (uninitialised) smart allocation
    // T1: stack alloca (≤8 KiB constant), T2: arena, T3: heap malloc
    var buf: ptr<i64> = alloc<i64>(16);   // T1: stack alloca, raw memory
    buf[0] = 42;

    // new T(n) — zero-initialised smart allocation
    // Same tiers as alloc<T>(n) but all bytes are zeroed
    var zbuf: ptr<i64> = new i64(16);     // T1: stack alloca + memset(0)

    // new T { field: val } — allocate + initialise fields in one expression
    var v: ptr<Vec2> = new Vec2 { x: 3, y: 4 };
    println(v->x);   // 3
    println(v->y);   // 4

    // construct — in-place field init on already-allocated memory (zero-cost)
    var w: ptr<Vec2> = alloc<Vec2>(1);
    construct w {
        x: 10,
        y: 20,
    };
    println(w->x);   // 10

    invalidate buf;   // free (ownership enforced at compile time)
    invalidate v;
    invalidate w;
    // zbuf is stack-allocated — no invalidate needed
    return 0;
}
```

### Structs
```omscript
struct Point { x: int; y: int; }

fn main() -> int {
    var p = Point { x: 10, y: 20 };
    println(p.x);     // 10
    p.x = 30;         // field assignment
    println(p.x);     // 30
    return 0;
}
```
Structs are lightweight named record types. All fields require type annotations. Structs can be passed to and returned from functions by value.

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

// Switch/case with multi-value matching
switch (value) {
    case 1, 2, 3: println("small"); break;
    case 4, 5:    println("medium"); break;
    default:      println("large");
}

// Error handling — top-level catch(N) blocks and `throw <int>;`
// (no `try {}` block, no exception variable; see §16 of the Language Reference)
fn risky() {
    throw 42;
    return 0;

    catch(42) {
        println("caught 42");
        return -1;
    }
}
risky();
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

// String interpolation
var name = "world";
var greeting = $"hello {name}!";          // "hello world!"
var result = $"{n} + {f} = {n + f}";      // "5 + 3.14 = 8.14"
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
fn worker() {
    println("hello from thread");
}

fn main() {
    var t = thread_create("worker");   // function name as a string literal
    thread_join(t);
    return 0;
}
```
`thread_create` takes the **name of a top-level function as a string literal** and runs it on a new pthread with no arguments. Use `global var` + a mutex to communicate with the worker. See §20 of the Language Reference for the full concurrency model.

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
| Category | Operators / Syntax |
|----------|--------------------|
| Arithmetic | `+` `-` `*` `/` `%` `**` (exponentiation) |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Chained comparison | `1 < x < 10` → `(1 < x) && (x < 10)` |
| Logical | `&&` `\|\|` `!` |
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Ternary | `cond ? a : b` |
| Null coalescing | `value ?? fallback` |
| Elvis | `value ?: fallback` |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` `**=` `??=` `&&=` `\|\|=` |
| Increment/Decrement | `++x` `--x` (prefix) `x++` `x--` (postfix) |
| Pipe | `expr \|> fn` |
| Lambda | `\|x\| expr` |
| `in` operator | `x in arr` → `array_contains(arr, x)` |
| Spread | `[1, ...arr, 2]` |
| Type-cast | `u8(x)` `u16(x)` `u32(x)` `u64(x)` `i8(x)` `i16(x)` `i32(x)` `i64(x)` `bool(x)` |
| Compile-time | `comptime { expr; }` |

### Comments
```omscript
// Single-line comment
/* Multi-line
   block comment */
var x = 10; /* inline */
```

## Built-in Functions (140+ total)

### Math
| Function | Description | Compile-Time Folds |
|----------|-------------|--------------------|
| `abs(x)` | Absolute value | ✓ literal |
| `ceil(x)` | Ceiling (float → int) | ✓ literal |
| `floor(x)` | Floor (float → int) | ✓ literal |
| `round(x)` | Round to nearest integer | ✓ literal |
| `sqrt(x)` | Integer square root | ✓ literal |
| `pow(b, e)` | Integer exponentiation | ✓ literals |
| `log2(x)` | Integer log base 2 | ✓ literal |
| `log(x)` | Natural logarithm (float) | — |
| `log10(x)` | Base-10 logarithm (float) | — |
| `exp(x)` | Exponential e^x (float) | — |
| `exp2(x)` | 2^x | ✓ literal |
| `sin(x)` | Sine (float) | — |
| `cos(x)` | Cosine (float) | — |
| `tan(x)` | Tangent (float) | — |
| `asin(x)` | Arc sine (float) | — |
| `acos(x)` | Arc cosine (float) | — |
| `atan(x)` | Arc tangent (float) | — |
| `atan2(y, x)` | Two-argument arc tangent (float) | — |
| `cbrt(x)` | Cube root (float) | — |
| `hypot(x, y)` | Hypotenuse sqrt(x²+y²) (float) | — |
| `gcd(a, b)` | Greatest common divisor | ✓ literals |
| `lcm(a, b)` | Least common multiple | ✓ literals |
| `min(a, b)` | Minimum of two values | ✓ literals |
| `max(a, b)` | Maximum of two values | ✓ literals |
| `min_float(a, b)` | Floating-point minimum (NaN-aware) | — |
| `max_float(a, b)` | Floating-point maximum (NaN-aware) | — |
| `clamp(x, lo, hi)` | Clamp x to [lo, hi] | ✓ literals |
| `sign(x)` | Sign: -1, 0, or 1 | ✓ literal |
| `is_even(n)` | 1 if even, 0 otherwise | ✓ literal |
| `is_odd(n)` | 1 if odd, 0 otherwise | ✓ literal |
| `is_power_of_2(n)` | 1 if power of 2, 0 otherwise | ✓ literal |
| `fma(a, b, c)` | Fused multiply-add (a×b+c) | — |
| `copysign(x, y)` | Magnitude of x with sign of y | — |
| `saturating_add(a, b)` | Saturating addition (clamps at INT64_MAX) | ✓ literals |
| `saturating_sub(a, b)` | Saturating subtraction (clamps at INT64_MIN) | ✓ literals |
| `sum(arr)` | Sum of array elements | ✓ constant array |
| `fast_add/sub/mul/div(a,b)` | Arithmetic with `nsw` flag | — |
| `precise_add/sub/mul/div(a,b)` | Arithmetic without unsafe flags | — |

### Bit Manipulation
| Function | Description | Compile-Time Folds |
|----------|-------------|--------------------|
| `popcount(x)` | Count set bits (POPCNT) | ✓ literal |
| `clz(x)` | Count leading zeros (LZCNT) | ✓ literal |
| `ctz(x)` | Count trailing zeros (TZCNT) | ✓ literal |
| `bitreverse(x)` | Reverse bit order | ✓ literal |
| `bswap(x)` | Byte-swap (endianness) | ✓ literal |
| `rotate_left(x, n)` | Rotate left by n | ✓ literals |
| `rotate_right(x, n)` | Rotate right by n | ✓ literals |

### Integer Type-Casts (v4.1.1)
| Function | Description | Compile-Time Folds |
|----------|-------------|--------------------|
| `u64(x)` / `i64(x)` / `int(x)` / `uint(x)` | Identity (no-op) | ✓ |
| `u32(x)` | Mask to 32 bits (`x & 0xFFFFFFFF`) | ✓ |
| `i32(x)` | Truncate + sign-extend to 32 bits | ✓ |
| `u16(x)` | Mask to 16 bits (`x & 0xFFFF`) | ✓ |
| `i16(x)` | Truncate + sign-extend to 16 bits | ✓ |
| `u8(x)` | Mask to 8 bits (`x & 0xFF`) | ✓ |
| `i8(x)` | Truncate + sign-extend to 8 bits | ✓ |
| `bool(x)` | Normalize to 0 or 1 | ✓ |

### Array
| Function | Description | Compile-Time Folds |
|----------|-------------|--------------------|
| `len(arr)` | Array length | ✓ for `array_fill`, `range`, `range_step`, `array_concat`, `str_chars` |
| `push(arr, v)` | Append element | — |
| `pop(arr)` | Remove and return last element | — |
| `sort(arr)` | Sort in-place | — |
| `reverse(arr)` | Reverse in-place | — |
| `swap(arr, i, j)` | Swap two elements (bounds-checked) | — |
| `index_of(arr, v)` | First index of value, or -1 | ✓ constant array |
| `array_contains(arr, v)` | 1 if value is in array, 0 otherwise | ✓ constant array |
| `array_min(arr)` | Minimum element | ✓ constant array |
| `array_max(arr)` | Maximum element | ✓ constant array |
| `array_find(arr, fn)` | Index of first match, or -1 | ✓ constant array |
| `array_any(arr, fn)` | 1 if any element matches predicate | — |
| `array_every(arr, fn)` | 1 if all elements match predicate | — |
| `array_count(arr, fn)` | Count elements matching predicate | — |
| `array_map(arr, fn)` | Map function over elements | — |
| `array_filter(arr, fn)` | Filter by predicate | — |
| `array_reduce(arr, fn, init)` | Left-fold with initial value | — |
| `array_slice(arr, start, end)` | Sub-array [start, end) | — |
| `array_concat(a, b)` | Concatenate two arrays | ✓ `len()` constant-folds |
| `array_copy(arr)` | Shallow copy | — |
| `array_fill(n, v)` | Create array of n copies of v | ✓ `len()` constant-folds |
| `array_remove(arr, i)` | Remove element at index | — |
| `array_insert(arr, i, v)` | Insert v at index i | — |
| `array_product(arr)` | Product of all elements | ✓ constant array |
| `array_last(arr)` | Last element | ✓ constant array |
| `range(start, end)` | Array `[start..end-1]` | ✓ `len()` constant-folds |
| `range_step(start, end, step)` | Array with step | ✓ `len()` constant-folds |
| `sum(arr)` | Sum of all elements | ✓ constant array, `array_fill`, `range` |

### String
| Function | Description | Compile-Time Folds |
|----------|-------------|--------------------|
| `str_len(s)` / `len(s)` | String length | ✓ string literal |
| `str_concat(a, b)` | Concatenate two strings | ✓ string literals |
| `str_substr(s, start, len)` | Substring | — |
| `str_upper(s)` / `str_lower(s)` | Case conversion | ✓ string literal |
| `str_trim(s)` | Strip leading/trailing whitespace | ✓ string literal |
| `str_replace(s, old, new)` | Replace all occurrences | ✓ string literals |
| `str_contains(s, sub)` | Substring test | ✓ string literals |
| `str_starts_with(s, pre)` / `str_ends_with(s, suf)` | Prefix/suffix test | ✓ string literals |
| `str_index_of(s, sub)` | First index of substring (-1 if not found) | ✓ string literals |
| `str_find(s, code)` | First index of character code | — |
| `str_split(s, delim)` | Split into array | — |
| `str_join(arr, delim)` | Join array of strings with delimiter | — |
| `str_count(s, sub)` | Count non-overlapping occurrences | ✓ string literals |
| `str_chars(s)` | Array of character codes (integers) | ✓ `len()` constant-folds |
| `str_repeat(s, n)` | Repeat n times | ✓ string literal + literal n |
| `str_reverse(s)` | Reverse string | ✓ string literal |
| `str_eq(a, b)` | String equality (1/0) | ✓ string literals |
| `str_to_int(s)` / `str_to_float(s)` | Parse string to number | ✓ string literal |
| `str_pad_left(s, n, ch)` | Left-pad to width n | ✓ string literal |
| `str_pad_right(s, n, ch)` | Right-pad to width n | ✓ string literal |
| `to_string(x)` / `number_to_string(x)` | Number to string | — |
| `string_to_number(s)` / `to_int(x)` / `to_float(x)` | Conversions | — |
| `char_at(s, i)` | Character code at index (bounds-checked) | — |
| `char_code(c)` | Code point of first char | — |
| `to_char(n)` | Integer to single-character string | ✓ literal |
| `is_alpha(n)` | 1 if alphabetic, 0 otherwise | ✓ literal |
| `is_digit(n)` | 1 if decimal digit, 0 otherwise | ✓ literal |

### Map
| Function | Description |
|----------|-------------|
| `map_new()` | Create empty map |
| `map_set(m, key, val)` | Insert/update key |
| `map_get(m, key)` / `map_get(m, key, default)` | Get value (null or default if absent) |
| `map_has(m, key)` | 1 if key exists, 0 otherwise |
| `map_remove(m, key)` | Remove key |
| `map_size(m)` | Number of entries |
| `map_keys(m)` | Array of keys |
| `map_values(m)` | Array of values |

### I/O
| Function | Description |
|----------|-------------|
| `print(x)` | Print value with newline |
| `println(x)` | Print value with newline (alias for `print`) |
| `write(val)` | Print value WITHOUT trailing newline |
| `print_char(n)` | Print character with given ASCII code |
| `input()` | Read whitespace-delimited word from stdin |
| `input_line()` | Read full line from stdin |
| `exit(code)` / `exit_program(code)` | Exit with code |
| `file_read(path)` | Read entire file as string |
| `file_write(path, text)` | Write text to file (overwrite) |
| `file_append(path, text)` | Append text to file |
| `file_exists(path)` | 1 if file exists, 0 otherwise |

### Threading
| Function | Description |
|----------|-------------|
| `thread_create(fn)` | Spawn thread, returns handle |
| `thread_join(t)` | Wait for thread to finish |
| `mutex_new()` | Create mutex |
| `mutex_lock(m)` | Acquire mutex |
| `mutex_unlock(m)` | Release mutex |
| `mutex_destroy(m)` | Free mutex |

### Type / System / Optimizer Hints
| Function | Description |
|----------|-------------|
| `typeof(x)` | **Deprecated** — compile-time type tag (1=int, 2=float, 3=string); use type annotations |
| `to_int(x)` / `to_float(x)` | Type conversion |
| `assert(cond)` | Abort if false (runtime assertion) |
| `exit_program(code)` | Exit with code |
| `random()` | Random float in [0, 1) |
| `sleep(ms)` | Sleep milliseconds |
| `time()` | Current Unix timestamp (seconds) |
| `assume(cond)` | LLVM `llvm.assume` optimizer hint |
| `unreachable()` | Mark as unreachable (UB if executed) |
| `expect(val, expected)` | Branch prediction hint |

## Project Workflow (recommended)

OmScript supports a project-oriented workflow with `oms.toml` manifests — similar
to Cargo (Rust) or Go modules.

### Create a project

```bash
omsc init my_app          # create ./my_app/ with oms.toml + src/main.om
omsc init my_app ./path   # create project in a specific directory
```

### Build & run

```bash
cd my_app

omsc build                # compile (debug profile)
omsc build --release      # compile (release profile: O3, whole-program, strip)
omsc build --profile bench  # use a custom profile from oms.toml

omsc run                  # build + run (debug)
omsc run --release        # build + run (release)

omsc clean                # remove target/debug/
omsc clean --release      # remove target/release/
```

Build artifacts land in `target/debug/<name>` or `target/release/<name>`.
Incremental builds skip recompilation when source content + manifest + profile
are all unchanged (fingerprinted with FNV-1a).

### Override profile flags on the command line

All codegen flags work on top of the selected profile — they override only the
fields they match, leaving the rest at their profile defaults:

```bash
omsc build --release -fno-egraph          # release, but e-graph disabled
omsc build -O3 -march=znver4              # debug profile at O3 + custom CPU
omsc build --profile bench -fsuperopt-level=3
omsc run   --release -fno-superopt        # fastest release without superopt
```

### oms.toml reference

```toml
[project]
name    = "my_app"
version = "0.1.0"
entry   = "src/main.om"   # relative to project root

[profile.debug]
opt_level     = 0
egraph        = false
superopt      = false
debug_info    = true
whole_program = false

[profile.release]
opt_level      = 3
egraph         = true
superopt       = true
superopt_level = 2       # 0–3
whole_program  = true    # whole-program optimizer
strip          = true
lto            = false
fast_math      = false

[dependencies]
my_lib = "../my_lib"    # local path dependency
```

### Single-file mode (unchanged)

All existing single-file commands continue to work unchanged:

```bash
omsc source.om -o output          # compile directly
omsc run source.om -O3 -fno-egraph
omsc check source.om
omsc emit-ir source.om
```

---

## Building

### Prerequisites
- CMake 3.13+ (3.16+ recommended for precompiled header support)
- C++17 compatible compiler (GCC or Clang)
- LLVM 18+ development libraries
- GCC (for linking)

### Build Instructions
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-18/cmake
make -j$(nproc)
```

The build system automatically uses **ccache**/sccache when available and
enables **precompiled headers** (PCH) on CMake 3.16+ to speed up compilation.
Install ccache for faster incremental rebuilds:
```bash
sudo apt-get install -y ccache   # Debian/Ubuntu
brew install ccache               # macOS
```

## Usage

```bash
# Compile a source file to an executable
./build/omsc source.om -o output

# Compile and immediately run (AOT-compiled, then execute)
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
| `-fstack-protector` | Stack buffer overflow protection | off |
| `-static` | Static linking | off |
| `-s` / `--strip` | Strip debug symbols | off |
| `--pgo-gen=<path>` | Generate PGO instrumentation profile | — |
| `--pgo-use=<path>` | Use PGO profile for optimization | — |
| `-v` / `--verbose` | Show compilation details | off |
| `-q` / `--quiet` | Suppress non-error output | off |
| `--time` | Show timing breakdown | off |

Use `-fno-<flag>` to disable any `-f` flag (e.g. `-fno-vectorize`).

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

### Integer Type-Cast Syntax (v4.1.1)

OmScript 4.1.1 introduces function-call-style type coercions. These look like function calls but are zero-overhead bitwise operations compiled directly to LLVM IR — and they fold entirely at compile time inside `comptime` blocks.

```omscript
fn main() {
    var x:int = 300;

    // Masking casts (zero-extend upper bits)
    var a = u8(x);    // 44  (300 & 0xFF)
    var b = u16(x);   // 300 (fits in 16 bits)
    var c = u32(x);   // 300 (fits in 32 bits)
    var d = u64(x);   // 300 (identity — no-op)

    // Sign-extension casts (truncate then sign-extend)
    var e = i8(200);  // -56  (200 wraps to -56 as signed 8-bit)
    var f = i16(40000); // -25536 (wraps as signed 16-bit)
    var g = i32(x);   // 300  (fits — unchanged)

    // Boolean normalization
    var h = bool(0);  // 0
    var i_ = bool(x); // 1

    println(a);  // 44
    println(e);  // -56
    println(h);  // 0
    return 0;
}
```

### Compile-Time Array Generation (v4.1.1)

`comptime` blocks can now call user-defined functions that return arrays. The result is a compile-time constant with zero runtime overhead:

```omscript
// This function packs a string into 64-bit words, little-endian
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

// The ENTIRE function body is evaluated at compile time.
// No runtime allocation, no function call — just a global constant.
var M:u64[] = comptime { str_to_u64_fast("hello"); };
// Emits: @M = private unnamed_addr constant [2 x i64] [i64 1, i64 478560413544]

fn main() {
    println(len(M));   // 1  (compile-time constant)
    println(M[0]);     // 478560413544 (== 0x6F6C6C6568 == "hello" little-endian)
    return 0;
}
```

Comptime blocks also support implicit return (no `return` keyword required):
```omscript
var BLOCK = comptime { 1 << 6; };    // 64 — implicit return of last expression
var MASK  = comptime { BLOCK - 1; }; // 63
var LOG2  = comptime { log2(BLOCK); }; // 6
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
| `string` | Heap-allocated UTF-8 string (pointer) |
| `int[]` / `T[]` | Typed, dynamically-sized array |
| `dict` | Hash map with string keys |
| `bool` | `true` (1) / `false` (0), stored as `i1`→`i64` |
| `null` | Absence of value |

All types are resolved at compile time. Arrays, strings, maps, and structs are represented as pointer types in LLVM IR — no runtime type tags, no `ptrtoint` round-trips for typed values.

## Testing

```bash
# Unit tests (GTest via CTest; requires libgtest-dev)
cd build && ctest --output-on-failure

# Integration tests (full CLI + language suite)
bash run_tests.sh
```

### Coverage (full production-source instrumentation)

```bash
# Build with coverage instrumentation
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCOVERAGE=ON -DLLVM_DIR=/usr/lib/llvm-18/cmake
cmake --build build --parallel $(nproc)

# Run unit + integration tests to collect counters
cd build && ctest --output-on-failure && cd ..
bash run_tests.sh

# Report coverage for production code
gcovr -r . --filter 'src/' --filter 'runtime/' --exclude 'build/' --print-summary
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
│   └── http_runtime.c          # HTTP request runtime support
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
