#!/bin/bash

set -e

# ──────────────────────────────────────────────────────────────
#  OmScript Benchmark Suite
#  Runs isolated micro-benchmarks (3 iterations each), compares
#  OM vs idiomatic C, and pinpoints which subsystem is slow.
# ──────────────────────────────────────────────────────────────

RUNS=3          # iterations per benchmark for stable timing

# Track wall-clock time for the entire script so we can report where time is spent.
SCRIPT_START=$(date +%s%N)

NUM_BENCHMARKS=48

BENCH_NAME=(
    "integer_math"
    "array_push"
    "array_hof"
    "string_concat"
    "string_ops"
    "struct_access"
    "switch_branch"
    "recursion_fib"
    "nested_loops"
    "array_sort"
    "while_loop"
    "if_else_chain"
    "array_indexing"
    "function_calls"
    "bitwise_ops"
    "combined"
    "float_math"
    "bitwise_intrinsics"
    "reduction"
    "modular_arith"
    "mixed_arithmetic"
    "polynomial_eval"
    "branch_table"
    "cascade_inline"
    "loop_strength"
    "deep_calls"
    "bitcount_loop"
    "xor_reduce"
    "multi_switch"
    "accum_chain"
    "sum_of_squares"
    "cond_accum"
    "inline_math"
    "bitshift_chain"
    "switch12"
    "pure_arith"
    "mixed_reduce"
    "deep_inline10"
    "branch_elim"
    "switch20"
    "triple_inline"
    "reduction4"
    "switch_inline"
    "unrolled_sum"
    "switch_dispatch4"
    "parallel_xor"
    "inline_arith6"
    "multi_reduce"
)

BENCH_DESC=(
    "GCD, log2, and modular arithmetic in a tight loop"
    "Push N elements onto a dynamically-growing array"
    "Higher-order array_map / array_filter / array_reduce with lambdas"
    "Repeated str_concat building a progressively longer string"
    "str_contains and str_index_of on a 1000-character haystack"
    "Struct field read/write in a tight loop"
    "Switch/case dispatch with 4 branches inside a loop"
    "Naive recursive Fibonacci (exponential call tree)"
    "Triple-nested loop with N^3 iterations"
    "Bubble-sort a pseudo-random array of N elements"
    "While-loop accumulator (compared to for-in)"
    "Cascading if/else classification of integer ranges"
    "Pseudo-random array read and write access pattern"
    "Deeply nested small-function call chains"
    "Shift, AND, OR, and XOR intensive loop"
    "End-to-end combined workload exercising all subsystems"
    "Floating-point sqrt, exp2, pow in a tight loop"
    "popcount, clz, ctz, is_power_of_2 in a tight loop"
    "Sum and modular product reduction"
    "Chained modular arithmetic with dependent variables"
    "Mixed arithmetic: multiply, shift, XOR, add"
    "Horner polynomial evaluation with @pure"
    "8-way branch dispatch in a tight loop"
    "8-level deep inline chain"
    "Loop with multiply strength reduction"
    "Deep call chain with varying args"
    "Popcount loop with accumulation"
    "4-wide XOR reduction loop"
    "Nested switch with inline dispatch"
    "Inlined accumulator chain"
    "Pure sum-of-squares reduction"
    "Conditional accumulation with branches"
    "Deeply inlined math chain"
    "Bit shift and mask chain"
    "12-way switch dispatch"
    "Pure arithmetic chain with @pure"
    "Multiple reduction with different ops"
    "10-level deep inline chain"
    "Branch elimination with @pure classify"
    "20-way switch dispatch"
    "Triple-level inline chain"
    "Four parallel reductions"
    "Switch dispatching to inline functions"
    "8-way parallel unrolled sum"
    "4-way switch with inline arithmetic helpers"
    "8 parallel XOR accumulators"
    "6-deep inline arithmetic chain"
    "Multiple independent reductions"
)

# Input sizes – tuned so each test takes ~30-200 ms in C.
# Previous values were far too large (fib(44) alone could take 30+ sec).
BENCH_N=(
    500000    #  0  integer_math
    500000    #  1  array_push
    200000    #  2  array_hof
    10000     #  3  string_concat
    500000    #  4  string_ops
    5000000   #  5  struct_access
    5000000   #  6  switch_branch
    35        #  7  recursion_fib   (fib(35) ~ 9 M calls, ~50 ms)
    200       #  8  nested_loops    (200^3 = 8 M)
    3000      #  9  array_sort     (bubble = O(n^2) = 9 M)
    5000000   # 10  while_loop
    5000000   # 11  if_else_chain
    2000000   # 12  array_indexing
    5000000   # 13  function_calls
    5000000   # 14  bitwise_ops
    100000    # 15  combined
    2000000   # 16  float_math
    5000000   # 17  bitwise_intrinsics
    5000000   # 18  reduction
    5000000   # 19  modular_arith
    5000000   # 20  mixed_arithmetic
    5000000   # 21  polynomial_eval
    5000000   # 22  branch_table
    5000000   # 23  cascade_inline
    5000000   # 24  loop_strength
    5000000   # 25  deep_calls
    5000000   # 26  bitcount_loop
    5000000   # 27  xor_reduce
    5000000   # 28  multi_switch
    5000000   # 29  accum_chain
    5000000   # 30  sum_of_squares
    5000000   # 31  cond_accum
    5000000   # 32  inline_math
    5000000   # 33  bitshift_chain
    5000000   # 34  switch12
    5000000   # 35  pure_arith
    5000000   # 36  mixed_reduce
    5000000   # 37  deep_inline10
    5000000   # 38  branch_elim
    5000000   # 39  switch20
    5000000   # 40  triple_inline
    5000000   # 41  reduction4
    5000000   # 42  switch_inline
    5000000   # 43  unrolled_sum
    5000000   # 44  switch_dispatch4
    5000000   # 45  parallel_xor
    5000000   # 46  inline_arith6
    5000000   # 47  multi_reduce
)

BOTTLENECK_LABELS=(
    "math builtin call overhead (gcd / log2)"
    "array reallocation strategy during push"
    "lambda / higher-order function dispatch overhead"
    "string allocation overhead on each concat"
    "string search and manipulation builtin overhead"
    "struct field-access indirection overhead"
    "switch codegen quality and branch prediction"
    "function-call overhead in deep recursion"
    "loop body codegen and auto-vectorization"
    "sort algorithm complexity (bubble sort is O(n^2))"
    "while-loop codegen quality"
    "branch codegen quality (if/else chains)"
    "array bounds-check overhead"
    "call/return overhead for small inlined functions"
    "bitwise operation codegen quality"
    "overall compiler optimization quality"
    "floating-point codegen and FP optimization rules"
    "LLVM intrinsic codegen for popcount/clz/ctz"
    ""
    ""
    ""
    "Horner scheme with @pure function semantics"
    "8-way switch codegen quality"
    "aggressive inlining of 8-level chain"
    "loop invariant code motion and strength reduction"
    "deep function call chain optimization"
    "popcount intrinsic loop optimization"
    "XOR reduction vectorization"
    "nested switch dispatch with inlining"
    "inlined accumulator chain optimization"
    "pure vectorizable reduction"
    "conditional accumulation optimization"
    "deeply inlined math chain"
    "bit shift and mask optimization"
    "12-way switch codegen quality"
    "pure arithmetic chain with constant propagation"
    "multi-operation reduction optimization"
    "10-level inlining and constant folding"
    "branch elimination through @pure annotation"
    "20-way switch codegen quality"
    "triple-level inlining optimization"
    "parallel reduction vectorization"
    "switch+inline combo optimization"
    "8-way parallel sum unrolling"
    "4-way switch with inline dispatch"
    "8-way parallel XOR reduction"
    "6-level inline chain optimization"
    "multiple independent reduction optimization"
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
echo "Generating source files …"

cat > bench.om << 'OMEOF'
@noalias
struct Point { hot int x, hot int y }

OPTMAX=:

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
@hot @flatten @pure @unroll
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
@hot @flatten @pure @unroll
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
@hot @pure
fn fib(n:int) -> int {
    if (n <= 1) { return n; }
    return fib(n - 1) + fib(n - 2);
}
@flatten @hot
fn bench_recurse(n:int) -> int {
    return fib(n);
}
@hot @flatten @pure @vectorize @unroll
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
@hot @flatten @vectorize
fn bench_sort(n:int) -> int {
    var arr:int[] = [];
    prefetch arr;
    for (i:int in 0...n:int) {
        arr = push(arr, (i * 2654435761) % 1000000);
    }
    sort(arr);
    var result:int = arr[0] + arr[n / 2] + arr[n - 1];
    invalidate arr;
    return result;
}
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
@hot
fn classify(x:int) -> int {
    if (x < 10)    { return 1; }
    if (x < 100)   { return 2; }
    if (x < 1000)  { return 3; }
    if (x < 10000) { return 4; }
    if (x < 100000){ return 5; }
    return 6;
}
@hot @flatten @unroll
fn bench_ifelse(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        sum += classify(i % 200000);
    }
    return sum;
}
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

