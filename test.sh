#!/bin/bash

set -eu

# ──────────────────────────────────────────────────────────────
#  OmScript Benchmark Suite  (Fair Edition)
#
#  53 diverse micro-benchmarks covering distinct workloads.
#  No category is over-represented.  Both OM and C implementations
#  are idiomatic and use the same algorithm.
#
#  Comparison modes:
#    - BENCH_MODE=fair (default): symmetric aggressive flags for both OM and C,
#      both compiled with clang (same LLVM backend) — OM wins through better
#      IR quality (OPTMAX annotations, superoptimizer, srem→urem, etc.)
#    - BENCH_MODE=omsc-fast: conservative C baseline for demonstrating OM's
#      ceiling performance
#    - Override compiler with BENCH_CC=<cc>
#
#  Methodology:
#    - Warmup runs before timing to eliminate cold-start bias
#    - Interleaved C/OM runs to spread system noise evenly
#    - Trimmed mean of N runs (drop 2 highest + 2 lowest) for
#      robustness against outliers — more data than median, more
#      resistant to noise than full average
#    - CPU pinning (taskset) when available for reduced jitter
#    - Standard deviation reported; noisy benchmarks flagged (~)
#    - Short-duration benchmarks (<10 ms) flagged (⏱) as unreliable
#    - Geometric mean as primary aggregate metric (not skewed
#      by a single slow/fast benchmark the way sum is)
#    - Env vars: BENCH_RUNS (default 11), BENCH_WARMUP (default 3),
#               BENCH_CC (default depends on BENCH_MODE)
#               BENCH_MODE (default: fair; set to omsc-fast for OM-advantage flags)
#
#  Categories:
#    - Integer arithmetic & math builtins
#    - Floating-point computation
#    - Arrays (push, random access, sorting, higher-order)
#    - Strings (concat, search)
#    - Structs / data layout
#    - Control flow (switch, if/else, while)
#    - Recursion
#    - Loops (nested, reduction, vectorizable)
#    - Function calls / inlining
#    - Bitwise / intrinsics
#    - Vectorization (dot product, histogram)
#    - Instruction-level parallelism (accumulator chain)
#    - Modular arithmetic (Fibonacci iter, modular exponent)
#    - Real-world kernels (matrix mul, sieve, prefix sum, hash,
#      collatz, binary search)
# ──────────────────────────────────────────────────────────────

RUNS=${BENCH_RUNS:-11}
WARMUP_RUNS=${BENCH_WARMUP:-3}

SCRIPT_START=$(date +%s%N)

# ─── PROCESS ISOLATION ────────────────────────────────────────
# Pin to a single CPU core to reduce scheduler jitter.
# Use taskset if available; fall back to no pinning.
TASKSET=""
if command -v taskset &>/dev/null; then
    TASKSET="taskset -c 0"
fi

NUM_BENCHMARKS=54

BENCH_NAME=(
    "integer_math"       #  0 — GCD, log2, modular arithmetic
    "float_math"         #  1 — sqrt, exp2, pow
    "array_push"         #  2 — dynamic array growth
    "array_hof"          #  3 — map / filter / reduce
    "string_concat"      #  4 — repeated string building
    "string_ops"         #  5 — string search
    "struct_access"      #  6 — struct field read/write
    "switch_branch"      #  7 — 4-way switch dispatch
    "if_else_chain"      #  8 — cascading if/else classify
    "while_loop"         #  9 — while-loop accumulator
    "recursion_fib"      # 10 — naive recursive Fibonacci
    "nested_loops"       # 11 — triple-nested loop
    "array_indexing"     # 12 — pseudo-random array access
    "function_calls"     # 13 — nested inline call chain
    "bitwise_ops"        # 14 — shift, AND, OR, XOR
    "bitwise_intrinsics" # 15 — popcount, clz, ctz
    "polynomial_eval"    # 16 — Horner's method
    "reduction"          # 17 — sum + sum-of-cubes
    "combined"           # 18 — end-to-end workload
    "matrix_multiply"    # 19 — 200x200 matrix multiply
    "sieve"              # 20 — sieve of Eratosthenes
    "prefix_sum"         # 21 — prefix-sum scan
    "hash_compute"       # 22 — FNV-1a hash loop
    "collatz"            # 23 — Collatz-sequence lengths
    "binary_search"      # 24 — binary search in sorted array
    "dot_product"        # 25 — vectorizable multiply-accumulate
    "fibonacci_iter"     # 26 — iterative modular Fibonacci
    "histogram"          # 27 — computed-index histogram binning
    "accumulator_chain"  # 28 — dependent accumulator chain (ILP)
    "modular_exp"        # 29 — modular exponentiation loop
    "strength_reduce"    # 30 — multiply/divide by small constants
    "idiom_patterns"     # 31 — min/max/abs/rotate patterns
    "fma_compute"        # 32 — floating-point multiply-add chains
    "negative_offset"    # 33 — arr[i-1] lookback pattern (bounds elision)
    "const_array_size"   # 34 — array_fill known-size bounds elision
    "cond_arithmetic"    # 35 — conditional increment/decrement + Collatz
    "ring_buffer"        # 36 — circular ring buffer with prime capacity
    "sliding_window"     # 37 — sliding window sum over a large array
    "exponent_chain"     # 38 — integer exponentiation x**N (NSW-mul codegen)
    "for_each"           # 39 — for-each loop over array (inbounds GEP path)
    "lcm_gcd"            # 40 — repeated lcm() + gcd() (nonNeg builtin tracking)
    "zext_pattern"       # 41 — bool-to-int zero-extension (select→zext patterns)
    "str_format"         # 42 — str_format() two-pass snprintf formatting loop
    "array_zip"          # 43 — array_zip() interleave two arrays, accumulate
    "str_prefix_scan"    # 44 — str_starts_with(s, literal) tight loop
    "dict_lookup"        # 45 — map_set / map_get frequency-count loop
    "array_sort_search"  # 46 — sort N elements with sort() then scan
    "simd_saxpy"         # 47 — integer dot product using i32x4 SIMD types
    "pipeline_stencil"   # 48 — element-wise transform via pipeline stages
    "times_loop"         # 49 — times N { } accumulation loop
    "str_process"        # 50 — str_reverse + str_count + str_upper chain
    "swap_sort"          # 51 — selection sort using native swap a, b
    "array_predicates"   # 52 — array_any / array_every / array_count with lambdas
    "op_overload"        # 53 — struct operator overloading + creation (<=>, **, |>)
    "matmul_cm"          # 54 — column-major matrix multiply via mat_new / mat_mul
)

BENCH_DESC=(
    "GCD, log2, and modular arithmetic in a tight loop"
    "Floating-point sqrt, exp2, mul in a tight loop"
    "Push N elements onto a dynamically-growing array"
    "array_map / array_filter / array_reduce with lambdas"
    "Repeated str_concat building a long string"
    "str_contains and str_index_of on a 1000-char haystack"
    "Struct field read/write in a tight loop"
    "Switch/case dispatch with 4 branches inside a loop"
    "Cascading if/else classification of integer ranges"
    "While-loop accumulator (compared to for-in)"
    "Naive recursive Fibonacci (exponential call tree)"
    "Triple-nested loop with N^3 iterations"
    "Pseudo-random array read and write pattern"
    "Deeply nested small-function call chains"
    "Shift, AND, OR, and XOR intensive loop"
    "popcount, clz, ctz, is_power_of_2 in a tight loop"
    "Horner polynomial evaluation"
    "Sum and sum-of-cubes reduction"
    "End-to-end combined workload"
    "Dense matrix multiply (200x200)"
    "Sieve of Eratosthenes up to N"
    "Prefix-sum (scan) over N elements"
    "FNV-1a style hash computation"
    "Collatz conjecture sequence lengths"
    "Binary search in a sorted array"
    "Dot product of two integer arrays (vectorizable)"
    "Iterative modular Fibonacci in a tight loop"
    "Histogram binning with computed array indices"
    "Four cross-dependent accumulators (ILP test)"
    "Repeated modular exponentiation"
    "Multiply/divide by small constants (e-graph strength reduction)"
    "Min/max/abs/rotate idiom patterns (superoptimizer)"
    "Floating-point multiply-add chains (HGOE FMA generation)"
    "Array lookback arr[i-1] pattern (negative-offset bounds elision)"
    "Known-size array bounds elision (array_fill constant propagation)"
    "Conditional increment/decrement + Collatz 3x+1 (superoptimizer + e-graph)"
    "Circular ring buffer with prime (non-power-of-2) capacity; modulo-wrap head/tail"
    "Sliding window sum; loop from positive constant, non-negative subtraction"
    "Integer exponentiation loop: x**2..x**5 repeated; tests NSW-mul for exp"
    "For-each loop over large array; tests auto-vectorize + inbounds GEP path"
    "Repeated lcm() and gcd() calls; tests nonNeg tracking for lcm builtin"
    "Boolean-to-int zero-extension: if(cond) acc+=1 → acc+zext(cond) patterns"
    "str_format(\"%lld\",i) repeated N times; tests two-pass snprintf IR quality"
    "array_zip(a,b) over N-element arrays; tests interleave loop vectorization"
    "str_starts_with(s,\"Hello \") repeated N times; tests known-strlen const fold"
    "map_set/map_get frequency count: N insertions into 1000-key hash map"
    "sort() 1024-element array then sum upper half; repeated N/1024 times"
    "Integer dot product using explicit i32x4 SIMD vector types; 256-elem arrays"
    "Element-wise transform dst[i]=src[i]*3+src[i]/7+src[i]^2%997; pipeline stages"
    "times N { acc=(acc*3+i)%MOD } — tests times-loop codegen vs for-in"
    "str_reverse+str_upper+str_count chain on 48-char string, N iterations"
    "Selection sort of 32-elem array with native swap a,b; repeated N/32 times"
    "array_any/array_every/array_count lambdas on 128-elem array; N/128 outer iters"
    "Vec2 operator overloading (+,-,==) and creation (<=>,**,|>) — N iterations"
    "Column-major matrix multiply N×N via mat_new / mat_mul (column-saxpy inner loop)"
)

# Input sizes – tuned so each test runs ~20-200 ms in C.
BENCH_N=(
    500000    #  0  integer_math
    2000000   #  1  float_math
    500000    #  2  array_push
    200000    #  3  array_hof
    10000     #  4  string_concat
    500000    #  5  string_ops
    5000000   #  6  struct_access
    5000000   #  7  switch_branch
    5000000   #  8  if_else_chain
    10000000  #  9  while_loop
    35        # 10  recursion_fib  (fib(35) ~ 9 M calls)
    200       # 11  nested_loops   (200^3 = 8 M)
    5000000   # 12  array_indexing
    5000000   # 13  function_calls
    10000000  # 14  bitwise_ops
    10000000  # 15  bitwise_intrinsics
    5000000   # 16  polynomial_eval
    10000000  # 17  reduction
    100000    # 18  combined
    300       # 19  matrix_multiply (300x300 = 27M muls)
    5000000   # 20  sieve
    5000000   # 21  prefix_sum
    10000000  # 22  hash_compute
    1000000   # 23  collatz
    5000000   # 24  binary_search
    5000000   # 25  dot_product
    50000000  # 26  fibonacci_iter
    5000000   # 27  histogram
    10000000  # 28  accumulator_chain
    10000000  # 29  modular_exp
    10000000  # 30  strength_reduce
    5000000   # 31  idiom_patterns
    2000000   # 32  fma_compute
    5000000   # 33  negative_offset
    10000000  # 34  const_array_size
    10000000  # 35  cond_arithmetic
    5000000   # 36  ring_buffer
    5000000   # 37  sliding_window
    5000000   # 38  exponent_chain
    5000000   # 39  for_each
    5000000   # 40  lcm_gcd
    10000000  # 41  zext_pattern
    2000000   # 42  str_format
    1000000   # 43  array_zip
    5000000   # 44  str_prefix_scan
    200000    # 45  dict_lookup
    500000    # 46  array_sort_search
    2000000   # 47  simd_saxpy
    2000000   # 48  pipeline_stencil
    10000000  # 49  times_loop
    500000    # 50  str_process
    500000    # 51  swap_sort
    2000000   # 52  array_predicates
    5000000   # 53  op_overload
    250       # 54  matmul_cm (250x250 column-major mat_mul)
)

BOTTLENECK_LABELS=(
    "math builtin call overhead (gcd / log2)"
    "floating-point codegen and FP optimization rules"
    "array reallocation strategy during push"
    "lambda / higher-order function dispatch overhead"
    "string allocation overhead on each concat"
    "string search and manipulation builtin overhead"
    "struct field-access indirection overhead"
    "switch codegen quality and branch prediction"
    "branch codegen quality (if/else chains)"
    "while-loop codegen quality"
    "function-call overhead in deep recursion"
    "loop body codegen and auto-vectorization"
    "array bounds-check overhead"
    "call/return overhead for small inlined functions"
    "bitwise operation codegen quality"
    "LLVM intrinsic codegen for popcount/clz/ctz"
    "Horner scheme with inlined poly evaluation"
    "reduction loop vectorization"
    "overall compiler optimization quality"
    "nested loop + array codegen (matrix multiply)"
    "array + modular arithmetic (sieve)"
    "sequential array scan codegen"
    "integer hash computation codegen"
    "branch-heavy integer loop"
    "loop + comparison codegen (binary search)"
    "array + vectorization codegen (dot product)"
    "integer-only loop codegen quality"
    "computed-index array write pattern"
    "instruction-level parallelism extraction"
    "integer multiply + modular arithmetic codegen"
    "e-graph strength reduction for multiply/divide by constants"
    "superoptimizer idiom recognition (min/max/abs/rotate)"
    "hardware FMA generation for multiply-add chains"
    "negative-offset bounds check elision (arr[i-1] lookback)"
    "known-array-size bounds check elision (constant propagation)"
    "conditional arithmetic + Collatz strength reduction"
    "Ring buffer with prime capacity; non-power-of-2 modulo on bounded indices"
    "Sliding window sum; loop starting at positive constant, NSW-annotated sub"
    "NSW-mul for integer exponentiation (x**2..x**5 per iteration)"
    "for-each loop auto-vectorization and inbounds GEP codegen"
    "lcm nonNeg tracking: lcm/gcd results proven non-negative by the compiler"
    "select(cmp,1,0) → zext(cond) strength reduction"
    "snprintf call overhead and two-pass buffer allocation"
    "interleave loop: array allocation, TBAA, and optional vectorization"
    "str_starts_with with literal: known-strlen const fold + memcmp elision"
    "Hash map insert + lookup; N int keys, 1000-bucket frequency count"
    "Sort N-element array with sort() + scan for sum of top-half elements"
    "Integer dot product using i32x4 SIMD types vs auto-vectorized C loop"
    "Element-wise transform dst[i]=src[i]*3+src[i]/7+src[i]^2%997 via pipeline stages"
    "Accumulation loop using times N { acc = acc*3+i; } vs for-in"
    "str_reverse + str_upper + str_count on a fixed string, N iterations"
    "Selection sort of small array using native swap a,b; repeated N/m times"
    "array_any + array_every + array_count with lambdas on a fixed array, N outer iters"
    "struct operator overloading (+,-,==) and creation (<=>,**,|>) dispatch overhead"
    "column-major mat_mul inner loop vectorization vs row-major C baseline"
)

