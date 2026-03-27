#!/bin/bash

set -eu

# ──────────────────────────────────────────────────────────────
#  OmScript Benchmark Suite  (Fair Edition)
#
#  36 diverse micro-benchmarks covering distinct workloads.
#  No category is over-represented.  Both OM and C implementations
#  are idiomatic and use the same algorithm.
#
#  Fair-comparison design:
#    - C compiled with clang (same LLVM backend as OM) so any
#      performance difference reflects frontend IR quality, not
#      backend differences between GCC and LLVM
#    - Override with BENCH_CC=gcc if desired
#    - No LTO for C (OM is single-file; LTO would give C unfair
#      inter-procedural advantage that OM already gets natively)
#    - No -ffast-math for either side (unsafe / non-IEEE)
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
#               BENCH_CC (default: clang-18 > clang > gcc)
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

NUM_BENCHMARKS=36

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
    5000000   #  9  while_loop
    35        # 10  recursion_fib  (fib(35) ~ 9 M calls)
    200       # 11  nested_loops   (200^3 = 8 M)
    2000000   # 12  array_indexing
    5000000   # 13  function_calls
    5000000   # 14  bitwise_ops
    5000000   # 15  bitwise_intrinsics
    5000000   # 16  polynomial_eval
    5000000   # 17  reduction
    100000    # 18  combined
    300       # 19  matrix_multiply (300x300 = 27M muls)
    5000000   # 20  sieve
    5000000   # 21  prefix_sum
    5000000   # 22  hash_compute
    1000000   # 23  collatz
    5000000   # 24  binary_search
    5000000   # 25  dot_product
    50000000  # 26  fibonacci_iter
    5000000   # 27  histogram
    5000000   # 28  accumulator_chain
    5000000   # 29  modular_exp
    5000000   # 30  strength_reduce
    5000000   # 31  idiom_patterns
    2000000   # 32  fma_compute
    5000000   # 33  negative_offset
    5000000   # 34  const_array_size
    5000000   # 35  cond_arithmetic
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

OPTMAX=:

// ── 0. integer_math ──────────────────────────────────────────
@hot @flatten @unroll
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
@hot @flatten @unroll
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
@hot
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
@hot @flatten @pure @unroll @vectorize
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
@hot @unroll @flatten @pure
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
@hot @flatten
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
@hot @flatten @pure @unroll @vectorize
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
@hot
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
@hot @inline
fn classify(x:int) -> int {
    if (x < 10)    { return 1; }
    if (x < 100)   { return 2; }
    if (x < 1000)  { return 3; }
    if (x < 10000) { return 4; }
    if (x < 100000){ return 5; }
    return 6;
}
@hot @flatten @vectorize
fn bench_ifelse(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        sum += classify(i % 200000);
    }
    return sum;
}

// ── 9. while_loop ────────────────────────────────────────────
@hot @flatten @unroll
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
@hot @pure
fn fib(n:int) -> int {
    if (n <= 1) { return n; }
    return fib(n - 1) + fib(n - 2);
}
@flatten @hot
fn bench_recurse(n:int) -> int {
    return fib(n);
}

// ── 11. nested_loops ─────────────────────────────────────────
@hot @flatten @pure @unroll @vectorize
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
@hot @flatten @unroll
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
@hot @inline
fn add_one(x:int) -> int { return x + 1; }
@hot @inline
fn add_two(x:int) -> int { return add_one(add_one(x)); }
@hot @inline
fn add_four(x:int) -> int { return add_two(add_two(x)); }
@hot @flatten @vectorize
fn bench_calls(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        sum += add_four(i % 1000);
    }
    invalidate n;
    return sum;
}

// ── 14. bitwise_ops ──────────────────────────────────────────
@hot @flatten @vectorize @unroll
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
@hot @flatten @unroll
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
@hot @flatten @pure @unroll @inline
fn poly_eval(x:int) -> int {
    var r:int = 3;
    r = r * x + 2;
    r = r * x + 1;
    r = r * x + 5;
    r = r * x + 7;
    r = r * x + 11;
    return r;
}
@hot @flatten @vectorize
fn bench_poly(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        sum += poly_eval(i % 1000);
    }
    return sum;
}