@hot @inline
fn add_one(x:int) -> int { return x + 1; }
@hot @inline
fn add_two(x:int) -> int { return add_one(add_one(x)); }
@hot @inline
fn add_four(x:int) -> int { return add_two(add_two(x)); }
@hot @flatten @unroll
fn bench_calls(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        sum += add_four(i % 1000);
    }
    invalidate n;
    return sum;
}
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
@hot @unroll @flatten
fn bench_combined(n:int) -> int {
    var total:int = 0;

    // math
    var acc:int = 0;
    for (i:int in 1...n) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
    }
    total += acc;

    // array push + hof
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

    // struct
    var p:struct = Point { x: 1, y: 2 };
    for (i:int in 0...n) {
        p.x = p.x + i;
        p.y = p.y ^ i;
        total += p.x + p.y;
    }

    // branch
    for (i:int in 0...n) {
        switch (i % 4) {
            case 0: total += i;       break;
            case 1: total -= i;       break;
            case 2: total ^= i;       break;
            default: total += (i * 2);
        }
    }

    // string
    var s:str = "x";
    for (i:int in 0...(n / 100)) {
        s = str_concat(s, "y");
    }
    total += str_len(s);
    invalidate s;
    invalidate n;

    // nested loop (small)
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
@hot @flatten @pure @unroll
fn bench_modular(@prefetch n:int) -> int {
    var a:int = 0;
    var b:int = 0;
    var c:int = 0;
    for (i:int in 0...n) {
        a += i % 97;
        b += i % 193;
        c += i % 389;
    }
    return a + b + c;
}
@hot @flatten @unroll @pure
fn bench_arithmetic(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 1...n) {
        acc += (i * i + i) / 2;
        acc ^= (i * 3 + 7);
        acc += ((i & 255) << 3) + (i >> 2);
    }
    return acc;
}

// 21. polynomial_eval - Horner's method evaluation
@hot @flatten @pure @unroll
fn poly_eval(x:int) -> int {
    // Evaluate polynomial: 3x^5 + 2x^4 + x^3 + 5x^2 + 7x + 11
    var r:int = 3;
    r = r * x + 2;
    r = r * x + 1;
    r = r * x + 5;
    r = r * x + 7;
    r = r * x + 11;
    return r;
}
@hot @flatten @unroll
fn bench_poly(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        sum += poly_eval(i % 1000);
    }
    return sum;
}

// 22. branch_table - 8-way switch dispatch
@hot
fn bench_branch8(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        switch (i % 8) {
            case 0: sum += i;           break;
            case 1: sum -= i;           break;
            case 2: sum ^= i;           break;
            case 3: sum += (i * 2);     break;
            case 4: sum += (i >> 1);    break;
            case 5: sum -= (i & 255);   break;
            case 6: sum ^= (i << 1);    break;
            default: sum += (i * 3);
        }
    }
    invalidate n;
    return sum;
}

// 23. cascade_inline - 8-level deep inline chain
@hot @inline
fn level1(x:int) -> int { return x + 1; }
@hot @inline
fn level2(x:int) -> int { return level1(level1(x)); }
@hot @inline
fn level3(x:int) -> int { return level2(level2(x)); }
@hot @inline
fn level4(x:int) -> int { return level3(x) + level3(x + 1); }
@hot @flatten
fn bench_cascade(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        sum += level4(i % 1000);
    }
    invalidate n;
    return sum;
}

// 24. loop_strength - Loop with strength reduction
@hot @flatten @unroll @pure
fn bench_strength(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        // These multiplications can be strength-reduced to additions
        sum += i * 7;
        sum += i * 13;
        sum += i * 97;
        sum ^= (i * 5);
    }
    return sum;
}

// 25. deep_calls - Deep call chain with different args
@hot @inline
fn step_a(x:int) -> int { return x * 3 + 1; }
@hot @inline
fn step_b(x:int) -> int { return (x >> 1) + x; }
@hot @inline
fn step_c(x:int) -> int { return x ^ (x + 7); }
@hot @inline
fn step_d(x:int) -> int { return step_a(step_b(step_c(x))); }
@hot @inline
fn step_e(x:int) -> int { return step_d(x) + step_c(step_a(x)); }
@hot @flatten
fn bench_deep(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        sum += step_e(i % 10000);
    }
    invalidate n;
    return sum;
}

// 28. bitcount_loop - Popcount in a tight loop
@hot @flatten @unroll
fn bench_bitcount(@prefetch n:int) -> int {
    var acc:int = 0;
    for (i:int in 1...n) {
        acc += popcount(i);
        acc += popcount(i * 3);
    }
    return acc;
}

// 27. xor_reduce - XOR reduction loop (vectorizable)
@hot @flatten @unroll @vectorize
fn bench_xor_reduce(@prefetch n:int) -> int {
    var acc1:int = 0;
    var acc2:int = 0;
    var acc3:int = 0;
    var acc4:int = 0;
    for (i:int in 0...n) {
        acc1 ^= (i * 7 + 3);
        acc2 ^= (i * 13 + 5);
        acc3 ^= (i * 19 + 11);
        acc4 ^= (i * 31 + 17);
    }
    return acc1 + acc2 + acc3 + acc4;
}

// 28. multi_switch - Multi-level switch with inline helpers
@hot @inline
fn dispatch_inner(x:int) -> int {
    switch (x % 6) {
        case 0: return x * 2;
        case 1: return x + 3;
        case 2: return x ^ 7;
        case 3: return x - 1;
        case 4: return x * 3;
        default: return x + x;
    }
}
@hot @flatten @unroll
fn bench_multi_switch(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        switch (i % 8) {
            case 0: sum += dispatch_inner(i);     break;
            case 1: sum -= dispatch_inner(i + 1); break;
            case 2: sum ^= dispatch_inner(i + 2); break;
            case 3: sum += dispatch_inner(i + 3); break;
            case 4: sum -= dispatch_inner(i * 2); break;
            case 5: sum += dispatch_inner(i + 5); break;
            case 6: sum ^= dispatch_inner(i + 6); break;
            default: sum += dispatch_inner(i + 7);
        }
    }
    invalidate n;
    return sum;
}

// 29. accumulator_chain - Long dependency chain with inline
@hot @inline
fn acc_step1(x:int) -> int { return (x * 5 + 3) ^ (x >> 2); }
@hot @inline
fn acc_step2(x:int) -> int { return (x + 7) * 3 - (x >> 1); }
@hot @inline
fn acc_step3(x:int) -> int { return x ^ (x << 1) + (x & 0xff); }
@hot @flatten @unroll
fn bench_accum(@prefetch n:int) -> int {
    var a:int = 0;
    var b:int = 0;
    for (i:int in 0...n) {
        a += acc_step1(i % 10000);
        b += acc_step2(i % 10000);
        a ^= acc_step3(b % 10000);
    }
    invalidate n;
    return a + b;
}