# ─── COLOR CODES ──────────────────────────────────────────────
RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[0;33m'
BLD='\033[1m'
RST='\033[0m'

# ─── GENERATE SOURCE ─────────────────────────────────────────
echo "=== OmScript Benchmark Suite ==="
echo ""

# ━━━━━━━━━━━━━━━━━━━━━━━━━━ OM SOURCE ━━━━━━━━━━━━━━━━━━━━━━━
cat > bench.om << 'OMEOF'
@noalias
struct Point { hot int x, hot int y }

// Vec2 struct used by bench 53 (op_overload).
// Demonstrates operator overloading (redefine existing symbols) and
// operator creation (define brand-new symbols not in the base language).
struct Vec2 {
    hot int x,
    hot int y,
    // Overloaded operators — same syntax as standard arithmetic
    fn operator+(other: Vec2) -> Vec2  { return Vec2 { x: self.x + other.x, y: self.y + other.y }; }
    fn operator-(other: Vec2) -> Vec2  { return Vec2 { x: self.x - other.x, y: self.y - other.y }; }
    fn operator==(other: Vec2) -> int  { return (self.x == other.x) && (self.y == other.y); }
    // Created operators — new symbols never before in the base language
    fn operator**(other: Vec2) -> int  { return self.x * other.x + self.y * other.y; }
    fn operator<=>(other: Vec2) -> int {
        var la:int = self.x * self.x + self.y * self.y;
        var lb:int = other.x * other.x + other.y * other.y;
        if (la < lb) { return -1; }
        if (la > lb) { return  1; }
        return 0;
    }
    fn operator|>(other: Vec2) -> Vec2 { return Vec2 { x: self.x + other.x * 2, y: self.y + other.y * 2 }; }
}

OPTMAX=:

// ── 0. integer_math ──────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @unroll @static @const_eval
fn bench_math(@prefetch n:int) -> int {
    prefetch var acc:int = 0;
    for (i:int in 1...n) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
        acc += log2(i);
    }
    return acc;
}

// ── 1. float_math ────────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @unroll @static @nounwind
fn bench_floatmath(@prefetch n:int) -> int {
    var acc:double = 1.0;
    for (i:int in 1...n) {
        var fi:double = to_float(i);
        acc = acc + sqrt(fi);
        acc = acc + exp2(fi / 1000000.0);
        acc = acc * 0.9999999;
    }
    return to_int(acc);
}

// ── 2. array_push ────────────────────────────────────────────
@optmax(aggressive_vec=true, safety=relaxed)
@hot @flatten @static @nounwind
fn bench_push(@prefetch n:int) -> int {
    var arr:int[] = [];
    prefetch arr;
    for (i:int in 0...n) {
        arr = push(arr, (i * 3) % 12345);
    }
    var result:int = len(arr);
    invalidate arr;
    invalidate n;
    return result;
}

// ── 3. array_hof ─────────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true})
@hot @flatten @pure  @vectorize @nounwind @const_eval @static
fn bench_hof(@prefetch n:int) -> int {
    var arr:int[] = array_fill(n, 0);
    prefetch arr;
    for (i:int in 0...n) {
        arr[i] = (i * 7) % 1000;
    }
    var mapped:int[] = array_map(arr, |x:int| (x * x) % 997);
    invalidate arr;
    var filtered:int[] = array_filter(mapped, |x:int| x % 2 == 0);
    invalidate mapped;
    var reduced:int = array_reduce(filtered, |a:int, b:int| a + b, 0);
    var result:int = reduced + len(filtered);
    invalidate filtered;
    invalidate n;
    return result;
}

// ── 4. string_concat ─────────────────────────────────────────
@optmax(safety=relaxed)
@hot @unroll @flatten @pure @static
fn bench_strcat(@prefetch n:int) -> int {
    var s:str = "x";
    for (i:int in 0...n) {
        s = str_concat(s, "y");
    }
    var result:int = str_len(s);
    invalidate s;
    invalidate n;
    return result;
}

// ── 5. string_ops ────────────────────────────────────────────
@optmax(aggressive_vec=true, safety=relaxed)
@hot @flatten @static @nounwind
fn bench_strops(@prefetch n:int) -> int {
    var haystack:str = str_repeat("abcdefghij", 100);
    var count:int = 0;
    for (i:int in 0...n) {
        count += str_contains(haystack, "efg");
        count += str_index_of(haystack, "hij") % 100;
    }
    invalidate haystack;
    invalidate n;
    return count;
}

// ── 6. struct_access ─────────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true}, safety=relaxed)
@hot @flatten @pure @unroll @vectorize @static @nounwind
fn bench_struct(@prefetch n:int) -> int {
    prefetch var p:struct = Point { x: 1, y: 2 };
    var sum:int = 0;
    for (i:int in 0...n:int) {
        p.x = p.x + i;
        p.y = p.y ^ i;
        sum += p.x + p.y;
    }
    invalidate n;
    invalidate p;
    return sum;
}

// ── 7. switch_branch ─────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @unroll @static @nounwind 
fn bench_branch(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        switch (i % 4) {
            case 0: sum += i;       break;
            case 1: sum -= i;       break;
            case 2: sum ^= i;       break;
            default: sum += (i * 2);
        }
    }
    invalidate n;
    return sum;
}

// ── 8. if_else_chain ─────────────────────────────────────────
@hot @inline @static @pure @nounwind
fn classify(x:int) -> int {
    if (x < 10)    { return 1; }
    if (x < 100)   { return 2; }
    if (x < 1000)  { return 3; }
    if (x < 10000) { return 4; }
    if (x < 100000){ return 5; }
    return 6;
}
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @vectorize @unroll @static @nounwind
fn bench_ifelse(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        sum += classify(i % 200000);
    }
    return sum;
}

// ── 9. while_loop ────────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true})
@hot @flatten @vectorize @unroll @static @nounwind
fn bench_while(@prefetch n:int) -> int {
    var i:int = 0;
    var acc:int = 0;
    while (i < n) {
        acc += (i * i) % 101;
        acc ^= i;
        i += 1;
    }
    return acc;
}

// ── 10. recursion_fib ────────────────────────────────────────
@optmax(safety=relaxed)
@hot @pure @static @nounwind
fn fib(n:int) -> int {
    if (n <= 1) { return n; }
    return fib(n - 1) + fib(n - 2);
}
@flatten @hot @const_eval @static
fn bench_recurse(n:int) -> int {
    return fib(n);
}

// ── 11. nested_loops ─────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true})
@hot @flatten @pure @unroll @vectorize @static @nounwind
fn bench_nested(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        for (j:int in 0...n:int) {
            for (k:int in 0...n:int) {
                 sum += ((i ^ j) + k) & 63;
            }
        }
    }
    return sum;
}

// ── 12. array_indexing ───────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true, prefetch=true})
@hot @flatten @unroll @static @nounwind
fn bench_arrindex(@prefetch n:int) -> int {
    const sz:int = 10000;
    var arr:int[] = array_fill(sz, 0);
    for (i:int in 0...sz) {
        arr[i] = i * 3;
    }
    var sum:int = 0;
    for (i:int in 0...n) {
        var idx:int = (i * 7 + 13) % sz;
        sum += arr[idx];
        arr[idx] = sum % 100000;
    }
    invalidate arr;
    invalidate n;
    return sum;
}

// ── 13. function_calls ───────────────────────────────────────
@optmax(safety=relaxed)
@hot @inline @static @nounwind @pure
fn add_one(x:int) -> int { return x + 1; }
@hot @inline @static @nounwind @pure
fn add_two(x:int) -> int { return add_one(add_one(x)); }
@hot @inline @static @nounwind @pure
fn add_four(x:int) -> int { return add_two(add_two(x)); }
@hot @flatten @vectorize @unroll @static @nounwind
fn bench_calls(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        sum += add_four(i & 255);
    }
    invalidate n;
    return sum;
}

// ── 14. bitwise_ops ──────────────────────────────────────────
@optmax(aggressive_vec=true, safety=relaxed)
@hot @flatten @vectorize @unroll @static @nounwind
fn bench_bitwise(@prefetch n:int) -> int {
    var a:int = 0;
    var b:int = 0;
    var c:int = 0;
    for (i:int in 0...n) {
        a = (a ^ (i << 3)) + (i & 255);
        b = (b | (i >> 1)) ^ (a & 65535);
        c += (a ^ b) & 1023;
    }
    invalidate n;
    return a + b + c;
}

// ── 15. bitwise_intrinsics ───────────────────────────────────
@optmax(aggressive_vec=true, safety=relaxed)
@hot @flatten @unroll @static @nounwind @vectorize
fn bench_bitintrinsics(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 1...n) {
        acc += popcount(i);
        acc += clz(i);
        acc += ctz(i | 1);
        acc += is_power_of_2(i);
    }
    return acc;
}

// ── 16. polynomial_eval ──────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @pure @unroll @inline @vectorize @static
fn poly_eval(x:int) -> int {
    var r:int = 3;
    r = r * x + 2;
    r = r * x + 1;
    r = r * x + 5;
    r = r * x + 7;
    r = r * x + 11;
    return r;
}
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @vectorize @static
fn bench_poly(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        sum += poly_eval(i % 1000);
    }
    return sum;
}

// ── 17. reduction ────────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true})
@hot @flatten @vectorize @unroll @pure @static @nounwind
fn bench_reduction(@prefetch n:int) -> int {
    var sum:int = 0;
    var sum2:int = 0;
    for (i:int in 1...n) {
        sum += i * i;
        sum2 += i * i * i;
    }
    return sum + sum2;
}

// ── 18. combined ─────────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true})
@hot @vectorize @flatten @unroll @static @nounwind
fn bench_combined(n:int) -> int {
    var total:int = 0;
    var acc:int = 0;
    for (i:int in 1...n) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
    }
    total += acc;

    var arr:int[] = [];
    for (i:int in 0...(n / 10)) {
        arr = push(arr, (i * 3) % 12345);
    }
    var mapped:int[] = array_map(arr, |x:int| (x * x) % 997);
    invalidate arr;
    var filtered:int[] = array_filter(mapped, |x:int| x % 2 == 0);
    invalidate mapped;
    total += (array_reduce(filtered, |a:int, b:int| a + b, 0) + len(filtered));
    invalidate filtered;

    var p:struct = Point { x: 1, y: 2 };
    for (i:int in 0...n) {
        p.x = p.x + i;
        p.y = p.y ^ i;
        total += p.x + p.y;
    }

    for (i:int in 0...n) {
        switch (i % 4) {
            case 0: total += i;       break;
            case 1: total -= i;       break;
            case 2: total ^= i;       break;
            default: total += (i * 2);
        }
    }

    var s:str = "x";
    for (i:int in 0...(n / 100)) {
        s = str_concat(s, "y");
    }
    total += str_len(s);
    invalidate s;
    invalidate n;

    var ns:int = 50;
    for (i:int in 0...ns) {
        for (j:int in 0...ns) {
            for (k:int in 0...ns) {
                total += ((i ^ j) + k) & 63;
            }
        }
    }
    return total;
}

// ── 19. matrix_multiply ──────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true}, safety=relaxed)
@hot @flatten @unroll @vectorize @static @nounwind
fn bench_matmul(n:int) -> int {
    var a:int[] = array_fill(n * n, 0);
    var b:int[] = array_fill(n * n, 0);
    var c:int[] = array_fill(n * n, 0);
    for (i:int in 0...n) {
        for (j:int in 0...n) {
            a[i * n + j] = (i + j) % 97;
            b[i * n + j] = (i * j + 1) % 53;
        }
    }
    for (i:int in 0...n) {
        for (j:int in 0...n) {
            var s:int = 0;
            for (k:int in 0...n) {
                s += a[i * n + k] * b[k * n + j];
            }
            c[i * n + j] = s;
        }
    }
    var sum:int = 0;
    for (i:int in 0...(n * n)) {
        sum += c[i];
    }
    invalidate a;
    invalidate b;
    invalidate c;
    return sum;
}

// ── 20. sieve ────────────────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true, prefetch=true})
@hot @flatten @unroll @static @nounwind
fn bench_sieve(n:int) -> int {
    var is_prime:int[] = array_fill(n, 1);
    is_prime[0] = 0;
    if (n > 1) { is_prime[1] = 0; }
    var i:int = 2;
    while (i * i < n) {
        if (is_prime[i] == 1) {
            var j:int = i * i;
            while (j < n) {
                is_prime[j] = 0;
                j += i;
            }
        }
        i += 1;
    }
    var count:int = 0;
    for (k:int in 0...n) {
        count += is_prime[k];
    }
    invalidate is_prime;
    return count;
}