// ── 17. reduction ────────────────────────────────────────────
@hot @flatten @vectorize
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
@hot @unroll @flatten
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
@hot @flatten @unroll @vectorize
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
@hot @flatten
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
@hot @flatten @unroll
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
@hot @flatten @unroll @pure
fn bench_hash(@prefetch n:int) -> int {
    var hash:int = 5381;
    for (i:int in 0...n) {
        hash = ((hash << 5) + hash) ^ (i & 255);
        hash = hash & 0xFFFFFFFF;
    }
    return hash;
}

// ── 23. collatz ──────────────────────────────────────────────
@hot @flatten @unroll
fn bench_collatz(@prefetch n:int) -> int {
    var total_steps:int = 0;
    for (i:int in 1...n) {
        var x:int = i;
        var steps:int = 0;
        while (x != 1) {
            if (x % 2 == 0) {
                x = x / 2;
            } else {
                x = 3 * x + 1;
            }
            steps += 1;
        }
        total_steps += steps;
    }
    return total_steps;
}

// ── 24. binary_search ────────────────────────────────────────
@hot @flatten @unroll
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
@hot @flatten @vectorize @unroll
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
@hot @flatten @unroll
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
@hot @flatten @unroll
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
@hot @flatten @unroll
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
@hot @flatten @unroll
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
// Tests e-graph strength reduction: multiply and divide by small
// constants (3,5,7,10,12,100), modulo by powers of 2, and
// mixed shift-add patterns that the e-graph rewrites to cheaper
// shift+add/sub sequences.
@hot @flatten @unroll
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
// Tests superoptimizer idiom recognition: min, max, absolute value,
// conditional negation, and power-of-2 test patterns.
// Patterns are written inline to match the C version exactly.
@hot @flatten @unroll
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
// Tests HGOE FMA generation: floating-point multiply-add chains
// of the form a*b+c and a*b+c*d that the hardware graph optimizer
// converts to fused multiply-add instructions.
@hot @flatten @unroll @vectorize
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
// Tests negative-offset bounds check elision: arr[i-1] lookback
// pattern inside a loop starting from 1.  The compiler should
// prove that i-1 >= 0 because the loop starts at 1, and that
// i-1 < len(arr) because i < len(arr).
@hot @flatten @unroll
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
// Tests known-array-size bounds elision: array_fill with constant
// size + bounded loop access.  The compiler should track that
// the array has exactly 1024 elements and elide bounds checks
// when the loop bound <= 1024.
@hot @flatten @unroll
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
// Tests superoptimizer conditional increment/decrement detection.
// Patterns: if(cond) x++; else x; → x + zext(cond)
// These arise from conditional counter updates in tight loops.
@hot @flatten @unroll
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

OPTMAX!:

// ── main dispatch ────────────────────────────────────────────
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
        sum += add_four(i % 1000);
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
static long bench_collatz(long n) {
    long total_steps = 0;
    for (long i = 1; i < n; i++) {
        long x = i;
        long steps = 0;
        while (x != 1) {
            if (x % 2 == 0) x = x / 2;
            else x = 3 * x + 1;
            steps++;
        }
        total_steps += steps;
    }
    return total_steps;
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

OM_FLAGS="-O3 -march=native -mtune=native -fvectorize -funroll-loops -floop-optimize -fparallelize"
# Use clang (same LLVM backend as OM) for a fair comparison — any perf
# difference reflects frontend IR quality, not GCC-vs-LLVM backend diffs.
# Override: BENCH_CC=gcc bash test.sh
# No -flto: single-file compilation gets no cross-TU benefit anyway.
# -fno-plt: avoids PLT indirection overhead that OmScript never incurs.
# NOTE: -ffast-math removed from both OM and C flags — it is unsafe and can
# produce incorrect results for edge cases (NaN, Inf, signed zeros).
CC="${BENCH_CC:-}"
if [ -z "$CC" ]; then
    if command -v clang-18 &>/dev/null; then CC="clang-18"
    elif command -v clang   &>/dev/null; then CC="clang"
    else                                      CC="gcc"
    fi
fi
C_FLAGS="-O3 -march=native -mtune=native -funroll-loops -fno-plt -lm"

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
    if [ "$co" != "$oo" ]; then
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