// 30. sum_of_squares - Pure vectorizable reduction
@hot @flatten @unroll @vectorize @pure
fn bench_sumsq(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 1...n) {
        sum += i * i;
    }
    return sum;
}

// 31. conditional_accum - Branch-heavy accumulation
@hot @flatten @unroll
fn bench_cond_accum(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        if (i % 3 == 0) {
            sum += i * 2;
        } else if (i % 3 == 1) {
            sum -= i;
        } else {
            sum ^= i;
        }
    }
    return sum;
}

// 32. inline_math_chain - deeply inlined math
@hot @inline
fn math_a(x:int) -> int { return x * x + x; }
@hot @inline
fn math_b(x:int) -> int { return math_a(x) + math_a(x + 1); }
@hot @inline
fn math_c(x:int) -> int { return math_b(x) - math_b(x - 1); }
@hot @flatten @unroll
fn bench_inline_math(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 1...n) {
        sum += math_c(i % 1000);
    }
    invalidate n;
    return sum;
}

// 33. bitshift_chain - bit manipulation chain
@hot @flatten @unroll @pure
fn bench_bitshift(@prefetch n:int) -> int {
    var a:int = 0;
    var b:int = 0;
    for (i:int in 1...n) {
        a += (i << 3) - i;          // i * 7
        b += (i << 4) + (i << 1);   // i * 18
        a ^= (b >> 2);
        b += (a & 0xffff);
    }
    return a + b;
}

// 34. switch12 - 12-way switch dispatch
@hot @flatten @unroll
fn bench_switch12(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        switch (i % 12) {
            case 0: sum += i * 2;       break;
            case 1: sum -= i + 3;       break;
            case 2: sum ^= i * 5;       break;
            case 3: sum += (i >> 1);    break;
            case 4: sum -= i ^ 0xff;    break;
            case 5: sum += i * 7;       break;
            case 6: sum ^= (i << 2);    break;
            case 7: sum += i + i;       break;
            case 8: sum -= (i & 1023);  break;
            case 9: sum += i * 11;      break;
            case 10: sum ^= i + 42;     break;
            default: sum += i * 3;
        }
    }
    invalidate n;
    return sum;
}

// 35. pure_arith_chain - Pure arithmetic with @pure
@hot @inline @pure
fn arith_a(x:int) -> int { return x * x - x + 1; }
@hot @inline @pure
fn arith_b(x:int) -> int { return arith_a(x) + arith_a(x + 1) - arith_a(x - 1); }
@hot @flatten @unroll @pure
fn bench_pure_arith(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 1...n) {
        sum += arith_b(i % 5000);
    }
    invalidate n;
    return sum;
}

// 36. mixed_reduce - Multiple reduction with different ops
@hot @flatten @unroll @vectorize @pure
fn bench_mixed_reduce(@prefetch n:int) -> int {
    var sum:int = 0;
    var xor_acc:int = 0;
    var prod:int = 1;
    for (i:int in 1...n) {
        sum += i;
        xor_acc ^= (i * 3 + 7);
        prod = (prod * 3 + i) & 0x7fffffff;
    }
    return sum + xor_acc + prod;
}

// 37. deep_inline_call - Very deep inline call chain
@hot @inline
fn d1(x:int) -> int { return x + 1; }
@hot @inline
fn d2(x:int) -> int { return d1(x) * 2; }
@hot @inline
fn d3(x:int) -> int { return d2(x) + d1(x); }
@hot @inline
fn d4(x:int) -> int { return d3(x) - d2(x); }
@hot @inline
fn d5(x:int) -> int { return d4(x) + d3(x) + d1(x); }
@hot @inline
fn d6(x:int) -> int { return d5(x) ^ d4(x); }
@hot @inline
fn d7(x:int) -> int { return d6(x) + d5(x) - d3(x); }
@hot @inline
fn d8(x:int) -> int { return d7(x) + d6(x); }
@hot @inline
fn d9(x:int) -> int { return d8(x) ^ d7(x) + d5(x); }
@hot @inline
fn d10(x:int) -> int { return d9(x) + d8(x) - d6(x); }
@hot @flatten @unroll
fn bench_deep10(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        sum += d10(i % 10000);
    }
    invalidate n;
    return sum;
}

// 38. branch_elim - Switch-based range classification
@hot @inline
fn classify_switch(x:int) -> int {
    switch (x / 10000) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 3;
        case 3: return 4;
        case 4: return 5;
        case 5: return 6;
        case 6: return 7;
        case 7: return 8;
        case 8: return 9;
        case 9: return 10;
        case 10: return 11;
        case 11: return 12;
        case 12: return 13;
        case 13: return 14;
        case 14: return 15;
        case 15: return 16;
        case 16: return 17;
        case 17: return 18;
        case 18: return 19;
        default: return 20;
    }
}
@hot @flatten @unroll
fn bench_branch_elim(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        sum += classify_switch(i % 200000);
        sum += classify_switch((i * 7) % 200000);
    }
    invalidate n;
    return sum;
}

// 39. switch20 - 20-way switch (huge jump table)
@hot @flatten
fn bench_switch20(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        switch (i % 20) {
            case 0: sum += i;           break;
            case 1: sum -= i + 1;       break;
            case 2: sum ^= i * 3;      break;
            case 3: sum += i >> 1;      break;
            case 4: sum -= i & 511;     break;
            case 5: sum += i * 5;       break;
            case 6: sum ^= i << 1;     break;
            case 7: sum += i + 7;       break;
            case 8: sum -= i * 2;       break;
            case 9: sum += i ^ 13;     break;
            case 10: sum ^= i * 7;     break;
            case 11: sum += i >> 2;     break;
            case 12: sum -= i + 12;     break;
            case 13: sum += i * 11;     break;
            case 14: sum ^= i & 255;   break;
            case 15: sum += i * 9;      break;
            case 16: sum -= i ^ 7;     break;
            case 17: sum += i << 2;     break;
            case 18: sum ^= i + 42;    break;
            default: sum += i * 13;
        }
    }
    invalidate n;
    return sum;
}

// 40. triple_inline - Three-level deep inline chain
@hot @inline
fn t1a(x:int) -> int { return x * 3 + 1; }
@hot @inline
fn t1b(x:int) -> int { return x * 5 - 2; }
@hot @inline
fn t1c(x:int) -> int { return x ^ (x >> 1); }
@hot @inline
fn t2a(x:int) -> int { return t1a(x) + t1b(x); }
@hot @inline
fn t2b(x:int) -> int { return t1b(x) - t1c(x); }
@hot @inline
fn t3(x:int) -> int { return t2a(x) ^ t2b(x) + t1a(x); }
@hot @flatten @unroll
fn bench_triple_inline(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        sum += t3(i % 10000);
    }
    invalidate n;
    return sum;
}

// 41. reduction4 - Four parallel reductions
@hot @flatten @unroll @vectorize @pure
fn bench_reduce4(@prefetch n:int) -> int {
    var a:int = 0;
    var b:int = 0;
    var c:int = 0;
    var d:int = 0;
    for (i:int in 1...n) {
        a += i;
        b += i * i;
        c += i * 3;
        d ^= i;
    }
    return a + b + c + d;
}