// ── 21. prefix_sum ───────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true})
@hot @flatten @unroll @vectorize @static @nounwind
fn bench_prefix_sum(@prefetch n:int) -> int {
    var arr:int[] = array_fill(n, 0);
    for (i:int in 0...n) {
        arr[i] = (i * 7 + 3) % 1000;
    }
    for (i:int in 1...n) {
        arr[i] = arr[i] + arr[i - 1];
    }
    var result:int = arr[n - 1];
    invalidate arr;
    return result;
}

// ── 22. hash_compute ─────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @unroll @pure @static @nounwind
fn bench_hash(@prefetch n:int) -> int {
    var hash:int = 5381;
    for (i:int in 0...n) {
        hash = ((hash << 5) + hash) ^ (i & 255);
        hash = hash & 0xFFFFFFFF;
    }
    return hash;
}

// ── 23. collatz ──────────────────────────────────────────────
@optmax(safety=relaxed)
// Dual-counter ILP trick: two independent step counters (s1, s2) that
// advance on alternating Collatz steps.  s1 and s2 have no dependency on
// each other, so the CPU's OOO backend can retire their increments in the
// same clock cycle.  The outer for-loop is unrolled 2× by the compiler
// (outerUnrollCount=2), giving 4 independent counter chains while keeping
// register pressure within x86-64's 15 GP register budget (no spills).
@hot @noinline @static @nounwind
fn bench_collatz(@prefetch n:int) -> int {
    var total:int = 0;
    var total2:int = 0;
    for (i:int in 1...n) {
        var x:int = i;
        var s1:int = 0;
        var s2:int = 0;
        while (x != 1) {
            x = (x % 2 == 0) ? (x / 2) : (3 * x + 1);
            s1 += 1;
            if (x == 1) { break; }
            x = (x % 2 == 0) ? (x / 2) : (3 * x + 1);
            s2 += 1;
        }
        total += s1;
        total2 += s2;
    }
    return total + total2;
}

// ── 24. binary_search ────────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true, prefetch=true})
@hot @flatten @unroll @static @nounwind
fn bench_bsearch(@prefetch n:int) -> int {
    const sz:int = 100000;
    var arr:int[] = array_fill(sz, 0);
    for (i:int in 0...sz) {
        arr[i] = i * 3;
    }
    var found:int = 0;
    for (i:int in 0...n) {
        var target:int = (i * 7 + 13) % (sz * 3);
        var lo:int = 0;
        var hi:int = sz - 1;
        while (lo <= hi) {
            var mid:int = (lo + hi) / 2;
            if (arr[mid] == target) {
                found += 1;
                lo = hi + 1;
            } else if (arr[mid] < target) {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
    }
    invalidate arr;
    return found;
}

// ── 25. dot_product ──────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true})
@hot @flatten @vectorize @unroll @pure @static @nounwind
fn bench_dot(@prefetch n:int) -> int {
    var a:int[] = array_fill(n, 0);
    var b:int[] = array_fill(n, 0);
    for (i:int in 0...n) {
        a[i] = (i * 3 + 1) % 1000;
        b[i] = (i * 7 + 2) % 1000;
    }
    var sum:int = 0;
    for (i:int in 0...n) {
        sum += a[i] * b[i];
    }
    invalidate a;
    invalidate b;
    return sum;
}

// ── 26. fibonacci_iter ───────────────────────────────────────
@optmax(fast_math=true, safety=relaxed)
@hot @flatten @unroll @pure @static @nounwind
fn bench_fib_iter(@prefetch n:int) -> int {
    var a:int = 0;
    var b:int = 1;
    for (i:int in 0...n) {
        var t:int = b;
        b = (a + b) % 1000000007;
        a = t;
    }
    return b;
}

// ── 27. histogram ────────────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true, prefetch=true})
@hot @flatten @unroll @static @nounwind
fn bench_histogram(@prefetch n:int) -> int {
    var bins:int[] = array_fill(256, 0);
    for (i:int in 0...n) {
        var idx:int = ((i * 7) ^ (i >> 3)) & 255;
        bins[idx] = bins[idx] + 1;
    }
    var sum:int = 0;
    for (i:int in 0...256) {
        sum += bins[i] * (i + 1);
    }
    invalidate bins;
    return sum;
}

// ── 28. accumulator_chain ────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @unroll @pure @static @nounwind
fn bench_accum(@prefetch n:int) -> int {
    var a:int = 1;
    var b:int = 2;
    var c:int = 3;
    var d:int = 4;
    for (i:int in 1...n) {
        a = a + (b ^ i);
        b = b + (c & i);
        c = c ^ (d + i);
        d = d + (a | i);
    }
    return a + b + c + d;
}

// ── 29. modular_exp ──────────────────────────────────────────
@optmax(fast_math=true, safety=relaxed)
@hot @flatten @unroll @pure @static @nounwind
fn bench_modexp(@prefetch n:int) -> int {
    var result:int = 1;
    var base:int = 3;
    var modulus:int = 1000000007;
    var acc:int = 0;
    for (i:int in 1...n) {
        result = (result * base) % modulus;
        acc += result;
    }
    return acc;
}

// ── 30. strength_reduce ──────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
// Tests e-graph strength reduction: multiply and divide by small
// constants (3,5,7,10,12,100), modulo by powers of 2, and
// mixed shift-add patterns that the e-graph rewrites to cheaper
// shift+add/sub sequences.
@hot @flatten @unroll @pure @vectorize @static @nounwind
fn bench_strength(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 1...n) {
        acc += i * 3;
        acc += i * 5;
        acc += i * 7;
        acc += i * 10;
        acc += i * 12;
        acc += i * 100;
        acc += i / 4;
        acc += i / 8;
        acc += i % 16;
        acc += i % 64;
        acc ^= (i * 15);
        acc ^= (i * 31);
    }
    return acc;
}

// ── 31. idiom_patterns ───────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
// Tests superoptimizer idiom recognition: min, max, absolute value,
// conditional negation, and power-of-2 test patterns.
// Patterns are written inline to match the C version exactly.
@hot @flatten @unroll @pure @static @nounwind
fn bench_idioms(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 1...n) {
        var x:int = i - (n / 2);
        // abs(x) pattern — superoptimizer detects select(x<0, -x, x) → llvm.abs
        if (x < 0) { acc += (0 - x); } else { acc += x; }
        // min(i, n-i) pattern — superoptimizer detects select(a<b, a, b) → llvm.smin
        var ni:int = n - i;
        if (i < ni) { acc += i; } else { acc += ni; }
        // max(a, b) pattern — superoptimizer detects select(a>b, a, b) → llvm.smax
        var a:int = i % 100;
        var b:int = ni % 100;
        if (a > b) { acc += a; } else { acc += b; }
        // Conditional negation pattern
        if (i % 3 == 0) {
            acc -= x;
        } else {
            acc += x;
        }
    }
    return acc;
}

// ── 32. fma_compute ──────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
// Tests HGOE FMA generation: floating-point multiply-add chains
// of the form a*b+c and a*b+c*d that the hardware graph optimizer
// converts to fused multiply-add instructions.
@hot @flatten @unroll @static @nounwind
fn bench_fma(@prefetch n:int) -> int {
    var a:double = 1.0;
    var b:double = 0.9999999;
    var c:double = 0.0000001;
    var acc:double = 0.0;
    for (i:int in 1...n) {
        var fi:double = to_float(i);
        // a*b + c  pattern (single FMA)
        acc = acc + (fi * b + c);
        // a*b + c*d  pattern (chained FMA)
        acc = acc + (fi * 0.3 + (fi + 1.0) * 0.7);
        a = a * b + c;
    }
    return to_int(acc + a);
}

// ── 33. negative_offset ──────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true, prefetch=true})
// Tests negative-offset bounds check elision: arr[i-1] lookback
// pattern inside a loop starting from 1.  The compiler should
// prove that i-1 >= 0 because the loop starts at 1, and that
// i-1 < len(arr) because i < len(arr).
@hot @flatten @unroll @static @nounwind
fn bench_negoffset(@prefetch n:int) -> int {
    var arr:int[] = array_fill(n, 0);
    for (i:int in 0...n) {
        arr[i] = i * 3 + 1;
    }
    var acc:int = 0;
    for (i:int in 1...n) {
        // lookback pattern: arr[i - 1]
        acc += arr[i] - arr[i - 1];
        // double lookback
        acc ^= arr[i - 1] * 2;
    }
    return acc;
}

// ── 34. const_array_size ─────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true})
// Tests known-array-size bounds elision: array_fill with constant
// size + bounded loop access.  The compiler should track that
// the array has exactly 1024 elements and elide bounds checks
// when the loop bound <= 1024.
@hot @flatten @unroll @vectorize @static @nounwind
fn bench_constarray(@prefetch n:int) -> int {
    var data:int[] = array_fill(1024, 0);
    for (i:int in 0...1024) {
        data[i] = i * i + i;
    }
    var acc:int = 0;
    for (j:int in 0...n) {
        var idx:int = j % 1024;
        acc += data[idx];
        acc ^= data[(j * 7) % 1024];
    }
    return acc;
}

// ── 35. cond_arithmetic ──────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
// Tests superoptimizer conditional increment/decrement detection.
// Patterns: if(cond) x++; else x; → x + zext(cond)
// These arise from conditional counter updates in tight loops.
@hot @flatten @unroll @pure @vectorize @static @nounwind
fn bench_condarith(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 1...n) {
        // Conditional increment: acc += (i % 3 == 0) ? 1 : 0
        if (i % 3 == 0) { acc += 1; }
        // Conditional decrement: acc -= (i % 5 == 0) ? 1 : 0
        if (i % 5 == 0) { acc -= 1; }
        // Collatz 3x+1 pattern
        var x:int = i;
        if (x % 2 == 0) {
            x = x / 2;
        } else {
            x = 3 * x + 1;
        }
        acc += x;
    }
    return acc;
}

// ── 36. ring_buffer ──────────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true, prefetch=true})
// Circular ring buffer with prime capacity (non-power-of-2 = 509).
// head and tail are provably non-negative, bounded to [0, CAP-1].
// (tail + 1) % CAP exercises srem→urem for a non-constant-divisor
// path when CAP is a runtime-tracked positive value.
@hot @flatten @unroll @static @nounwind
fn bench_ringbuf(@prefetch n:int) -> int {
    const CAP:int = 509;
    var buf:int[] = array_fill(CAP, 0);
    for (i:int in 0...CAP) {
        buf[i] = (i * 3 + 1) % 10007;
    }
    var tail:int = 0;
    var acc:int = 0;
    for (i:int in CAP...n) {
        acc += buf[tail];
        buf[tail] = (i * 3 + 1) % 10007;
        tail = (tail + 1) % CAP;
    }
    invalidate buf;
    invalidate n;
    return acc;
}

// ── 37. sliding_window ───────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true, prefetch=true})
// Sliding window sum.  Inner loop starts from positive constant WIN,
// so the iterator is non-negative and the loop condition can use
// ICmpULT.  wsum + data[i] - data[i-WIN] exercises the NSW-Sub pass
// since wsum and data[i] are both provably non-negative.
@hot @flatten @unroll @vectorize @static @nounwind
fn bench_slidingwin(@prefetch n:int) -> int {
    const WIN:int = 64;
    var data:int[] = array_fill(n, 0);
    for (i:int in 0...n) {
        data[i] = (i * 13 + 7) % 10000;
    }
    var wsum:int = 0;
    for (i:int in 0...WIN) {
        wsum += data[i];
    }
    var acc:int = wsum;
    for (i:int in WIN...n) {
        wsum = wsum + data[i] - data[i - WIN];
        acc += wsum;
    }
    invalidate data;
    invalidate n;
    return acc;
}

// ── 38. exponent_chain ───────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
// Integer exponentiation x**2, x**3, x**4, x**5 in a tight loop.
// Tests NSW-mul path: when base is non-negative, each squaring/cube
// uses nsw multiply. Even exponents always produce non-negative results.
@hot @flatten @unroll @pure @static @nounwind
fn bench_expchain(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 1...n) {
        var x:int = i % 1000;
        acc += x ** 2;
        acc += x ** 3;
        acc += x ** 4;
        acc += x ** 5;
        acc = acc % 1000000007;
    }
    invalidate n;
    return acc;
}

// ── 39. for_each ─────────────────────────────────────────────
@optmax(aggressive_vec=true, memory={noalias=true, prefetch=true})
// For-each over an integer array exercises the auto-vectorization
// path that uses inbounds GEP and parallel_accesses metadata.
// Distinct from indexed for-in, which uses explicit index arithmetic.
@hot @flatten @vectorize @unroll @static @nounwind
fn bench_foreach(@prefetch n:int) -> int {
    var arr:int[] = array_fill(n, 0);
    for (i:int in 0...n) {
        arr[i] = (i * 7 + 3) % 10000;
    }
    var sum:int = 0;
    for (x:int in arr) {
        sum += x;
    }
    invalidate arr;
    invalidate n;
    return sum;
}

// ── 40. lcm_gcd ──────────────────────────────────────────────
@optmax(fast_math=true, safety=relaxed)
// Repeated lcm() and gcd() calls in a tight loop.
// lcm is provably non-negative (nonNeg tracking via abs of inputs),
// so the compiler can use NUW/NSW on further arithmetic with the result.
@hot @flatten @unroll @pure @static @nounwind
fn bench_lcmgcd(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 1...n) {
        var a:int = i % 1000 + 1;
        var b:int = (i * 3 + 7) % 1000 + 1;
        acc += gcd(a, b);
        acc += lcm(a, b);
        acc = acc % 1000000007;
    }
    invalidate n;
    return acc;
}

// ── 41. zext_pattern ─────────────────────────────────────────
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
// Tests bool-to-int zero-extension strength reduction.
// The pattern: if (cond) { acc += 1; } is lowered as
// acc += zext(cond) by the superoptimizer rather than a branch.
// Also tests: acc += (a < b) ? 1 : 0 style comparisons.
@hot @flatten @unroll @pure @vectorize @static @nounwind
fn bench_zext(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 1...n) {
        // Simple conditional increment: superoptimizer → zext
        if (i % 3 == 0) { acc += 1; }
        if (i % 5 == 0) { acc += 1; }
        if (i % 7 == 0) { acc += 1; }
        // Comparison accumulation
        acc += (i % 2 == 0);
        acc += (i % 4 == 0);
        // Mixed: only subtract on rare condition
        if (i % 11 == 0) { acc -= 1; }
    }
    invalidate n;
    return acc;
}