// 42. switch_inline_combo - Switch dispatching to inline functions
@hot @inline
fn op_add(a:int, b:int) -> int { return a + b; }
@hot @inline
fn op_sub(a:int, b:int) -> int { return a - b; }
@hot @inline
fn op_xor(a:int, b:int) -> int { return a ^ b; }
@hot @inline
fn op_mul2(a:int, b:int) -> int { return a + b * 2; }
@hot @flatten @unroll
fn bench_switch_inline(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        switch (i & 3) {
            case 0: sum = op_add(sum, i);     break;
            case 1: sum = op_sub(sum, i);     break;
            case 2: sum = op_xor(sum, i);     break;
            default: sum = op_mul2(sum, i);
        }
    }
    invalidate n;
    return sum;
}

// 43. unrolled_sum - Sum with @unroll for ILP
@hot @flatten @unroll @pure @vectorize
fn bench_unrolled_sum(@prefetch n:int) -> int {
    var s1:int = 0;
    var s2:int = 0;
    var s3:int = 0;
    var s4:int = 0;
    var s5:int = 0;
    var s6:int = 0;
    var s7:int = 0;
    var s8:int = 0;
    for (i:int in 0...n) {
        s1 += i;
        s2 += (i + 1);
        s3 += (i + 2);
        s4 += (i + 3);
        s5 ^= i;
        s6 ^= (i + 1);
        s7 ^= (i + 2);
        s8 ^= (i + 3);
    }
    return s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8;
}

// 44. switch_dispatch4 - 4-way switch with inline helpers
@hot @inline
fn sw4_op0(x:int) -> int { return (x * 7 + 3) ^ (x >> 2); }
@hot @inline
fn sw4_op1(x:int) -> int { return (x * 13 - 5) + (x & 0xff); }
@hot @inline
fn sw4_op2(x:int) -> int { return (x << 2) ^ (x * 3 + 11); }
@hot @inline
fn sw4_op3(x:int) -> int { return (x + x * 5) - (x >> 3); }
@hot @flatten @unroll
fn bench_switch4(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        switch (i & 3) {
            case 0: sum += sw4_op0(i); break;
            case 1: sum += sw4_op1(i); break;
            case 2: sum += sw4_op2(i); break;
            default: sum += sw4_op3(i);
        }
    }
    invalidate n;
    return sum;
}

// 45. parallel_xor - 8 parallel XOR accumulators
@hot @flatten @unroll @vectorize @pure
fn bench_par_xor(@prefetch n:int) -> int {
    var a:int = 0; var b:int = 0;
    var c:int = 0; var d:int = 0;
    var e:int = 0; var f:int = 0;
    var g:int = 0; var h:int = 0;
    for (i:int in 0...n) {
        a ^= (i * 7);
        b ^= (i * 13);
        c ^= (i * 19);
        d ^= (i * 31);
        e ^= (i * 37);
        f ^= (i * 43);
        g ^= (i * 53);
        h ^= (i * 61);
    }
    return a + b + c + d + e + f + g + h;
}

// 46. inline_arith6 - 6-deep inline arithmetic chain
@hot @inline
fn ia1(x:int) -> int { return x * 2 + 1; }
@hot @inline
fn ia2(x:int) -> int { return ia1(x) + ia1(x + 1); }
@hot @inline
fn ia3(x:int) -> int { return ia2(x) * 3 - ia1(x); }
@hot @inline
fn ia4(x:int) -> int { return ia3(x) + ia2(x + 1); }
@hot @inline
fn ia5(x:int) -> int { return ia4(x) ^ ia3(x + 2); }
@hot @inline
fn ia6(x:int) -> int { return ia5(x) + ia4(x) - ia2(x); }
@hot @flatten @unroll
fn bench_inline6(@prefetch n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        sum += ia6(i % 5000);
    }
    invalidate n;
    return sum;
}

// 47. multi_reduce - Multiple independent reductions
@hot @flatten @unroll @vectorize @pure
fn bench_multi_reduce(@prefetch n:int) -> int {
    var sum1:int = 0;
    var sum2:int = 0;
    var sum3:int = 0;
    var sum4:int = 0;
    for (i:int in 1...n) {
        sum1 += i * i;
        sum2 += i * i * i;
        sum3 += i * 3 + 1;
        sum4 ^= (i * 7 + 13);
    }
    return sum1 + sum2 + sum3 + sum4;
}
OPTMAX!:

@flatten @hot
fn main() -> int {
    var test_id:int = input();
    prefetch var n:int = input();

    switch (test_id) {
        case 0:  print(bench_math(n));     break;
        case 1:  print(bench_push(n));     break;
        case 2:  print(bench_hof(n));      break;
        case 3:  print(bench_strcat(n));   break;
        case 4:  print(bench_strops(n));   break;
        case 5:  print(bench_struct(n));   break;
        case 6:  print(bench_branch(n));   break;
        case 7:  print(bench_recurse(n));  break;
        case 8:  print(bench_nested(n));   break;
        case 9:  print(bench_sort(n));     break;
        case 10: print(bench_while(n));    break;
        case 11: print(bench_ifelse(n));   break;
        case 12: print(bench_arrindex(n)); break;
        case 13: print(bench_calls(n));    break;
        case 14: print(bench_bitwise(n));  break;
        case 15: print(bench_combined(n)); break;
        case 16: print(bench_floatmath(n));      break;
        case 17: print(bench_bitintrinsics(n));  break;
        case 18: print(bench_reduction(n));      break;
        case 19: print(bench_modular(n));         break;
        case 20: print(bench_arithmetic(n));      break;
        case 21: print(bench_poly(n));            break;
        case 22: print(bench_branch8(n));         break;
        case 23: print(bench_cascade(n));         break;
        case 24: print(bench_strength(n));        break;
        case 25: print(bench_deep(n));            break;
        case 26: print(bench_bitcount(n));        break;
        case 27: print(bench_xor_reduce(n));      break;
        case 28: print(bench_multi_switch(n));    break;
        case 29: print(bench_accum(n));           break;
        case 30: print(bench_sumsq(n));           break;
        case 31: print(bench_cond_accum(n));      break;
        case 32: print(bench_inline_math(n));     break;
        case 33: print(bench_bitshift(n));        break;
        case 34: print(bench_switch12(n));       break;
        case 35: print(bench_pure_arith(n));     break;
        case 36: print(bench_mixed_reduce(n));   break;
        case 37: print(bench_deep10(n));         break;
        case 38: print(bench_branch_elim(n));    break;
        case 39: print(bench_switch20(n));       break;
        case 40: print(bench_triple_inline(n));  break;
        case 41: print(bench_reduce4(n));        break;
        case 42: print(bench_switch_inline(n));  break;
        case 43: print(bench_unrolled_sum(n));   break;
        case 44: print(bench_switch4(n));        break;
        case 45: print(bench_par_xor(n));        break;
        case 46: print(bench_inline6(n));        break;
        case 47: print(bench_multi_reduce(n));   break;
        default: print(0);
    }
    invalidate n;
    return 0;
}
OMEOF

# ─── C SOURCE ────────────────────────────────────────────────
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

/*  0 ── integer math ──────────────────────────── */
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

/*  1 ── array push ────────────────────────────── */
static long bench_push(long n) {
    long cap = 16, len = 0;
    long *arr = malloc(cap * sizeof(long));
    for (long i = 0; i < n; i++) {
        if (len == cap) { cap *= 2; arr = realloc(arr, cap * sizeof(long)); }
        arr[len++] = (i * 3) % 12345;
    }
    free(arr);
    return len;
}

/*  2 ── higher-order (map/filter/reduce) ──────── */
static long bench_hof(long n) {
    long *arr = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) arr[i] = (i * 7) % 1000;

    long *mapped = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) mapped[i] = (arr[i] * arr[i]) % 997;

    long reduced = 0, count = 0;
    for (long i = 0; i < n; i++) {
        if (mapped[i] % 2 == 0) { reduced += mapped[i]; count++; }
    }
    free(arr); free(mapped);
    return reduced + count;
}

/*  3 ── string concat ─────────────────────────── */
static long bench_strcat(long n) {
    long cap = 16, len = 1;
    char *s = malloc(cap);
    s[0] = 'x'; s[1] = '\0';
    for (long i = 0; i < n; i++) {
        if (len + 2 > cap) { cap *= 2; s = realloc(s, cap); }
        s[len++] = 'y'; s[len] = '\0';
    }
    long r = len;
    free(s);
    return r;
}

/*  4 ── string ops ────────────────────────────── */
static long bench_strops(long n) {
    const char *unit = "abcdefghij";
    char haystack[1001];
    for (int i = 0; i < 100; i++) memcpy(haystack + i * 10, unit, 10);
    haystack[1000] = '\0';
    long count = 0;
    for (long i = 0; i < n; i++) {
        count += (strstr(haystack, "efg") != NULL);
        const char *p = strstr(haystack, "hij");
        count += (p ? (long)(p - haystack) : -1) % 100;
    }
    return count;
}

/*  5 ── struct access ─────────────────────────── */
static long bench_struct(long n) {
    Point p = {1, 2};
    long sum = 0;
    for (long i = 0; i < n; i++) {
        p.x += i; p.y ^= i; sum += p.x + p.y;
    }
    return sum;
}

/*  6 ── switch/branch ─────────────────────────── */
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

/*  7 ── recursion (fibonacci) ─────────────────── */
static long fib(long n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}
static long bench_recurse(long n) { return fib(n); }

/*  8 ── nested loops ──────────────────────────── */
static long bench_nested(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++)
        for (long j = 0; j < n; j++)
            for (long k = 0; k < n; k++)
                sum += ((i ^ j) + k) & 63;
    return sum;
}

/*  9 ── sort (bubble, same as OM built-in) ────── */
static long bench_sort(long n) {
    long *arr = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) arr[i] = (i * 2654435761UL) % 1000000;
    for (long i = 0; i < n; i++)
        for (long j = 0; j < n - i - 1; j++)
            if (arr[j] > arr[j + 1]) { long t = arr[j]; arr[j] = arr[j + 1]; arr[j + 1] = t; }
    long r = arr[0] + arr[n / 2] + arr[n - 1];
    free(arr);
    return r;
}

/* 10 ── while loop ────────────────────────────── */
static long bench_while(long n) {
    long i = 0, acc = 0;
    while (i < n) {
        acc += (i * i) % 101;
        acc ^= i;
        i++;
    }
    return acc;
}

/* 11 ── if/else chain ─────────────────────────── */
static long classify(long x) {
    if (x < 10)     return 1;
    if (x < 100)    return 2;
    if (x < 1000)   return 3;
    if (x < 10000)  return 4;
    if (x < 100000) return 5;
    return 6;
}
static long bench_ifelse(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) sum += classify(i % 200000);
    return sum;
}

/* 12 ── array indexing ────────────────────────── */
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

/* 13 ── function calls ────────────────────────── */
static long add_one(long x) { return x + 1; }
static long add_two(long x) { return add_one(add_one(x)); }
static long add_four(long x) { return add_two(add_two(x)); }
static long bench_calls(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) sum += add_four(i % 1000);
    return sum;
}

/* 14 ── bitwise ops ───────────────────────────── */
static long bench_bitwise(long n) {
    long a = 0, b = 0, c = 0;
    for (long i = 0; i < n; i++) {
        a = (a ^ (i << 3)) + (i & 255);
        b = (b | (i >> 1)) ^ (a & 65535);
        c += (a ^ b) & 1023;
    }
    return a + b + c;
}

/* 15 ── combined workload ─────────────────────── */
static long bench_combined(long n) {
    long total = 0;

    /* math */
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
    }
    total += acc;

    /* array push + hof */
    long hn = n / 10;
    long cap = 16, len = 0;
    long *arr = malloc(cap * sizeof(long));
    for (long i = 0; i < hn; i++) {
        if (len == cap) { cap *= 2; arr = realloc(arr, cap * sizeof(long)); }
        arr[len++] = (i * 3) % 12345;
    }
    long *mapped = malloc(len * sizeof(long));
    for (long i = 0; i < len; i++) mapped[i] = (arr[i] * arr[i]) % 997;
    long reduced = 0, count = 0;
    for (long i = 0; i < len; i++) {
        if (mapped[i] % 2 == 0) { reduced += mapped[i]; count++; }
    }
    total += reduced + count;
    free(arr); free(mapped);

    /* struct */
    { Point p = {1, 2};
      for (long i = 0; i < n; i++) {
        p.x += i; p.y ^= i; total += p.x + p.y;
      }
    }

    /* branch */
    for (long i = 0; i < n; i++) {
        switch (i % 4) {
            case 0: total += i;       break;
            case 1: total -= i;       break;
            case 2: total ^= i;       break;
            default: total += (i * 2);
        }
    }

    /* string */
    { long sn = n / 100;
      long sc = 16; long sl = 1;
      char *s = malloc(sc);
      s[0] = 'x'; s[1] = '\0';
      for (long i = 0; i < sn; i++) {
        if (sl + 2 > sc) { sc *= 2; s = realloc(s, sc); }
        s[sl++] = 'y'; s[sl] = '\0';
      }
      total += sl;
      free(s);
    }

    /* nested loop */
    { long ns = 50;
      for (long i = 0; i < ns; i++)
        for (long j = 0; j < ns; j++)
          for (long k = 0; k < ns; k++)
            total += ((i ^ j) + k) & 63;
    }

    return total;
}

/* 16 ── float math ────────────────────────────── */
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

/* 17 ── bitwise intrinsics ────────────────────── */
static long bench_bitintrinsics(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += __builtin_popcountl(i);
        acc += __builtin_clzl(i);
        acc += __builtin_ctzl(i | 1);
        acc += (i > 0 && (i & (i - 1)) == 0) ? 1 : 0;
    }
    return acc;
}

/* 18 ── reduction ─────────────────────────────── */
static long bench_reduction(long n) {
    long sum = 0;
    long sum2 = 0;
    for (long i = 1; i < n; i++) {
        sum += i * i;
        sum2 += i * i * i;
    }
    return sum + sum2;
}

/* 19 ── modular arithmetic ────────────────────── */
static long bench_modular(long n) {
    long a = 0, b = 0, c = 0;
    for (long i = 0; i < n; i++) {
        a += i % 97;
        b += i % 193;
        c += i % 389;
    }
    return a + b + c;
}

/* 20 ── mixed arithmetic ──────────────────────── */
static long bench_arithmetic(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += (i * i + i) / 2;
        acc ^= (i * 3 + 7);
        acc += ((i & 255) << 3) + (i >> 2);
    }
    return acc;
}

/* 21 ── polynomial evaluation ────────────────── */
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
    for (long i = 0; i < n; i++) {
        sum += poly_eval(i % 1000);
    }
    return sum;
}

/* 22 ── 8-way branch table ────────────────────── */
static long bench_branch8(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        switch (i % 8) {
            case 0: sum += i;           break;
            case 1: sum -= i;           break;
            case 2: sum ^= i;           break;
            case 3: sum += (i * 2);     break;
            case 4: sum += (i >> 1);    break;
            case 5: sum -= (i & 255);   break;
            case 6: sum ^= (i << 1);    break;
            default: sum += (i * 3);
        }
    }
    return sum;
}

/* 23 ── cascade inline ────────────────────────── */
static inline long level1(long x) { return x + 1; }
static inline long level2(long x) { return level1(level1(x)); }
static inline long level3(long x) { return level2(level2(x)); }
static inline long level4(long x) { return level3(x) + level3(x + 1); }
static long bench_cascade(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        sum += level4(i % 1000);
    }
    return sum;
}