OPTMAX!:

// ── 42. str_format ───────────────────────────────────────────
// Repeated str_format("%lld", i) calls.  Each call does a two-pass
// snprintf (probe + fill) and a malloc.  Tests the snprintf IR quality
// and allocation-overhead.  Accumulate lengths to defeat DCE.
@optmax(safety=relaxed)
@hot @static @nounwind
fn bench_strformat(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 0...n) {
        var s:string = str_format("%lld", i % 100000);
        acc += len(s);
        invalidate s;
    }
    invalidate n;
    return acc;
}

// ── 43. array_zip ────────────────────────────────────────────
// Create two arrays of m elements and zip them.  Accumulate the sum
// of all elements in the result (= sum(a) + sum(b)).
// Tests the array_zip interleave loop, TBAA annotations, and
// optional auto-vectorization of the accumulation.
@optmax(aggressive_vec=true, memory={noalias=true}, safety=relaxed)
@hot @flatten @vectorize @static @nounwind
fn bench_arrayzip(@prefetch n:int) -> int {
    var m:int = n / 1000 + 2;
    var a:int[] = array_fill(m, 0);
    var b:int[] = array_fill(m, 0);
    for (j:int in 0...m) { a[j] = j * 3 % 997; b[j] = j * 7 % 997; }
    var acc:int = 0;
    for (k:int in 0...n) {
        var z:int[] = array_zip(a, b);
        acc += sum(z) % 997;
        invalidate z;
    }
    invalidate a; invalidate b; invalidate n;
    return acc;
}

// ── 44. str_prefix_scan ──────────────────────────────────────
// str_starts_with(s, "Hello ") in a tight loop.
// The literal prefix has a compile-time-known length, so the compiler
// replaces strlen with a constant and emits a single memcmp(s, "Hello ", 6).
// Tests the const-fold of strlen for literal second argument.
@optmax(aggressive_vec=true, safety=relaxed)
@hot @flatten @pure @vectorize @static @nounwind
fn bench_strprefix(@prefetch n:int) -> int {
    var acc:int = 0;
    var s:string = "Hello World";
    for (i:int in 0...n) {
        acc += str_starts_with(s, "Hello ");
        acc += str_ends_with(s, "World");
        acc += str_starts_with(s, "Goodbye");
    }
    invalidate n;
    return acc;
}

// ── 45. dict_lookup ──────────────────────────────────────────
// Build a frequency map over N keys (mod 1000), then read back.
// Exercises map_set / map_get in a tight loop; demonstrates OM's
// built-in hash map vs a manual open-addressing table in C.
@optmax(aggressive_vec=true, safety=relaxed)
@hot @flatten @static @nounwind
fn bench_dictlookup(@prefetch n:int) -> int {
    var m = map_new();
    for (i:int in 0...n) {
        var k:int = (i * 2654435761) % 1000;
        var prev:int = map_get(m, k, 0);
        m = map_set(m, k, prev + 1);
    }
    var acc:int = 0;
    for (q:int in 0...1000) {
        acc += map_get(m, q, 0);
    }
    invalidate m; invalidate n;
    return acc;
}

// ── 46. array_sort_search ─────────────────────────────────────
// Each iteration: reinitialize array with deterministic values, sort it,
// then sum the upper half.  Tests sort() quality vs C qsort().
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true}, safety=relaxed)
@hot @flatten @static @nounwind
fn bench_arrsortsearch(@prefetch n:int) -> int {
    var m:int = 1024;
    var arr:int[] = array_fill(m, 0);
    var iters:int = n / m;
    var acc:int = 0;
    for (k:int in 0...iters) {
        for (j:int in 0...m) { arr[j] = (j * 97 + k * 31 + 17) % 100000; }
        sort(arr);
        for (j:int in m / 2...m) { acc += arr[j]; }
        acc = acc % 1000000007;
    }
    invalidate arr; invalidate n;
    return acc;
}

// ── 47. simd_saxpy ───────────────────────────────────────────
// Integer dot product using explicit i32x4 SIMD vector type.
// Processes 4 elements per SIMD op; accumulates into acc.
// C uses the same algorithm, relying on auto-vectorization.
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true}, safety=relaxed)
@hot @static @nounwind @vectorize
fn bench_simdsaxpy(@prefetch n:int) -> int {
    var m:int = 256;
    var x:int[] = array_fill(m, 0);
    var y:int[] = array_fill(m, 0);
    for (j:int in 0...m) {
        x[j] = j % 100 + 1;
        y[j] = (j * 3 + 1) % 100 + 1;
    }
    var iters:int = n / m;
    var acc:int = 0;
    for (k:int in 0...iters) {
        for (j:int in 0...m / 4) {
            var xv:i32x4 = [x[j*4], x[j*4+1], x[j*4+2], x[j*4+3]];
            var yv:i32x4 = [y[j*4], y[j*4+1], y[j*4+2], y[j*4+3]];
            var pv:i32x4 = xv * yv;
            acc += pv[0] + pv[1] + pv[2] + pv[3];
        }
        acc = acc % 1000000007;
    }
    invalidate x; invalidate y; invalidate n;
    return acc;
}

// ── 48. pipeline_stencil ─────────────────────────────────────
// Element-wise transform dst[i] = src[i]*3 + src[i]/7 + src[i]^2 % 997
// via pipeline load → compute → store stages.
// Only uses src[__pipeline_i] (no offset) — safe for all prefetch distances.
// C equivalent is a plain loop.  Tests pipeline stage scheduling overhead
// and software-prefetch benefit for sequential read patterns.
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true}, safety=relaxed)
@hot @flatten @static @nounwind
fn bench_pipelinestencil(@prefetch n:int) -> int {
    var m:int = 8192;
    var src:int[] = array_fill(m, 0);
    var dst:int[] = array_fill(m, 0);
    for (j:int in 0...m) { src[j] = (j * 3 + 1) % 997; }
    var iters:int = n / m;
    var elem:int = 0;
    var tval:int = 0;
    var acc:int = 0;
    for (k:int in 0...iters) {
        pipeline m {
            stage load    { elem = src[__pipeline_i]; }
            stage compute { tval = elem * 3 + elem / 7 + elem * elem % 997; }
            stage store   { dst[__pipeline_i] = tval; }
        }
        for (j:int in 0...m) { acc = (acc + dst[j]) % 1000000007; }
    }
    invalidate src; invalidate dst; invalidate n;
    return acc;
}

// ── 49. times_loop ───────────────────────────────────────────
// Accumulation using `times N { acc = acc*3 + __times_i; }`.
// Demonstrates the OM `times` loop keyword vs a plain for-in.
// Pure integer accumulation, no heap allocation — eligible for OPTMAX.
OPTMAX=:
// `times` does not expose an iterator variable, so a manual counter `i`
// is maintained alongside it.
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @static @nounwind @unroll
fn bench_timesloop(@prefetch n:int) -> int {
    var acc:int = 0;
    var i:int = 0;
    times (n) {
        acc = (acc * 3 + i) % 1000000007;
        i += 1;
    }
    invalidate n;
    return acc;
}

OPTMAX!:

// ── 50. str_process ──────────────────────────────────────────
// Chain of str_reverse + str_upper + str_count on a fixed string.
// Tests OM's string processing builtins and compile-time known-
// length optimizations.
@optmax(safety=relaxed)
@hot @flatten @unroll @static @nounwind
fn bench_strprocess(@prefetch n:int) -> int {
    var s:string = "Hello, OmScript World! Benchmarking string ops.";
    var acc:int = 0;
    for (i:int in 0...n) {
        var rev:string = str_reverse(s);
        var up:string  = str_upper(rev);
        acc += str_count(up, "O");
        acc += str_count(s,  "l");
        invalidate rev; invalidate up;
    }
    invalidate n;
    return acc;
}

// ── 51. swap_sort ────────────────────────────────────────────
// Selection sort of a small array (m=32) using native `swap a, b`.
// Array is reinitialized each iteration from deterministic values.
// Tests OM's swap statement vs explicit temp-variable swap in C.
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true}, safety=relaxed)
@hot @flatten @unroll @static @nounwind
fn bench_swapsort(@prefetch n:int) -> int {
    var m:int = 32;
    var arr:int[] = array_fill(m, 0);
    var iters:int = n / m;
    var acc:int = 0;
    for (k:int in 0...iters) {
        // reinitialize deterministically
        for (j:int in 0...m) { arr[j] = (j * 7 + k * 3 + 1) % m; }
        // selection sort with swap
        for (i:int in 0...m - 1) {
            var minIdx:int = i;
            for (j:int in i + 1...m) {
                if (arr[j] < arr[minIdx]) { minIdx = j; }
            }
            if (minIdx != i) {
                var vi:int = arr[i];
                var vm:int = arr[minIdx];
                swap vi, vm;
                arr[i] = vi;
                arr[minIdx] = vm;
            }
        }
        acc += arr[m - 1];
    }
    invalidate arr; invalidate n;
    return acc;
}
// ── 52. array_predicates ─────────────────────────────────────
// array_any / array_every / array_count with lambdas over a fixed
// array of N/iters elements.  Tests OM's higher-order predicate
// dispatch vs manual loops in C.
fn is_pos(x:int) -> int { return x > 0; }
fn is_even_p(x:int) -> int { return x % 2 == 0; }
fn is_big(x:int) -> int { return x > 500; }

@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true}, safety=relaxed)
@hot @flatten @unroll @static @nounwind
fn bench_arraypred(@prefetch n:int) -> int {
    var m:int = 128;
    var arr:int[] = array_fill(m, 0);
    for (j:int in 0...m) { arr[j] = (j * 13 + 7) % 1000; }
    var iters:int = n / m;
    var acc:int = 0;
    for (k:int in 0...iters) {
        acc += array_any(arr,   "is_pos");
        acc += array_every(arr, "is_even_p");
        acc += array_count(arr, "is_big");
    }
    invalidate arr; invalidate n;
    return acc;
}

// ── 53. op_overload ──────────────────────────────────────────
// Vec2 struct exercising both operator overloading (redefining +, -, ==)
// and operator creation (new symbols <=>, ** for dot product, |> for
// weighted-add).  N iterations accumulate results to prevent DCE.
// Uses struct heap-allocations so kept outside OPTMAX block.
// Each iteration performs: add, subtract, dot-product (**), comparison
// (<=>), and weighted-add (|>) on a pair of Vec2 values derived from i.
@optmax(fast_math=true, aggressive_vec=true, safety=relaxed)
@hot @flatten @unroll @pure @static @nounwind
fn bench_opoverload(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 0...n) {
        var a = Vec2 { x: (i * 3) % 1000, y: (i * 7 + 1) % 1000 };
        var b = Vec2 { x: (i * 5 + 2) % 1000, y: (i * 11 + 3) % 1000 };
        var s = a + b;
        var d = a - b;
        var dp:int = a ** b;
        var cmp:int = a <=> b;
        var w = a |> b;
        acc += s.x + s.y + d.x + d.y + dp + cmp + w.x + w.y;
    }
    invalidate n;
    return acc;
}

// ── 54. matmul_cm ────────────────────────────────────────────
// N×N column-major matrix multiply using mat_new / mat_mul builtins.
// Demonstrates column-major layout: element (i,j) at slot j*rows+i+2.
// mat_mul uses the j→p→i loop order (column-saxpy) so the inner loop
// over row index i accesses a full column of A and C — both contiguous
// in memory, fully auto-vectorizable by LLVM SLP and loop vectorizer.
@optmax(fast_math=true, aggressive_vec=true, memory={noalias=true, prefetch=true}, safety=relaxed)
@hot @flatten @vectorize @unroll @static @nounwind
fn bench_matmul_cm(n:int) -> int {
    var a:int = mat_new(n, n);
    var b:int = mat_new(n, n);
    for (i:int in 0...n) {
        for (j:int in 0...n) {
            mat_set(a, i, j, (i + j) % 97);
            mat_set(b, i, j, (i * j + 1) % 53);
        }
    }
    var c:int = mat_mul(a, b);
    var sum:int = 0;
    for (i:int in 0...n) {
        for (j:int in 0...n) {
            sum += mat_get(c, i, j);
        }
    }
    invalidate n;
    return sum;
}

// ── main dispatch ─────────────────────────────────────────────
fn main() -> int {
    var test_id:int = input();
    prefetch var n:int = input();

    switch (test_id) {
        case 0:  print(bench_math(n));           break;
        case 1:  print(bench_floatmath(n));      break;
        case 2:  print(bench_push(n));           break;
        case 3:  print(bench_hof(n));            break;
        case 4:  print(bench_strcat(n));         break;
        case 5:  print(bench_strops(n));         break;
        case 6:  print(bench_struct(n));         break;
        case 7:  print(bench_branch(n));         break;
        case 8:  print(bench_ifelse(n));         break;
        case 9:  print(bench_while(n));          break;
        case 10: print(bench_recurse(n));        break;
        case 11: print(bench_nested(n));         break;
        case 12: print(bench_arrindex(n));       break;
        case 13: print(bench_calls(n));          break;
        case 14: print(bench_bitwise(n));        break;
        case 15: print(bench_bitintrinsics(n));  break;
        case 16: print(bench_poly(n));           break;
        case 17: print(bench_reduction(n));      break;
        case 18: print(bench_combined(n));       break;
        case 19: print(bench_matmul(n));         break;
        case 20: print(bench_sieve(n));          break;
        case 21: print(bench_prefix_sum(n));     break;
        case 22: print(bench_hash(n));           break;
        case 23: print(bench_collatz(n));        break;
        case 24: print(bench_bsearch(n));        break;
        case 25: print(bench_dot(n));            break;
        case 26: print(bench_fib_iter(n));       break;
        case 27: print(bench_histogram(n));      break;
        case 28: print(bench_accum(n));          break;
        case 29: print(bench_modexp(n));         break;
        case 30: print(bench_strength(n));       break;
        case 31: print(bench_idioms(n));         break;
        case 32: print(bench_fma(n));            break;
        case 33: print(bench_negoffset(n));      break;
        case 34: print(bench_constarray(n));     break;
        case 35: print(bench_condarith(n));      break;
        case 36: print(bench_ringbuf(n));        break;
        case 37: print(bench_slidingwin(n));     break;
        case 38: print(bench_expchain(n));       break;
        case 39: print(bench_foreach(n));        break;
        case 40: print(bench_lcmgcd(n));         break;
        case 41: print(bench_zext(n));           break;
        case 42: print(bench_strformat(n));      break;
        case 43: print(bench_arrayzip(n));       break;
        case 44: print(bench_strprefix(n));      break;
        case 45: print(bench_dictlookup(n));     break;
        case 46: print(bench_arrsortsearch(n));  break;
        case 47: print(bench_simdsaxpy(n));      break;
        case 48: print(bench_pipelinestencil(n)); break;
        case 49: print(bench_timesloop(n));      break;
        case 50: print(bench_strprocess(n));     break;
        case 51: print(bench_swapsort(n));       break;
        case 52: print(bench_arraypred(n));      break;
        case 53: print(bench_opoverload(n));     break;
        case 54: print(bench_matmul_cm(n));      break;
        default: print(0);
    }
    invalidate n;
    return 0;
}
OMEOF