/* 24 ── loop strength reduction ───────────────── */
static long bench_strength(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        sum += i * 7;
        sum += i * 13;
        sum += i * 97;
        sum ^= (i * 5);
    }
    return sum;
}

/* 25 ── deep calls ───────────────────────────── */
static inline long step_a(long x) { return x * 3 + 1; }
static inline long step_b(long x) { return (x >> 1) + x; }
static inline long step_c(long x) { return x ^ (x + 7); }
static inline long step_d(long x) { return step_a(step_b(step_c(x))); }
static inline long step_e(long x) { return step_d(x) + step_c(step_a(x)); }
static long bench_deep(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        sum += step_e(i % 10000);
    }
    return sum;
}

/* 28 ── bitcount loop ────────────────────────── */
static long bench_bitcount(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += __builtin_popcountl(i);
        acc += __builtin_popcountl(i * 3);
    }
    return acc;
}

/* 27 ── xor_reduce ────────────────────────────── */
static long bench_xor_reduce(long n) {
    long acc1 = 0, acc2 = 0, acc3 = 0, acc4 = 0;
    for (long i = 0; i < n; i++) {
        acc1 ^= (i * 7 + 3);
        acc2 ^= (i * 13 + 5);
        acc3 ^= (i * 19 + 11);
        acc4 ^= (i * 31 + 17);
    }
    return acc1 + acc2 + acc3 + acc4;
}

/* 28 ── multi_switch ──────────────────────────── */
static inline long dispatch_inner(long x) {
    switch (x % 6) {
        case 0: return x * 2;
        case 1: return x + 3;
        case 2: return x ^ 7;
        case 3: return x - 1;
        case 4: return x * 3;
        default: return x + x;
    }
}
static long bench_multi_switch(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        switch (i % 8) {
            case 0: sum += dispatch_inner(i);     break;
            case 1: sum -= dispatch_inner(i + 1); break;
            case 2: sum ^= dispatch_inner(i + 2); break;
            case 3: sum += dispatch_inner(i + 3); break;
            case 4: sum -= dispatch_inner(i * 2); break;
            case 5: sum += dispatch_inner(i + 5); break;
            case 6: sum ^= dispatch_inner(i + 6); break;
            default: sum += dispatch_inner(i + 7);
        }
    }
    return sum;
}

/* 29 ── accumulator_chain ─────────────────────── */
static inline long acc_step1(long x) { return (x * 5 + 3) ^ (x >> 2); }
static inline long acc_step2(long x) { return (x + 7) * 3 - (x >> 1); }
static inline long acc_step3(long x) { return x ^ (x << 1) + (x & 0xff); }
static long bench_accum(long n) {
    long a = 0, b = 0;
    for (long i = 0; i < n; i++) {
        a += acc_step1(i % 10000);
        b += acc_step2(i % 10000);
        a ^= acc_step3(b % 10000);
    }
    return a + b;
}

/* 30 ── sum_of_squares ────────────────────────── */
static long bench_sumsq(long n) {
    long sum = 0;
    for (long i = 1; i < n; i++) {
        sum += i * i;
    }
    return sum;
}

/* 31 ── conditional_accum ─────────────────────── */
static long bench_cond_accum(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        if (i % 3 == 0) {
            sum += i * 2;
        } else if (i % 3 == 1) {
            sum -= i;
        } else {
            sum ^= i;
        }
    }
    return sum;
}

/* 32 ── inline_math_chain ─────────────────────── */
static inline long math_a(long x) { return x * x + x; }
static inline long math_b(long x) { return math_a(x) + math_a(x + 1); }
static inline long math_c(long x) { return math_b(x) - math_b(x - 1); }
static long bench_inline_math(long n) {
    long sum = 0;
    for (long i = 1; i < n; i++) {
        sum += math_c(i % 1000);
    }
    return sum;
}

/* 33 ── bitshift_chain ────────────────────────── */
static long bench_bitshift(long n) {
    long a = 0, b = 0;
    for (long i = 1; i < n; i++) {
        a += (i << 3) - i;
        b += (i << 4) + (i << 1);
        a ^= (b >> 2);
        b += (a & 0xffff);
    }
    return a + b;
}

/* 34 ── switch12 ──────────────────────────────── */
static long bench_switch12(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        switch (i % 12) {
            case 0: sum += i * 2;       break;
            case 1: sum -= i + 3;       break;
            case 2: sum ^= i * 5;       break;
            case 3: sum += (i >> 1);    break;
            case 4: sum -= i ^ 0xff;    break;
            case 5: sum += i * 7;       break;
            case 6: sum ^= (i << 2);    break;
            case 7: sum += i + i;       break;
            case 8: sum -= (i & 1023);  break;
            case 9: sum += i * 11;      break;
            case 10: sum ^= i + 42;     break;
            default: sum += i * 3;
        }
    }
    return sum;
}

/* 35 ── pure_arith_chain ──────────────────────── */
static inline long arith_a(long x) { return x * x - x + 1; }
static inline long arith_b(long x) { return arith_a(x) + arith_a(x + 1) - arith_a(x - 1); }
static long bench_pure_arith(long n) {
    long sum = 0;
    for (long i = 1; i < n; i++) {
        sum += arith_b(i % 5000);
    }
    return sum;
}

/* 36 ── mixed_reduce ──────────────────────────── */
static long bench_mixed_reduce(long n) {
    long sum = 0, xor_acc = 0, prod = 1;
    for (long i = 1; i < n; i++) {
        sum += i;
        xor_acc ^= (i * 3 + 7);
        prod = (prod * 3 + i) & 0x7fffffff;
    }
    return sum + xor_acc + prod;
}

/* 37 ── deep_inline_call ──────────────────────── */
static inline long d1(long x) { return x + 1; }
static inline long d2(long x) { return d1(x) * 2; }
static inline long d3(long x) { return d2(x) + d1(x); }
static inline long d4(long x) { return d3(x) - d2(x); }
static inline long d5(long x) { return d4(x) + d3(x) + d1(x); }
static inline long d6(long x) { return d5(x) ^ d4(x); }
static inline long d7(long x) { return d6(x) + d5(x) - d3(x); }
static inline long d8(long x) { return d7(x) + d6(x); }
static inline long d9(long x) { return d8(x) ^ d7(x) + d5(x); }
static inline long d10(long x) { return d9(x) + d8(x) - d6(x); }
static long bench_deep10(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        sum += d10(i % 10000);
    }
    return sum;
}

/* 38 ── branch_elim ───────────────────────────── */
static inline long classify_switch(long x) {
    switch (x / 10000) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 3;
        case 3: return 4;
        case 4: return 5;
        case 5: return 6;
        case 6: return 7;
        case 7: return 8;
        case 8: return 9;
        case 9: return 10;
        case 10: return 11;
        case 11: return 12;
        case 12: return 13;
        case 13: return 14;
        case 14: return 15;
        case 15: return 16;
        case 16: return 17;
        case 17: return 18;
        case 18: return 19;
        default: return 20;
    }
}
static long bench_branch_elim(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        sum += classify_switch(i % 200000);
        sum += classify_switch((i * 7) % 200000);
    }
    return sum;
}

/* 39 ── switch20 ──────────────────────────────── */
static long bench_switch20(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        switch (i % 20) {
            case 0: sum += i;           break;
            case 1: sum -= i + 1;       break;
            case 2: sum ^= i * 3;      break;
            case 3: sum += i >> 1;      break;
            case 4: sum -= i & 511;     break;
            case 5: sum += i * 5;       break;
            case 6: sum ^= i << 1;     break;
            case 7: sum += i + 7;       break;
            case 8: sum -= i * 2;       break;
            case 9: sum += i ^ 13;     break;
            case 10: sum ^= i * 7;     break;
            case 11: sum += i >> 2;     break;
            case 12: sum -= i + 12;     break;
            case 13: sum += i * 11;     break;
            case 14: sum ^= i & 255;   break;
            case 15: sum += i * 9;      break;
            case 16: sum -= i ^ 7;     break;
            case 17: sum += i << 2;     break;
            case 18: sum ^= i + 42;    break;
            default: sum += i * 13;
        }
    }
    return sum;
}