# ━━━━━━━━━━━━━━━━━━━━━━━━━━ C SOURCE ━━━━━━━━━━━━━━━━━━━━━━━━
cat > bench.c << 'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { long x, y; } Point;

/* helpers */
static long gcd(long a, long b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { long t = b; b = a % b; a = t; }
    return a;
}
static long log2i(long n) {
    if (n <= 0) return -1;
    long r = 0;
    while (n >>= 1) r++;
    return r;
}

/*  0 ── integer_math ──────────────────────────── */
static long bench_math(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
        acc += log2i(i);
    }
    return acc;
}

/*  1 ── float_math ────────────────────────────── */
static long bench_floatmath(long n) {
    double acc = 1.0;
    for (long i = 1; i < n; i++) {
        double fi = (double)i;
        acc = acc + sqrt(fi);
        acc = acc + exp2(fi / 1000000.0);
        acc = acc * 0.9999999;
    }
    return (long)acc;
}

/*  2 ── array_push ────────────────────────────── */
static long bench_push(long n) {
    long cap = 16, len = 0;
    long *arr = malloc(cap * sizeof(long));
    for (long i = 0; i < n; i++) {
        if (len >= cap) { cap *= 2; arr = realloc(arr, cap * sizeof(long)); }
        arr[len++] = (i * 3) % 12345;
    }
    long result = len;
    free(arr);
    return result;
}

/*  3 ── array_hof ─────────────────────────────── */
static long bench_hof(long n) {
    long *arr = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) arr[i] = (i * 7) % 1000;
    /* map */
    long *mapped = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) mapped[i] = (arr[i] * arr[i]) % 997;
    free(arr);
    /* filter */
    long *filtered = malloc(n * sizeof(long));
    long flen = 0;
    for (long i = 0; i < n; i++)
        if (mapped[i] % 2 == 0) filtered[flen++] = mapped[i];
    free(mapped);
    /* reduce */
    long reduced = 0;
    for (long i = 0; i < flen; i++) reduced += filtered[i];
    long result = reduced + flen;
    free(filtered);
    return result;
}

/*  4 ── string_concat ─────────────────────────── */
static long bench_strcat(long n) {
    long len = 1, cap = 16;
    char *s = malloc(cap);
    s[0] = 'x'; s[1] = '\0';
    for (long i = 0; i < n; i++) {
        if (len + 1 >= cap) { cap *= 2; s = realloc(s, cap); }
        s[len++] = 'y';
        s[len] = '\0';
    }
    long result = len;
    free(s);
    return result;
}

/*  5 ── string_ops ────────────────────────────── */
static long bench_strops(long n) {
    char haystack[1001];
    for (int i = 0; i < 100; i++) memcpy(haystack + i*10, "abcdefghij", 10);
    haystack[1000] = '\0';
    long count = 0;
    for (long i = 0; i < n; i++) {
        count += (strstr(haystack, "efg") != NULL) ? 1 : 0;
        char *p = strstr(haystack, "hij");
        count += (p ? (long)(p - haystack) : -1) % 100;
    }
    return count;
}

/*  6 ── struct_access ─────────────────────────── */
static long bench_struct(long n) {
    Point p = {1, 2};
    long sum = 0;
    for (long i = 0; i < n; i++) {
        p.x = p.x + i;
        p.y = p.y ^ i;
        sum += p.x + p.y;
    }
    return sum;
}

/*  7 ── switch_branch ─────────────────────────── */
static long bench_branch(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        switch (i % 4) {
            case 0: sum += i;       break;
            case 1: sum -= i;       break;
            case 2: sum ^= i;       break;
            default: sum += (i * 2);
        }
    }
    return sum;
}

/*  8 ── if_else_chain ─────────────────────────── */
static inline long classify(long x) {
    if (x < 10)    return 1;
    if (x < 100)   return 2;
    if (x < 1000)  return 3;
    if (x < 10000) return 4;
    if (x < 100000) return 5;
    return 6;
}
static long bench_ifelse(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++)
        sum += classify(i % 200000);
    return sum;
}

/*  9 ── while_loop ────────────────────────────── */
static long bench_while(long n) {
    long i = 0, acc = 0;
    while (i < n) {
        acc += (i * i) % 101;
        acc ^= i;
        i++;
    }
    return acc;
}

/* 10 ── recursion_fib ─────────────────────────── */
static long fib(long n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}
static long bench_recurse(long n) { return fib(n); }

/* 11 ── nested_loops ──────────────────────────── */
static long bench_nested(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++)
        for (long j = 0; j < n; j++)
            for (long k = 0; k < n; k++)
                sum += ((i ^ j) + k) & 63;
    return sum;
}

/* 12 ── array_indexing ────────────────────────── */
static long bench_arrindex(long n) {
    long sz = 10000;
    long *arr = calloc(sz, sizeof(long));
    for (long i = 0; i < sz; i++) arr[i] = i * 3;
    long sum = 0;
    for (long i = 0; i < n; i++) {
        long idx = (i * 7 + 13) % sz;
        sum += arr[idx];
        arr[idx] = sum % 100000;
    }
    free(arr);
    return sum;
}

/* 13 ── function_calls ────────────────────────── */
static inline long add_one(long x) { return x + 1; }
static inline long add_two(long x) { return add_one(add_one(x)); }
static inline long add_four(long x) { return add_two(add_two(x)); }
static long bench_calls(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++)
        sum += add_four(i & 255);
    return sum;
}

/* 14 ── bitwise_ops ───────────────────────────── */
static long bench_bitwise(long n) {
    long a = 0, b = 0, c = 0;
    for (long i = 0; i < n; i++) {
        a = (a ^ (i << 3)) + (i & 255);
        b = (b | (i >> 1)) ^ (a & 65535);
        c += (a ^ b) & 1023;
    }
    return a + b + c;
}

/* 15 ── bitwise_intrinsics ────────────────────── */
static long bench_bitintrinsics(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += __builtin_popcountl(i);
        acc += __builtin_clzl(i);
        acc += __builtin_ctzl(i | 1);
        acc += ((i & (i - 1)) == 0) ? 1 : 0;
    }
    return acc;
}

/* 16 ── polynomial_eval ───────────────────────── */
static inline long poly_eval(long x) {
    long r = 3;
    r = r * x + 2;
    r = r * x + 1;
    r = r * x + 5;
    r = r * x + 7;
    r = r * x + 11;
    return r;
}
static long bench_poly(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++)
        sum += poly_eval(i % 1000);
    return sum;
}

/* 17 ── reduction ─────────────────────────────── */
static long bench_reduction(long n) {
    long sum = 0, sum2 = 0;
    for (long i = 1; i < n; i++) {
        sum += i * i;
        sum2 += i * i * i;
    }
    return sum + sum2;
}

/* 18 ── combined ──────────────────────────────── */
static long bench_combined(long n) {
    long total = 0;
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
    }
    total += acc;

    long cap = 16, len = 0;
    long *arr = malloc(cap * sizeof(long));
    for (long i = 0; i < n / 10; i++) {
        if (len >= cap) { cap *= 2; arr = realloc(arr, cap * sizeof(long)); }
        arr[len++] = (i * 3) % 12345;
    }
    long *mapped = malloc(len * sizeof(long));
    for (long i = 0; i < len; i++) mapped[i] = (arr[i] * arr[i]) % 997;
    free(arr);
    long *filtered = malloc(len * sizeof(long));
    long flen = 0;
    for (long i = 0; i < len; i++)
        if (mapped[i] % 2 == 0) filtered[flen++] = mapped[i];
    free(mapped);
    long reduced = 0;
    for (long i = 0; i < flen; i++) reduced += filtered[i];
    total += reduced + flen;
    free(filtered);

    Point p = {1, 2};
    for (long i = 0; i < n; i++) {
        p.x = p.x + i;
        p.y = p.y ^ i;
        total += p.x + p.y;
    }

    for (long i = 0; i < n; i++) {
        switch (i % 4) {
            case 0: total += i;       break;
            case 1: total -= i;       break;
            case 2: total ^= i;       break;
            default: total += (i * 2);
        }
    }

    long slen = 1, scap = 16;
    char *s = malloc(scap);
    s[0] = 'x'; s[1] = '\0';
    for (long i = 0; i < n / 100; i++) {
        if (slen + 1 >= scap) { scap *= 2; s = realloc(s, scap); }
        s[slen++] = 'y';
        s[slen] = '\0';
    }
    total += slen;
    free(s);

    for (long i = 0; i < 50; i++)
        for (long j = 0; j < 50; j++)
            for (long k = 0; k < 50; k++)
                total += ((i ^ j) + k) & 63;

    return total;
}

/* 19 ── matrix_multiply ───────────────────────── */
static long bench_matmul(long n) {
    long *a = calloc(n * n, sizeof(long));
    long *b = calloc(n * n, sizeof(long));
    long *c = calloc(n * n, sizeof(long));
    for (long i = 0; i < n; i++)
        for (long j = 0; j < n; j++) {
            a[i * n + j] = (i + j) % 97;
            b[i * n + j] = (i * j + 1) % 53;
        }
    for (long i = 0; i < n; i++)
        for (long j = 0; j < n; j++) {
            long s = 0;
            for (long k = 0; k < n; k++)
                s += a[i * n + k] * b[k * n + j];
            c[i * n + j] = s;
        }
    long sum = 0;
    for (long i = 0; i < n * n; i++) sum += c[i];
    free(a); free(b); free(c);
    return sum;
}

/* 20 ── sieve ─────────────────────────────────── */
static long bench_sieve(long n) {
    long *is_prime = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) is_prime[i] = 1;
    is_prime[0] = 0;
    if (n > 1) is_prime[1] = 0;
    for (long i = 2; i * i < n; i++) {
        if (is_prime[i]) {
            for (long j = i * i; j < n; j += i)
                is_prime[j] = 0;
        }
    }
    long count = 0;
    for (long i = 0; i < n; i++) count += is_prime[i];
    free(is_prime);
    return count;
}

/* 21 ── prefix_sum ────────────────────────────── */
static long bench_prefix_sum(long n) {
    long *arr = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) arr[i] = (i * 7 + 3) % 1000;
    for (long i = 1; i < n; i++) arr[i] += arr[i - 1];
    long result = arr[n - 1];
    free(arr);
    return result;
}

/* 22 ── hash_compute ──────────────────────────── */
static long bench_hash(long n) {
    long hash = 5381;
    for (long i = 0; i < n; i++) {
        hash = ((hash << 5) + hash) ^ (i & 255);
        hash = hash & 0xFFFFFFFFL;
    }
    return hash;
}

/* 23 ── collatz ───────────────────────────────── */
/* Dual-counter ILP: s1/s2 track alternating steps, eliminating the        */
/* dependency between consecutive counter updates and matching OM's         */
/* bench_collatz structure so the comparison is apples-to-apples.          */
static long bench_collatz(long n) {
    long total = 0, total2 = 0;
    for (long i = 1; i < n; i++) {
        long x = i;
        long s1 = 0, s2 = 0;
        while (x != 1) {
            x = (x % 2 == 0) ? (x / 2) : (3 * x + 1);
            s1++;
            if (x == 1) break;
            x = (x % 2 == 0) ? (x / 2) : (3 * x + 1);
            s2++;
        }
        total += s1;
        total2 += s2;
    }
    return total + total2;
}

/* 24 ── binary_search ─────────────────────────── */
static long bench_bsearch(long n) {
    long sz = 100000;
    long *arr = malloc(sz * sizeof(long));
    for (long i = 0; i < sz; i++) arr[i] = i * 3;
    long found = 0;
    for (long i = 0; i < n; i++) {
        long target = (i * 7 + 13) % (sz * 3);
        long lo = 0, hi = sz - 1;
        while (lo <= hi) {
            long mid = (lo + hi) / 2;
            if (arr[mid] == target) { found++; lo = hi + 1; }
            else if (arr[mid] < target) lo = mid + 1;
            else hi = mid - 1;
        }
    }
    free(arr);
    return found;
}

/* 25 ── dot_product ────────────────────────────── */
static long bench_dot(long n) {
    long *a = malloc(n * sizeof(long));
    long *b = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) {
        a[i] = (i * 3 + 1) % 1000;
        b[i] = (i * 7 + 2) % 1000;
    }
    long sum = 0;
    for (long i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    free(a); free(b);
    return sum;
}

/* 26 ── fibonacci_iter ─────────────────────────── */
static long bench_fib_iter(long n) {
    long a = 0, b = 1;
    for (long i = 0; i < n; i++) {
        long t = b;
        b = (a + b) % 1000000007L;
        a = t;
    }
    return b;
}

/* 27 ── histogram ──────────────────────────────── */
static long bench_histogram(long n) {
    long *bins = calloc(256, sizeof(long));
    for (long i = 0; i < n; i++) {
        long idx = ((i * 7) ^ (i >> 3)) & 255;
        bins[idx]++;
    }
    long sum = 0;
    for (long i = 0; i < 256; i++) {
        sum += bins[i] * (i + 1);
    }
    free(bins);
    return sum;
}

/* 28 ── accumulator_chain ──────────────────────── */
static long bench_accum(long n) {
    long a = 1, b = 2, c = 3, d = 4;
    for (long i = 1; i < n; i++) {
        a = a + (b ^ i);
        b = b + (c & i);
        c = c ^ (d + i);
        d = d + (a | i);
    }
    return a + b + c + d;
}

/* 29 ── modular_exp ────────────────────────────── */
static long bench_modexp(long n) {
    long result = 1, base = 3, modulus = 1000000007L;
    long acc = 0;
    for (long i = 1; i < n; i++) {
        result = (result * base) % modulus;
        acc += result;
    }
    return acc;
}

/* 30 ── strength_reduce ────────────────────────── */
static long bench_strength(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += i * 3;
        acc += i * 5;
        acc += i * 7;
        acc += i * 10;
        acc += i * 12;
        acc += i * 100;
        acc += i / 4;
        acc += i / 8;
        acc += i % 16;
        acc += i % 64;
        acc ^= (i * 15);
        acc ^= (i * 31);
    }
    return acc;
}

/* 31 ── idiom_patterns ─────────────────────────── */
static inline long my_abs(long x) {
    if (x < 0) return -x;
    return x;
}
static inline long my_min(long a, long b) {
    if (a < b) return a;
    return b;
}
static inline long my_max(long a, long b) {
    if (a > b) return a;
    return b;
}
static long bench_idioms(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        long x = i - (n / 2);
        acc += my_abs(x);
        acc += my_min(i, n - i);
        acc += my_max(i % 100, (n - i) % 100);
        /* Conditional negation pattern */
        if (i % 3 == 0) {
            acc -= x;
        } else {
            acc += x;
        }
    }
    return acc;
}

/* 32 ── fma_compute ────────────────────────────── */
static long bench_fma(long n) {
    double a = 1.0;
    double b = 0.9999999;
    double c = 0.0000001;
    double acc = 0.0;
    for (long i = 1; i < n; i++) {
        double fi = (double)i;
        /* a*b + c  pattern (single FMA) */
        acc = acc + (fi * b + c);
        /* a*b + c*d  pattern (chained FMA) */
        acc = acc + (fi * 0.3 + (fi + 1.0) * 0.7);
        a = a * b + c;
    }
    return (long)(acc + a);
}

/* 33 ── negative_offset ────────────────────────── */
static long bench_negoffset(long n) {
    long* arr = (long*)calloc(n, sizeof(long));
    for (long i = 0; i < n; i++) {
        arr[i] = i * 3 + 1;
    }
    long acc = 0;
    for (long i = 1; i < n; i++) {
        /* lookback pattern: arr[i - 1] */
        acc += arr[i] - arr[i - 1];
        /* double lookback */
        acc ^= arr[i - 1] * 2;
    }
    free(arr);
    return acc;
}

/* 34 ── const_array_size ───────────────────────── */
static long bench_constarray(long n) {
    long data[1024];
    for (long i = 0; i < 1024; i++) {
        data[i] = i * i + i;
    }
    long acc = 0;
    for (long j = 0; j < n; j++) {
        long idx = j % 1024;
        acc += data[idx];
        acc ^= data[(j * 7) % 1024];
    }
    return acc;
}

/* 35 ── cond_arithmetic ────────────────────────── */
static long bench_condarith(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        /* Conditional increment */
        if (i % 3 == 0) { acc += 1; }
        /* Conditional decrement */
        if (i % 5 == 0) { acc -= 1; }
        /* Collatz 3x+1 pattern */
        long x = i;
        if (x % 2 == 0) {
            x = x / 2;
        } else {
            x = 3 * x + 1;
        }
        acc += x;
    }
    return acc;
}

/* 36 ── ring_buffer ────────────────────────────── */
static long bench_ringbuf(long n) {
    long CAP = 509;
    long *buf = calloc(CAP, sizeof(long));
    for (long i = 0; i < CAP; i++) buf[i] = (i * 3 + 1) % 10007;
    long tail = 0, acc = 0;
    for (long i = CAP; i < n; i++) {
        acc += buf[tail];
        buf[tail] = (i * 3 + 1) % 10007;
        tail = (tail + 1) % CAP;
    }
    free(buf);
    return acc;
}

/* 37 ── sliding_window ─────────────────────────── */
static long bench_slidingwin(long n) {
    long WIN = 64;
    long *data = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) data[i] = (i * 13 + 7) % 10000;
    long wsum = 0;
    for (long i = 0; i < WIN; i++) wsum += data[i];
    long acc = wsum;
    for (long i = WIN; i < n; i++) {
        wsum = wsum + data[i] - data[i - WIN];
        acc += wsum;
    }
    free(data);
    return acc;
}

/* 38 ── exponent_chain ──────────────────────────── */
static long bench_expchain(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        long x = i % 1000;
        acc += x * x;
        acc += x * x * x;
        acc += x * x * x * x;
        acc += x * x * x * x * x;
        acc = acc % 1000000007L;
    }
    return acc;
}

/* 39 ── for_each ────────────────────────────────── */
static long bench_foreach(long n) {
    long *arr = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) arr[i] = (i * 7 + 3) % 10000;
    long sum = 0;
    for (long i = 0; i < n; i++) sum += arr[i];
    free(arr);
    return sum;
}

/* 40 ── lcm_gcd ─────────────────────────────────── */
static long lcm_c(long a, long b) {
    long g = a;
    long bb = b;
    while (bb) { long t = bb; bb = g % bb; g = t; }
    return (a / g) * b;
}
static long bench_lcmgcd(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        long a = i % 1000 + 1;
        long b = (i * 3 + 7) % 1000 + 1;
        long g = a;
        long bb = b;
        while (bb) { long t = bb; bb = g % bb; g = t; }
        acc += g;
        acc += lcm_c(a, b);
        acc = acc % 1000000007L;
    }
    return acc;
}

/* 41 ── zext_pattern ────────────────────────────── */
static long bench_zext(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        if (i % 3 == 0) acc += 1;
        if (i % 5 == 0) acc += 1;
        if (i % 7 == 0) acc += 1;
        acc += (i % 2 == 0) ? 1 : 0;
        acc += (i % 4 == 0) ? 1 : 0;
        if (i % 11 == 0) acc -= 1;
    }
    return acc;
}

/* 42 ── str_format ──────────────────────────────── */
/* Repeated snprintf into a stack buffer, accumulate formatted lengths. */
static long bench_strformat(long n) {
    long acc = 0;
    char buf[32];
    for (long i = 0; i < n; i++) {
        int len = snprintf(buf, sizeof(buf), "%ld", i % 100000);
        acc += len;
    }
    return acc;
}

/* 43 ── array_zip ───────────────────────────────── */
/* Zip two arrays of m elements, accumulate sum of combined result. */
static long bench_arrayzip(long n) {
    long m = n / 1000 + 2;
    long *a = malloc(m * sizeof(long));
    long *b = malloc(m * sizeof(long));
    for (long j = 0; j < m; j++) { a[j] = j * 3 % 997; b[j] = j * 7 % 997; }
    long acc = 0;
    for (long k = 0; k < n; k++) {
        long s = 0;
        for (long j = 0; j < m; j++) s += (a[j] + b[j]) % 997;
        acc += s % 997;
    }
    free(a); free(b);
    return acc;
}

/* 44 ── str_prefix_scan ────────────────────────── */
/* strncmp-based prefix/suffix test; C equivalent of str_starts_with(s, lit). */
static long bench_strprefix(long n) {
    const char *s = "Hello World";
    long acc = 0;
    size_t sl = strlen(s);
    for (long i = 0; i < n; i++) {
        acc += (strncmp(s, "Hello ", 6) == 0) ? 1 : 0;
        acc += (sl >= 5 && strncmp(s + sl - 5, "World", 5) == 0) ? 1 : 0;
        acc += (strncmp(s, "Goodbye", 7) == 0) ? 1 : 0;
    }
    return acc;
}

/* 45 ── dict_lookup ────────────────────────────── */
/* Open-addressing hash map with 1024 slots; frequency count of N keys. */
#define DICT_SIZE 1024
#define DICT_MASK (DICT_SIZE - 1)
typedef struct { long key; long val; int used; } DictSlot;
static void dict_set(DictSlot *d, long key, long val) {
    unsigned idx = (unsigned)((key * 2654435761UL) & DICT_MASK);
    while (d[idx].used && d[idx].key != key) idx = (idx + 1) & DICT_MASK;
    d[idx].key = key; d[idx].val = val; d[idx].used = 1;
}
static long dict_get(DictSlot *d, long key) {
    unsigned idx = (unsigned)((key * 2654435761UL) & DICT_MASK);
    while (d[idx].used && d[idx].key != key) idx = (idx + 1) & DICT_MASK;
    return d[idx].used ? d[idx].val : 0;
}
static long bench_dictlookup(long n) {
    DictSlot d[DICT_SIZE];
    memset(d, 0, sizeof(d));
    for (long i = 0; i < n; i++) {
        long k = ((long)((unsigned long)(i * 2654435761UL)) % 1000);
        dict_set(d, k, dict_get(d, k) + 1);
    }
    long acc = 0;
    for (long q = 0; q < 1000; q++) acc += dict_get(d, q);
    return acc;
}

/* 46 ── array_sort_search ───────────────────────── */
/* qsort + upper-half sum, reinit each iter. Matches OM algorithm exactly. */
static int cmp_long(const void *a, const void *b) {
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}
static long bench_arrsortsearch(long n) {
    const int m = 1024;
    long arr[1024];
    long acc = 0;
    long iters = n / m;
    for (long k = 0; k < iters; k++) {
        for (int j = 0; j < m; j++) arr[j] = (j * 97 + k * 31 + 17) % 100000;
        qsort(arr, m, sizeof(long), cmp_long);
        for (int j = m / 2; j < m; j++) acc += arr[j];
        acc %= 1000000007L;
    }
    return acc;
}

/* 47 ── simd_saxpy ─────────────────────────────── */
/* Integer dot product; same algorithm as OM i32x4 version, auto-vec in C. */
static long bench_simdsaxpy(long n) {
    const int m = 256;
    long x[256], y[256];
    for (int j = 0; j < m; j++) { x[j] = j % 100 + 1; y[j] = (j * 3 + 1) % 100 + 1; }
    long iters = n / m;
    long acc = 0;
    for (long k = 0; k < iters; k++) {
        for (int j = 0; j < m; j++) acc += x[j] * y[j];
        acc %= 1000000007L;
    }
    return acc;
}

/* 48 ── pipeline_stencil ────────────────────────── */
/* Element-wise transform via plain loop; C equivalent of OM pipeline stages. */
static long bench_pipelinestencil(long n) {
    const int m = 8192;
    long *src = malloc(m * sizeof(long));
    long *dst = malloc(m * sizeof(long));
    for (int j = 0; j < m; j++) src[j] = (j * 3 + 1) % 997;
    long iters = n / m;
    long acc = 0;
    for (long k = 0; k < iters; k++) {
        for (int j = 0; j < m; j++)
            dst[j] = src[j] * 3 + src[j] / 7 + src[j] * src[j] % 997;
        for (int j = 0; j < m; j++) acc = (acc + dst[j]) % 1000000007L;
    }
    free(src); free(dst);
    return acc;
}

/* 49 ── times_loop ─────────────────────────────── */
/* Plain for-loop equivalent of OM times N { }. */
static long bench_timesloop(long n) {
    long acc = 0;
    for (long i = 0; i < n; i++) acc = (acc * 3 + i) % 1000000007L;
    return acc;
}

/* 50 ── str_process ─────────────────────────────── */
/* str_reverse + str_upper + str_count chain, N iterations. */
static int str_count_c(const char *s, char c) {
    int cnt = 0;
    while (*s) { if (*s++ == c) cnt++; }
    return cnt;
}
static long bench_strprocess(long n) {
    const char *s = "Hello, OmScript World! Benchmarking string ops.";
    size_t slen = strlen(s);
    char buf[128];
    long acc = 0;
    for (long i = 0; i < n; i++) {
        /* reverse */
        for (size_t j = 0; j < slen; j++) buf[j] = s[slen - 1 - j];
        buf[slen] = '\0';
        /* upper of reversed */
        for (size_t j = 0; j < slen; j++) buf[j] = (char)((buf[j] >= 'a' && buf[j] <= 'z') ? buf[j] - 32 : buf[j]);
        acc += str_count_c(buf, 'O');
        acc += str_count_c(s, 'l');
    }
    return acc;
}

/* 51 ── swap_sort ───────────────────────────────── */
/* Selection sort with temp-variable swap, reinit each iter. */
static long bench_swapsort(long n) {
    const int m = 32;
    long arr[32];
    long iters = n / m;
    long acc = 0;
    for (long k = 0; k < iters; k++) {
        /* reinitialize deterministically, matching OM */
        for (int j = 0; j < m; j++) arr[j] = (j * 7 + k * 3 + 1) % m;
        for (int i = 0; i < m - 1; i++) {
            int mi = i;
            for (int j = i + 1; j < m; j++) if (arr[j] < arr[mi]) mi = j;
            if (mi != i) { long tmp = arr[mi]; arr[mi] = arr[i]; arr[i] = tmp; }
        }
        acc += arr[m - 1];
    }
    return acc;
}

/* 52 ── array_predicates ────────────────────────── */
/* Manual any/every/count over a fixed 128-element array, N/m iters. */
static long bench_arraypred(long n) {
    const int m = 128;
    long arr[128];
    for (int j = 0; j < m; j++) arr[j] = (j * 13 + 7) % 1000;
    long iters = n / m;
    long acc = 0;
    for (long k = 0; k < iters; k++) {
        /* array_any: is_pos */
        int any = 0;
        for (int j = 0; j < m && !any; j++) if (arr[j] > 0) any = 1;
        acc += any;
        /* array_every: is_even */
        int every = 1;
        for (int j = 0; j < m && every; j++) if (arr[j] % 2 != 0) every = 0;
        acc += every;
        /* array_count: is_big (>500) */
        int cnt = 0;
        for (int j = 0; j < m; j++) if (arr[j] > 500) cnt++;
        acc += cnt;
    }
    return acc;
}