/* 40 ── triple_inline ─────────────────────────── */
static inline long t1a(long x) { return x * 3 + 1; }
static inline long t1b(long x) { return x * 5 - 2; }
static inline long t1c(long x) { return x ^ (x >> 1); }
static inline long t2a(long x) { return t1a(x) + t1b(x); }
static inline long t2b(long x) { return t1b(x) - t1c(x); }
static inline long t3(long x) { return t2a(x) ^ t2b(x) + t1a(x); }
static long bench_triple_inline(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        sum += t3(i % 10000);
    }
    return sum;
}

/* 41 ── reduction4 ────────────────────────────── */
static long bench_reduce4(long n) {
    long a = 0, b = 0, c = 0, d = 0;
    for (long i = 1; i < n; i++) {
        a += i;
        b += i * i;
        c += i * 3;
        d ^= i;
    }
    return a + b + c + d;
}

/* 42 ── switch_inline_combo ───────────────────── */
static inline long op_add(long a, long b) { return a + b; }
static inline long op_sub(long a, long b) { return a - b; }
static inline long op_xor(long a, long b) { return a ^ b; }
static inline long op_mul2(long a, long b) { return a + b * 2; }
static long bench_switch_inline(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        switch (i & 3) {
            case 0: sum = op_add(sum, i);     break;
            case 1: sum = op_sub(sum, i);     break;
            case 2: sum = op_xor(sum, i);     break;
            default: sum = op_mul2(sum, i);
        }
    }
    return sum;
}

/* 43 ── unrolled_sum ──────────────────────────── */
static long bench_unrolled_sum(long n) {
    long s1 = 0, s2 = 0, s3 = 0, s4 = 0;
    long s5 = 0, s6 = 0, s7 = 0, s8 = 0;
    for (long i = 0; i < n; i++) {
        s1 += i;
        s2 += (i + 1);
        s3 += (i + 2);
        s4 += (i + 3);
        s5 ^= i;
        s6 ^= (i + 1);
        s7 ^= (i + 2);
        s8 ^= (i + 3);
    }
    return s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8;
}

/* 44 ── switch_dispatch4 ──────────────────────── */
static inline long sw4_op0(long x) { return (x * 7 + 3) ^ (x >> 2); }
static inline long sw4_op1(long x) { return (x * 13 - 5) + (x & 0xff); }
static inline long sw4_op2(long x) { return (x << 2) ^ (x * 3 + 11); }
static inline long sw4_op3(long x) { return (x + x * 5) - (x >> 3); }
static long bench_switch4(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        switch (i & 3) {
            case 0: sum += sw4_op0(i); break;
            case 1: sum += sw4_op1(i); break;
            case 2: sum += sw4_op2(i); break;
            default: sum += sw4_op3(i);
        }
    }
    return sum;
}

/* 45 ── parallel_xor ─────────────────────────── */
static long bench_par_xor(long n) {
    long a = 0, b = 0, c = 0, d = 0;
    long e = 0, f = 0, g = 0, h = 0;
    for (long i = 0; i < n; i++) {
        a ^= (i * 7);
        b ^= (i * 13);
        c ^= (i * 19);
        d ^= (i * 31);
        e ^= (i * 37);
        f ^= (i * 43);
        g ^= (i * 53);
        h ^= (i * 61);
    }
    return a + b + c + d + e + f + g + h;
}

/* 46 ── inline_arith6 ─────────────────────────── */
static inline long ia1(long x) { return x * 2 + 1; }
static inline long ia2(long x) { return ia1(x) + ia1(x + 1); }
static inline long ia3(long x) { return ia2(x) * 3 - ia1(x); }
static inline long ia4(long x) { return ia3(x) + ia2(x + 1); }
static inline long ia5(long x) { return ia4(x) ^ ia3(x + 2); }
static inline long ia6(long x) { return ia5(x) + ia4(x) - ia2(x); }
static long bench_inline6(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        sum += ia6(i % 5000);
    }
    return sum;
}

/* 47 ── multi_reduce ──────────────────────────── */
static long bench_multi_reduce(long n) {
    long sum1 = 0, sum2 = 0, sum3 = 0, sum4 = 0;
    for (long i = 1; i < n; i++) {
        sum1 += i * i;
        sum2 += i * i * i;
        sum3 += i * 3 + 1;
        sum4 ^= (i * 7 + 13);
    }
    return sum1 + sum2 + sum3 + sum4;
}

int main(void) {
    int test_id; long n;
    scanf("%d %ld", &test_id, &n);
    long r = 0;
    switch (test_id) {
        case 0:  r = bench_math(n);     break;
        case 1:  r = bench_push(n);     break;
        case 2:  r = bench_hof(n);      break;
        case 3:  r = bench_strcat(n);   break;
        case 4:  r = bench_strops(n);   break;
        case 5:  r = bench_struct(n);   break;
        case 6:  r = bench_branch(n);   break;
        case 7:  r = bench_recurse(n);  break;
        case 8:  r = bench_nested(n);   break;
        case 9:  r = bench_sort(n);     break;
        case 10: r = bench_while(n);    break;
        case 11: r = bench_ifelse(n);   break;
        case 12: r = bench_arrindex(n); break;
        case 13: r = bench_calls(n);    break;
        case 14: r = bench_bitwise(n);       break;
        case 15: r = bench_combined(n);      break;
        case 16: r = bench_floatmath(n);     break;
        case 17: r = bench_bitintrinsics(n); break;
        case 18: r = bench_reduction(n);    break;
        case 19: r = bench_modular(n);      break;
        case 20: r = bench_arithmetic(n);   break;
        case 21: r = bench_poly(n);          break;
        case 22: r = bench_branch8(n);       break;
        case 23: r = bench_cascade(n);       break;
        case 24: r = bench_strength(n);      break;
        case 25: r = bench_deep(n);          break;
        case 26: r = bench_bitcount(n);      break;
        case 27: r = bench_xor_reduce(n);    break;
        case 28: r = bench_multi_switch(n);  break;
        case 29: r = bench_accum(n);         break;
        case 30: r = bench_sumsq(n);         break;
        case 31: r = bench_cond_accum(n);    break;
        case 32: r = bench_inline_math(n);   break;
        case 33: r = bench_bitshift(n);      break;
        case 34: r = bench_switch12(n);    break;
        case 35: r = bench_pure_arith(n);  break;
        case 36: r = bench_mixed_reduce(n);break;
        case 37: r = bench_deep10(n);      break;
        case 38: r = bench_branch_elim(n); break;
        case 39: r = bench_switch20(n);    break;
        case 40: r = bench_triple_inline(n);break;
        case 41: r = bench_reduce4(n);     break;
        case 42: r = bench_switch_inline(n);break;
        case 43: r = bench_unrolled_sum(n);break;
        case 44: r = bench_switch4(n);     break;
        case 45: r = bench_par_xor(n);     break;
        case 46: r = bench_inline6(n);     break;
        case 47: r = bench_multi_reduce(n);break;
    }
    printf("%ld\n", r);
    return 0;
}
CEOF

# ─── COMPILE ──────────────────────────────────────────────────
# Locate the omsc compiler: prefer the freshly-built binary in ./build/,
# then fall back to whatever is on PATH (consistent with run_tests.sh).
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

# All max-optimization flags are enabled.  OPTMAX is set inside bench.om.
OM_FLAGS="-O3 -flto -march=native -mtune=native -ffast-math -fvectorize -funroll-loops -floop-optimize"
C_FLAGS="-O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -lm"