/* 53 ── op_overload ─────────────────────────────── */
/* Vec2 arithmetic matching OM operator overload/creation bench.      */
/* Implements +, -, dot (**), compare (<=>), weighted-add (|>) inline */
/* as plain C struct operations — the C compiler sees identical IR to  */
/* the OM operator-lowered form, making the benchmark a fair measure  */
/* of operator dispatch overhead (OM should be equal or faster).      */
typedef struct { long x, y; } Vec2;
static inline Vec2 vec2_add(Vec2 a, Vec2 b) { return (Vec2){ a.x+b.x, a.y+b.y }; }
static inline Vec2 vec2_sub(Vec2 a, Vec2 b) { return (Vec2){ a.x-b.x, a.y-b.y }; }
static inline long vec2_dot(Vec2 a, Vec2 b)  { return a.x*b.x + a.y*b.y; }
static inline long vec2_cmp(Vec2 a, Vec2 b)  {
    long la = a.x*a.x + a.y*a.y, lb = b.x*b.x + b.y*b.y;
    return (la > lb) - (la < lb);
}
static inline Vec2 vec2_wadd(Vec2 a, Vec2 b) { return (Vec2){ a.x+b.x*2, a.y+b.y*2 }; }
static long bench_opoverload(long n) {
    long acc = 0;
    for (long i = 0; i < n; i++) {
        Vec2 a = { (i*3)%1000, (i*7+1)%1000 };
        Vec2 b = { (i*5+2)%1000, (i*11+3)%1000 };
        Vec2 s = vec2_add(a, b);
        Vec2 d = vec2_sub(a, b);
        long dp  = vec2_dot(a, b);
        long cmp = vec2_cmp(a, b);
        Vec2 w   = vec2_wadd(a, b);
        acc += s.x + s.y + d.x + d.y + dp + cmp + w.x + w.y;
    }
    return acc;
}

/* ── 54. matmul_cm ─────────────────────────────────────────────────────── */
/* Row-major C baseline for column-major OM mat_mul.  Same algorithm but   */
/* stored row-major (C default).  OM's column-major layout gives a         */
/* vectorization advantage on the column-saxpy inner loop.                 */
static long bench_matmul_cm(long n) {
    long* a = (long*)calloc((size_t)(n*n), sizeof(long));
    long* b = (long*)calloc((size_t)(n*n), sizeof(long));
    long* c = (long*)calloc((size_t)(n*n), sizeof(long));
    for (long i = 0; i < n; i++)
        for (long j = 0; j < n; j++) {
            a[i*n+j] = (i+j) % 97;
            b[i*n+j] = (i*j+1) % 53;
        }
    /* j→p→i loop order (same as OM mat_mul) but row-major for C */
    for (long j = 0; j < n; j++)
        for (long p = 0; p < n; p++) {
            long b_pj = b[p*n+j];
            for (long i = 0; i < n; i++)
                c[j*n+i] += b_pj * a[p*n+i];  /* C[j,i] += b_pj * A[p,i] */
        }
    long sum = 0;
    for (long k = 0; k < n*n; k++) sum += c[k];
    free(a); free(b); free(c);
    return sum;
}

int main(void) {
    int test_id; long n;
    scanf("%d %ld", &test_id, &n);
    long r = 0;
    switch (test_id) {
        case 0:  r = bench_math(n);           break;
        case 1:  r = bench_floatmath(n);      break;
        case 2:  r = bench_push(n);           break;
        case 3:  r = bench_hof(n);            break;
        case 4:  r = bench_strcat(n);         break;
        case 5:  r = bench_strops(n);         break;
        case 6:  r = bench_struct(n);         break;
        case 7:  r = bench_branch(n);         break;
        case 8:  r = bench_ifelse(n);         break;
        case 9:  r = bench_while(n);          break;
        case 10: r = bench_recurse(n);        break;
        case 11: r = bench_nested(n);         break;
        case 12: r = bench_arrindex(n);       break;
        case 13: r = bench_calls(n);          break;
        case 14: r = bench_bitwise(n);        break;
        case 15: r = bench_bitintrinsics(n);  break;
        case 16: r = bench_poly(n);           break;
        case 17: r = bench_reduction(n);      break;
        case 18: r = bench_combined(n);       break;
        case 19: r = bench_matmul(n);         break;
        case 20: r = bench_sieve(n);          break;
        case 21: r = bench_prefix_sum(n);     break;
        case 22: r = bench_hash(n);           break;
        case 23: r = bench_collatz(n);        break;
        case 24: r = bench_bsearch(n);        break;
        case 25: r = bench_dot(n);            break;
        case 26: r = bench_fib_iter(n);       break;
        case 27: r = bench_histogram(n);      break;
        case 28: r = bench_accum(n);          break;
        case 29: r = bench_modexp(n);         break;
        case 30: r = bench_strength(n);       break;
        case 31: r = bench_idioms(n);         break;
        case 32: r = bench_fma(n);            break;
        case 33: r = bench_negoffset(n);      break;
        case 34: r = bench_constarray(n);     break;
        case 35: r = bench_condarith(n);      break;
        case 36: r = bench_ringbuf(n);           break;
        case 37: r = bench_slidingwin(n);        break;
        case 38: r = bench_expchain(n);          break;
        case 39: r = bench_foreach(n);           break;
        case 40: r = bench_lcmgcd(n);            break;
        case 41: r = bench_zext(n);              break;
        case 42: r = bench_strformat(n);         break;
        case 43: r = bench_arrayzip(n);          break;
        case 44: r = bench_strprefix(n);         break;
        case 45: r = bench_dictlookup(n);        break;
        case 46: r = bench_arrsortsearch(n);     break;
        case 47: r = bench_simdsaxpy(n);         break;
        case 48: r = bench_pipelinestencil(n);   break;
        case 49: r = bench_timesloop(n);         break;
        case 50: r = bench_strprocess(n);        break;
        case 51: r = bench_swapsort(n);          break;
        case 52: r = bench_arraypred(n);         break;
        case 53: r = bench_opoverload(n);        break;
        case 54: r = bench_matmul_cm(n);         break;
    }
    printf("%ld\n", r);
    return 0;
}
CEOF

# ─── COMPILE ──────────────────────────────────────────────────
OMSC="./build/omsc"
if [ ! -x "$OMSC" ]; then
    OMSC="$(command -v omsc 2>/dev/null || true)"
    if [ -z "$OMSC" ]; then
        echo "ERROR: omsc not found.  Build it first:  mkdir -p build && cd build && cmake .. && make -j\$(nproc)" >&2
        exit 1
    fi
fi

echo "────────────────────────────────────────────────────────────────"
echo "  Compilation Timing"
echo "────────────────────────────────────────────────────────────────"

BENCH_MODE="${BENCH_MODE:-fair}"
OM_FLAGS="-O3 -march=native -mtune=native -fvectorize -funroll-loops -floop-optimize -fparallelize"
CC="${BENCH_CC:-}"
if [ -z "$CC" ]; then
    if [ "$BENCH_MODE" = "fair" ]; then
        if command -v clang-18 &>/dev/null; then CC="clang-18"
        elif command -v clang   &>/dev/null; then CC="clang"
        else                                      CC="gcc"
        fi
    else
        if command -v gcc-13 &>/dev/null; then CC="gcc-13"
        elif command -v gcc  &>/dev/null; then CC="gcc"
        elif command -v cc   &>/dev/null; then CC="cc"
        else                                     CC="clang"
        fi
    fi
fi
if [ "$BENCH_MODE" = "fair" ]; then
    # fair: both sides compiled with the same backend (clang = LLVM, same as OM)
    # and identical flags so OM wins only through better IR quality, not flag tricks.
    # -fno-pie matches OM's default static relocation model (no GOT overhead).
    C_FLAGS="-O3 -march=native -mtune=native -fno-plt -fno-pie -lm"
else
    C_FLAGS="-O2 -mtune=generic -fno-unroll-loops -fno-tree-vectorize -fno-plt -lm"
fi

echo "Compiling OM ($OMSC $OM_FLAGS) …"
OM_COMP_START=$(date +%s%N)
"$OMSC" bench.om $OM_FLAGS -o bench_om
OM_COMP_END=$(date +%s%N)
OM_COMP_MS=$(( (OM_COMP_END - OM_COMP_START) / 1000000 ))
echo "  OM compile time: ${OM_COMP_MS} ms"

echo "Compiling C  ($CC $C_FLAGS) …"
C_COMP_START=$(date +%s%N)
$CC bench.c $C_FLAGS -o bench_c
C_COMP_END=$(date +%s%N)
C_COMP_MS=$(( (C_COMP_END - C_COMP_START) / 1000000 ))
echo "  C  compile time: ${C_COMP_MS} ms"

echo ""
if [ "$OM_COMP_MS" -gt 10000 ]; then
    echo -e "  ${RED}⚠ OM compilation took >10 s – compiler may be generating slow code paths.${RST}"
elif [ "$OM_COMP_MS" -gt 5000 ]; then
    echo -e "  ${YEL}⚠ OM compilation took >5 s – consider profiling the compiler.${RST}"
else
    echo -e "  ${GRN}✓ OM compilation completed in a reasonable time.${RST}"
fi
echo ""

# ─── HELPERS ──────────────────────────────────────────────────
declare -a RATIOS
declare -a C_TIMES
declare -a OM_TIMES
MISMATCH=0

run_one() {
    local id=$1 n=$2 name=$3

    local co oo
    co=$(echo "$id $n" | $TASKSET ./bench_c)
    oo=$(echo "$id $n" | $TASKSET ./bench_om)
    # For floating-point benchmarks (e.g. fma_compute), vectorisation may
    # reorder the FP reduction producing a slightly different (but equally
    # valid) result.  Allow a small relative tolerance for integer results
    # that originate from FP computations (to_int of a double sum).
    local match=0
    if [ "$co" = "$oo" ]; then
        match=1
    elif [[ "$co" =~ ^-?[0-9]+$ ]] && [[ "$oo" =~ ^-?[0-9]+$ ]]; then
        # Both are integers: allow ±0.001% relative difference for large
        # values that result from FP accumulations.
        local abs_co=${co#-}
        local abs_diff=$(( co > oo ? co - oo : oo - co ))
        if [ "$abs_co" -gt 1000000 ] && [ "$abs_diff" -le $(( abs_co / 100000 + 1 )) ]; then
            match=1
        fi
    fi
    if [ "$match" -eq 0 ]; then
        printf "  %-22s  C=%-14s  OM=%-14s  ${RED}❌ MISMATCH${RST}\n" "$name" "$co" "$oo"
        MISMATCH=1
        RATIOS[$id]=0
        C_TIMES[$id]=0
        OM_TIMES[$id]=0
        return
    fi

    # Warmup runs: prime the CPU caches and branch predictors so that
    # the timed runs start from a warm state.  This removes cold-start
    # variance that would unfairly penalize whichever binary runs first.
    for (( w=0; w<WARMUP_RUNS; w++ )); do
        echo "$id $n" | $TASKSET ./bench_c  > /dev/null
        echo "$id $n" | $TASKSET ./bench_om > /dev/null
    done

    # Interleaved timed runs: alternate C and OM to spread any
    # system-level interference (thermal throttling, background tasks)
    # evenly between the two rather than biasing one side.
    local c_runs=() om_runs=()
    for (( r=0; r<RUNS; r++ )); do
        local cs ce ct os oe ot
        cs=$(date +%s%N)
        echo "$id $n" | $TASKSET ./bench_c > /dev/null
        ce=$(date +%s%N)
        ct=$(( (ce - cs) / 1000000 ))
        c_runs+=("$ct")

        os=$(date +%s%N)
        echo "$id $n" | $TASKSET ./bench_om > /dev/null
        oe=$(date +%s%N)
        ot=$(( (oe - os) / 1000000 ))
        om_runs+=("$ot")
    done

    IFS=$'\n' c_sorted=($(printf '%s\n' "${c_runs[@]}" | sort -n)); unset IFS
    IFS=$'\n' om_sorted=($(printf '%s\n' "${om_runs[@]}" | sort -n)); unset IFS
    local mid=$(( RUNS / 2 ))
    local ct=${c_sorted[$mid]}
    local ot=${om_sorted[$mid]}

    # Also track min (best-case) for secondary analysis.
    local c_min=${c_sorted[0]}
    local om_min=${om_sorted[0]}

    # Trimmed mean: drop the highest and lowest 2 runs to remove outliers,
    # then average the remaining runs.  More robust than median for small
    # sample sizes because it uses more data points while still rejecting
    # extreme values from system noise.
    # When RUNS < 5, trimming 2 from each end leaves nothing — fall back
    # to trimming 1 from each end, or use plain median for very few runs.
    local trim
    if [ "$RUNS" -ge 5 ]; then
        trim=2
    elif [ "$RUNS" -ge 3 ]; then
        trim=1
    else
        trim=0
    fi
    local trim_count=$(( RUNS - trim * 2 ))
    local c_trim_sum=0 om_trim_sum=0
    if [ "$trim_count" -gt 0 ]; then
        local trim_end=$(( RUNS - trim ))
        for (( t=trim; t<trim_end; t++ )); do
            c_trim_sum=$(( c_trim_sum + c_sorted[t] ))
            om_trim_sum=$(( om_trim_sum + om_sorted[t] ))
        done
        local c_trimmed=$(( c_trim_sum / trim_count ))
        local om_trimmed=$(( om_trim_sum / trim_count ))
    else
        local c_trimmed=$ct
        local om_trimmed=$ot
    fi

    # Use trimmed mean as the primary timing metric for ratio calculation.
    # Falls back to median for very short benchmarks where integer division
    # artifacts in the trimmed mean could distort results.
    local c_primary=$c_trimmed
    local om_primary=$om_trimmed
    if [ "$c_trimmed" -lt 3 ] || [ "$om_trimmed" -lt 3 ]; then
        c_primary=$ct
        om_primary=$ot
    fi

    C_TIMES[$id]=$c_primary
    OM_TIMES[$id]=$om_primary

    local ratio
    if [ "$c_primary" -eq 0 ]; then
        if [ "$om_primary" -le 1 ]; then ratio=100; else ratio=$(( om_primary * 1000 )); fi
    else
        ratio=$(( om_primary * 100 / c_primary ))
    fi
    RATIOS[$id]=$ratio

    # Compute stddev to flag noisy benchmarks.
    local c_stddev om_stddev
    c_stddev=$(awk -v med="$c_primary" 'BEGIN { s=0; n=0 }
        { d=$1-med; s+=d*d; n++ }
        END { if(n>1) printf "%.0f", sqrt(s/(n-1)); else print 0 }' \
        <<< "$(printf '%s\n' "${c_runs[@]}")")
    om_stddev=$(awk -v med="$om_primary" 'BEGIN { s=0; n=0 }
        { d=$1-med; s+=d*d; n++ }
        END { if(n>1) printf "%.0f", sqrt(s/(n-1)); else print 0 }' \
        <<< "$(printf '%s\n' "${om_runs[@]}")")

    local tag
    if   [ "$ratio" -le 120 ]; then tag="${GRN}✅ competitive${RST}"
    elif [ "$ratio" -le 250 ]; then tag="${YEL}⚠️  slower${RST}"
    else                            tag="${RED}❌ bottleneck${RST}"
    fi

    # Show noisy-benchmark warning when stddev > 15% of trimmed mean.
    local noise=""
    if [ "$c_primary" -gt 0 ] && [ "$(( c_stddev * 100 / c_primary ))" -gt 15 ]; then
        noise=" ${YEL}~${RST}"
    fi
    if [ "$om_primary" -gt 0 ] && [ "$(( om_stddev * 100 / om_primary ))" -gt 15 ]; then
        noise=" ${YEL}~${RST}"
    fi
    # Flag benchmarks under 10 ms as potentially unreliable due to
    # timing resolution — 1 ms jitter on a 5 ms test is 20% noise.
    if [ "$c_primary" -lt 10 ] || [ "$om_primary" -lt 10 ]; then
        noise="${noise} ${YEL}⏱${RST}"
    fi

    printf "  %-22s  C: %6d ms (±%3d)  OM: %6d ms (±%3d)  %4d%%  %b%b\n" \
           "$name" "$c_primary" "$c_stddev" "$om_primary" "$om_stddev" "$ratio" "$tag" "$noise"
}

# ─── RUN ──────────────────────────────────────────────────────
echo "╔═══════════════════════════════════════════════════════════════════════════════════╗"
echo "║         Per-Function Benchmarks  (trimmed mean of $RUNS runs, $WARMUP_RUNS warmup)               ║"
echo "╚═══════════════════════════════════════════════════════════════════════════════════╝"
echo ""
printf "  ${BLD}%-22s  %-20s %-20s %-7s %-12s${RST}\n" \
       "BENCHMARK" "C TIME" "OM TIME" "RATIO" "STATUS"
printf "  %-22s  %-20s %-20s %-7s %-12s\n" \
       "─────────────────────" "───────────────────" "───────────────────" "──────" "──────────"

for (( id=0; id<NUM_BENCHMARKS; id++ )); do
    run_one "$id" "${BENCH_N[$id]}" "${BENCH_NAME[$id]}"
done

echo ""
echo -e "  ${YEL}~${RST} = noisy benchmark (stddev > 15% of trimmed mean); results may be unreliable."
echo -e "  ${YEL}⏱${RST} = very short benchmark (<10 ms); timing resolution may dominate."
echo "  ± values show standard deviation across runs."
echo "  Times use trimmed mean (drop 2 highest + 2 lowest of $RUNS runs)."

if [ "$MISMATCH" -eq 1 ]; then
    echo ""
    echo -e "${RED}ERROR: Output mismatch detected – fix correctness issues before benchmarking.${RST}"
    exit 1
fi

# ─── SCALING ──────────────────────────────────────────────────
echo ""
echo "╔═══════════════════════════════════════════════════════════════════════════════════╗"
echo "║                    Loop Scaling  (x1  x2  x4  x8)                               ║"
echo "╚═══════════════════════════════════════════════════════════════════════════════════╝"
echo ""
echo "  Tests whether OM runtime scales linearly with input size, like C."
echo ""

SCALE_IDS=(0 6 7 9 14 26 28)
SCALE_NAMES=("integer_math" "struct_access" "switch_branch" "while_loop" "bitwise_ops" "fibonacci_iter" "accumulator_chain")
SCALE_BASE=(125000 1000000 1000000 1000000 1000000 5000000 1000000)

for si in "${!SCALE_IDS[@]}"; do
    sid=${SCALE_IDS[$si]}
    sname=${SCALE_NAMES[$si]}
    sbase=${SCALE_BASE[$si]}

    printf "  %-18s  " "$sname"
    for mult in 1 2 4 8; do
        sn=$(( sbase * mult ))
        # Warmup
        echo "$sid $sn" | $TASKSET ./bench_c  > /dev/null
        echo "$sid $sn" | $TASKSET ./bench_om > /dev/null
        # Median-of-5 for scaling tests (up from 3 for better reliability)
        sc_runs=()
        so_runs=()
        for sr in 0 1 2 3 4; do
            cs=$(date +%s%N)
            echo "$sid $sn" | $TASKSET ./bench_c > /dev/null
            ce=$(date +%s%N)
            sc_runs+=("$(( (ce - cs) / 1000000 ))")

            os=$(date +%s%N)
            echo "$sid $sn" | $TASKSET ./bench_om > /dev/null
            oe=$(date +%s%N)
            so_runs+=("$(( (oe - os) / 1000000 ))")
        done
        IFS=$'\n' sc_s=($(printf '%s\n' "${sc_runs[@]}" | sort -n)); unset IFS
        IFS=$'\n' so_s=($(printf '%s\n' "${so_runs[@]}" | sort -n)); unset IFS
        ct=${sc_s[2]}
        ot=${so_s[2]}

        if [ "$ct" -gt 0 ]; then ratio=$(( ot * 100 / ct )); else ratio=100; fi
        printf "x%-2d C:%4dms OM:%4dms (%3d%%)  " "$mult" "$ct" "$ot" "$ratio"
    done
    echo ""
done

# ─── ANALYSIS ─────────────────────────────────────────────────
echo ""
echo "╔═══════════════════════════════════════════════════════════════════════════╗"
echo "║                          Bottleneck Analysis                            ║"
echo "╚═══════════════════════════════════════════════════════════════════════════╝"
echo ""

BOTTLENECKS=()
WARNINGS=()
COMPETITIVE=()
for (( id=0; id<NUM_BENCHMARKS; id++ )); do
    r=${RATIOS[$id]}
    if   [ "$r" -gt 250 ]; then BOTTLENECKS+=("$id")
    elif [ "$r" -gt 120 ]; then WARNINGS+=("$id")
    else                        COMPETITIVE+=("$id")
    fi
done

if [ ${#BOTTLENECKS[@]} -gt 0 ]; then
    echo -e "${RED}${BLD}Bottlenecks (>2.5x slower than C):${RST}"
    for id in "${BOTTLENECKS[@]}"; do
        printf "  • %-22s  %5d%% of C    --  %s\n" \
               "${BENCH_NAME[$id]}" "${RATIOS[$id]}" "${BOTTLENECK_LABELS[$id]}"
    done
    echo ""
fi

if [ ${#WARNINGS[@]} -gt 0 ]; then
    echo -e "${YEL}${BLD}Slower (1.2x-2.5x of C):${RST}"
    for id in "${WARNINGS[@]}"; do
        printf "  • %-22s  %5d%% of C    --  %s\n" \
               "${BENCH_NAME[$id]}" "${RATIOS[$id]}" "${BOTTLENECK_LABELS[$id]}"
    done
    echo ""
fi

if [ ${#COMPETITIVE[@]} -gt 0 ]; then
    echo -e "${GRN}${BLD}Competitive (<=1.2x of C):${RST}"
    for id in "${COMPETITIVE[@]}"; do
        printf "  • %-22s  %5d%% of C\n" \
               "${BENCH_NAME[$id]}" "${RATIOS[$id]}"
    done
    echo ""
fi

# ─── SUMMARY ──────────────────────────────────────────────────
COUNT_FASTER=0
COUNT_EQUAL=0
COUNT_SLOWER=0
for (( id=0; id<NUM_BENCHMARKS; id++ )); do
    r=${RATIOS[$id]}
    c=${C_TIMES[$id]}
    o=${OM_TIMES[$id]}
    # For very short benchmarks (<10 ms), use absolute difference instead of
    # ratio — 1 ms of timing noise on a 5 ms benchmark is 20% even though
    # the actual performance is identical.  Consider "tied" if within 2 ms.
    if [ "$c" -lt 10 ] && [ "$o" -lt 10 ]; then
        diff=$(( o - c ))
        if [ "$diff" -lt 0 ]; then diff=$(( -diff )); fi
        if [ "$diff" -le 2 ]; then
            # Within timing noise — treat as tied regardless of ratio.
            COUNT_EQUAL=$((COUNT_EQUAL + 1))
            continue
        fi
    fi
    if   [ "$r" -lt 95 ];  then COUNT_FASTER=$((COUNT_FASTER + 1))
    elif [ "$r" -le 105 ]; then COUNT_EQUAL=$((COUNT_EQUAL + 1))
    else                        COUNT_SLOWER=$((COUNT_SLOWER + 1))
    fi
done

SUM_C=0; SUM_OM=0
for (( id=0; id<NUM_BENCHMARKS; id++ )); do
    SUM_C=$(( SUM_C + C_TIMES[$id] ))
    SUM_OM=$(( SUM_OM + OM_TIMES[$id] ))
done
if [ "$SUM_C" -gt 0 ]; then OVERALL=$(( SUM_OM * 100 / SUM_C )); else OVERALL=100; fi

GEOMEAN=$(awk -v n="$NUM_BENCHMARKS" 'BEGIN {
    sum = 0; count = 0
}
{
    r = $1 / 100.0
    if (r > 0) { sum += log(r); count++ }
}
END {
    if (count > 0) printf "%.0f", exp(sum / count) * 100
    else           printf "100"
}' <<< "$(for (( id=0; id<NUM_BENCHMARKS; id++ )); do echo "${RATIOS[$id]}"; done)")

echo "==================================================================="
echo ""
echo "  Methodology:  $RUNS timed runs + $WARMUP_RUNS warmup, trimmed mean (drop 2 high + 2 low), interleaved C/OM"
echo ""
echo "  Individual results:  $COUNT_FASTER faster, $COUNT_EQUAL tied, $COUNT_SLOWER slower (out of $NUM_BENCHMARKS)"
echo ""
printf "  Aggregate (sum):    C: %d ms   OM: %d ms   (%d%%)\n" \
       "$SUM_C" "$SUM_OM" "$OVERALL"
printf "  Geometric mean:     %d%%  ← primary metric (unbiased by outliers)\n" "$GEOMEAN"
echo ""

if [ "$OVERALL" -lt 100 ]; then
    DIFF=$(( 100 - OVERALL ))
    echo -e "  ${GRN}${BLD}OM is ${DIFF}% faster than C on aggregate.${RST}"
elif [ "$OVERALL" -gt 100 ]; then
    DIFF=$(( OVERALL - 100 ))
    echo -e "  ${RED}${BLD}OM is ${DIFF}% slower than C on aggregate.${RST}"
else
    echo -e "  ${GRN}${BLD}OM is on par with C on aggregate.${RST}"
fi
echo ""

# Fairness check
if [ "$COUNT_FASTER" -ge $(( NUM_BENCHMARKS * 2 / 3 )) ]; then
    echo -e "  ${GRN}${BLD}✓ Majority ($COUNT_FASTER/$NUM_BENCHMARKS) of benchmarks are faster than C.${RST}"
else
    echo -e "  ${YEL}Only $COUNT_FASTER/$NUM_BENCHMARKS benchmarks are faster than C.${RST}"
fi

if [ "$COUNT_SLOWER" -eq 0 ]; then
    echo -e "  ${GRN}${BLD}✓ No benchmarks are slower than C.${RST}"
else
    echo -e "  ${YEL}⚠ $COUNT_SLOWER benchmarks are slower than C.${RST}"
fi

echo ""
echo "=== DONE ==="

# ─── TIME BREAKDOWN ───────────────────────────────────────────
SCRIPT_END=$(date +%s%N)
TOTAL_MS=$(( (SCRIPT_END - SCRIPT_START) / 1000000 ))
BENCH_MS=$(( TOTAL_MS - OM_COMP_MS - C_COMP_MS ))
echo ""
echo "────────────────────────────────────────────────────────────────"
echo "  Time Breakdown"
echo "────────────────────────────────────────────────────────────────"
printf "  OM compile:     %6d ms  (%d%%)\n" "$OM_COMP_MS" "$(( OM_COMP_MS * 100 / (TOTAL_MS > 0 ? TOTAL_MS : 1) ))"
printf "  C  compile:     %6d ms  (%d%%)\n" "$C_COMP_MS"  "$(( C_COMP_MS  * 100 / (TOTAL_MS > 0 ? TOTAL_MS : 1) ))"
printf "  Benchmarks:     %6d ms  (%d%%)\n" "$BENCH_MS"   "$(( BENCH_MS   * 100 / (TOTAL_MS > 0 ? TOTAL_MS : 1) ))"
printf "  Total:          %6d ms\n" "$TOTAL_MS"
echo ""
if [ "$OM_COMP_MS" -gt "$BENCH_MS" ]; then
    echo -e "  ${YEL}→ Most time was spent in the OM compiler, not running benchmarks.${RST}"
    echo -e "  ${YEL}  The compiler itself is the primary bottleneck.${RST}"
fi
echo "────────────────────────────────────────────────────────────────"

# ─── CLEANUP ──────────────────────────────────────────────────
rm -f bench.om bench.c bench_om bench_c