echo "Compiling OM ($OMSC $OM_FLAGS) …"
OM_COMP_START=$(date +%s%N)
"$OMSC" bench.om $OM_FLAGS -o bench_om
OM_COMP_END=$(date +%s%N)
OM_COMP_MS=$(( (OM_COMP_END - OM_COMP_START) / 1000000 ))
echo "  OM compile time: ${OM_COMP_MS} ms"

echo "Compiling C  (gcc $C_FLAGS) …"
C_COMP_START=$(date +%s%N)
gcc bench.c $C_FLAGS -o bench_c
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

# run_one  <id> <n> <name>
# Runs $RUNS iterations, keeps the median time.
run_one() {
    local id=$1 n=$2 name=$3

    # correctness check (single run) — also serves as warmup for I-cache / D-cache
    local co oo
    co=$(echo "$id $n" | ./bench_c)
    oo=$(echo "$id $n" | ./bench_om)
    if [ "$co" != "$oo" ]; then
        printf "  %-22s  C=%-14s  OM=%-14s  ${RED}❌ MISMATCH${RST}\n" "$name" "$co" "$oo"
        MISMATCH=1
        RATIOS[$id]=0
        C_TIMES[$id]=0
        OM_TIMES[$id]=0
        return
    fi

    # timed runs
    local c_runs=() om_runs=()
    for (( r=0; r<RUNS; r++ )); do
        local cs ce ct os oe ot
        cs=$(date +%s%N)
        echo "$id $n" | ./bench_c > /dev/null
        ce=$(date +%s%N)
        ct=$(( (ce - cs) / 1000000 ))
        c_runs+=("$ct")

        os=$(date +%s%N)
        echo "$id $n" | ./bench_om > /dev/null
        oe=$(date +%s%N)
        ot=$(( (oe - os) / 1000000 ))
        om_runs+=("$ot")
    done

    # sort and pick median
    IFS=$'\n' c_sorted=($(printf '%s\n' "${c_runs[@]}" | sort -n)); unset IFS
    IFS=$'\n' om_sorted=($(printf '%s\n' "${om_runs[@]}" | sort -n)); unset IFS
    local mid=$(( RUNS / 2 ))
    local ct=${c_sorted[$mid]}
    local ot=${om_sorted[$mid]}

    C_TIMES[$id]=$ct
    OM_TIMES[$id]=$ot

    # ratio
    local ratio
    if [ "$ct" -eq 0 ]; then
        if [ "$ot" -le 1 ]; then ratio=100; else ratio=$(( ot * 1000 )); fi
    else
        ratio=$(( ot * 100 / ct ))
    fi
    RATIOS[$id]=$ratio

    local tag
    if   [ "$ratio" -le 120 ]; then tag="${GRN}✅ competitive${RST}"
    elif [ "$ratio" -le 250 ]; then tag="${YEL}⚠️  slower${RST}"
    else                            tag="${RED}❌ bottleneck${RST}"
    fi

    printf "  %-22s  C: %6d ms   OM: %6d ms   %4d%%   %b\n" \
           "$name" "$ct" "$ot" "$ratio" "$tag"
}

# ─── RUN ──────────────────────────────────────────────────────
echo "╔═══════════════════════════════════════════════════════════════════════════╗"
echo "║              Per-Function Benchmarks  (median of $RUNS runs)               ║"
echo "╚═══════════════════════════════════════════════════════════════════════════╝"
echo ""
printf "  ${BLD}%-22s  %-15s %-15s %-7s  %-12s${RST}\n" \
       "BENCHMARK" "C TIME" "OM TIME" "RATIO" "STATUS"
printf "  %-22s  %-15s %-15s %-7s  %-12s\n" \
       "─────────────────────" "──────────────" "──────────────" "──────" "──────────"

for (( id=0; id<NUM_BENCHMARKS; id++ )); do
    run_one "$id" "${BENCH_N[$id]}" "${BENCH_NAME[$id]}"
done

if [ "$MISMATCH" -eq 1 ]; then
    echo ""
    echo -e "${RED}ERROR: Output mismatch detected – fix correctness issues before benchmarking.${RST}"
    exit 1
fi

# ─── SCALING ──────────────────────────────────────────────────
echo ""
echo "╔═══════════════════════════════════════════════════════════════════════════╗"
echo "║                    Loop Scaling  (x1  x2  x4)                           ║"
echo "╚═══════════════════════════════════════════════════════════════════════════╝"
echo ""
echo "  Tests whether OM runtime scales linearly with input size, like C."
echo ""

SCALE_IDS=(0 5 6 10 14)
SCALE_NAMES=("integer_math" "struct_access" "switch_branch" "while_loop" "bitwise_ops")
SCALE_BASE=(250000 2000000 2000000 2000000 2000000)

for si in "${!SCALE_IDS[@]}"; do
    sid=${SCALE_IDS[$si]}
    sname=${SCALE_NAMES[$si]}
    sbase=${SCALE_BASE[$si]}

    printf "  %-18s  " "$sname"
    for mult in 1 2 4; do
        sn=$(( sbase * mult ))

        cs=$(date +%s%N)
        echo "$sid $sn" | ./bench_c > /dev/null
        ce=$(date +%s%N)
        ct=$(( (ce - cs) / 1000000 ))

        os=$(date +%s%N)
        echo "$sid $sn" | ./bench_om > /dev/null
        oe=$(date +%s%N)
        ot=$(( (oe - os) / 1000000 ))

        if [ "$ct" -gt 0 ]; then
            ratio=$(( ot * 100 / ct ))
        else
            ratio=100
        fi

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
        printf "    what it tests: %s\n" "${BENCH_DESC[$id]}"
        printf "    input size:    N=%-8s  C: %d ms   OM: %d ms\n\n" \
               "${BENCH_N[$id]}" "${C_TIMES[$id]}" "${OM_TIMES[$id]}"
    done
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

# ─── OVERALL SCORE ────────────────────────────────────────────
SUM_C=0; SUM_OM=0
for (( id=0; id<NUM_BENCHMARKS; id++ )); do
    SUM_C=$(( SUM_C + C_TIMES[$id] ))
    SUM_OM=$(( SUM_OM + OM_TIMES[$id] ))
done
if [ "$SUM_C" -gt 0 ]; then
    OVERALL=$(( SUM_OM * 100 / SUM_C ))
else
    OVERALL=100
fi

# Geometric mean of per-benchmark ratios (more meaningful than arithmetic
# average because it treats all benchmarks equally regardless of absolute
# time, and it correctly handles ratios: geomean of {2x, 0.5x} = 1.0x).
# Uses awk for floating-point log/exp.
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
printf "  Aggregate:  C: %d ms   OM: %d ms   (%d%%)\n" \
       "$SUM_C" "$SUM_OM" "$OVERALL"
printf "  Geometric mean of per-benchmark ratios: %d%%\n" "$GEOMEAN"
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
if   [ "$GEOMEAN" -le 120 ]; then echo -e "  Verdict:    ${GRN}${BLD}Excellent – near-native performance.${RST}"
elif [ "$GEOMEAN" -le 200 ]; then echo -e "  Verdict:    ${YEL}${BLD}Acceptable – room for improvement.${RST}"
else                               echo -e "  Verdict:    ${RED}${BLD}Needs work – significant overhead detected.${RST}"
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
elif [ "$BENCH_MS" -gt $(( TOTAL_MS * 80 / 100 )) ]; then
    echo -e "  ${GRN}→ Most time was spent running benchmarks (expected).${RST}"
    echo -e "  ${GRN}  Compilation is fast; benchmark input sizes drive total time.${RST}"
fi
echo "────────────────────────────────────────────────────────────────"

# ─── CLEANUP ──────────────────────────────────────────────────
rm -f bench.om bench.c bench_om bench_c
