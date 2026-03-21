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

NUM_BENCHMARKS=16

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
)

BENCH_DESC=(
    "GCD, log2, modular arithmetic in a tight loop"
    "Push N elements onto a dynamic array"
    "array_map / array_filter / array_reduce with lambdas"
    "Repeated str_concat to build a long string"
    "str_contains + str_index_of on a 1000-char haystack"
    "Struct field read/write in a tight loop"
    "Switch/case with 4 branches inside a loop"
    "Naive recursive Fibonacci (exponential calls)"
    "Triple-nested loop (N^3 iterations)"
    "Bubble-sort a pseudo-random array"
    "While-loop accumulator (vs for-in)"
    "Cascading if/else classification"
    "Pseudo-random array read + write access pattern"
    "Deeply nested small-function calls"
    "Shift, AND, OR, XOR intensive loop"
    "End-to-end workload exercising all subsystems"
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
)

BOTTLENECK_LABELS=(
    "math builtins (gcd / log2 call overhead)"
    "array reallocation strategy on push"
    "lambda / higher-order function overhead"
    "string allocation on every concat"
    "string search / manipulation builtins"
    "struct field-access indirection"
    "switch codegen / branch prediction"
    "function-call overhead (deep recursion)"
    "loop body codegen / vectorisation"
    "sort algorithm (bubble sort is O(n^2))"
    "while-loop codegen quality"
    "branch codegen (if/else chains)"
    "array bounds-check overhead"
    "call / return overhead for small functions"
    "bitwise-op codegen quality"
    "overall compiler performance"
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
OPTMAX=:

struct Point { hot int x, hot int y }

@hot 
fn bench_math(n:int) -> int {
   prefetch hot var acc:int = 0;
    for (i:int in 1...n) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
        acc += log2(i);
    }
    return acc;
}

fn bench_push(@prefetch n:int) -> int {
    prefetch var arr:int[] align(64) = [];
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
    prefetch hot var arr:int[] align(64) = array_fill(n, 0);
    for (i:int in 0...n) {
        arr[i] = (i * 7) % 1000;
    }
    var mapped:int[] = array_map(arr, |x:int| (x * x) % 997);
    invalidate arr;
    var filtered:int[] align(64) = array_filter(mapped, |x:int| x % 2 == 0);
    invalidate mapped;
    var reduced:int align(64) = array_reduce(filtered, |a:int, b:int| a + b, 0);
    var result:int align(64) = reduced + len(filtered);
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
@hot
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
@hot
fn fib(n:int) -> int {
    if (n <= 1) { return n; }
    return fib(n - 1) + fib(n - 2);
}
fn bench_recurse(n:int) -> int {
    return fib(n);
}
@hot @flatten @pure @unroll
fn bench_nested(n:int) -> int {
    hot var sum:int = 0;
    for (i:int in 0...n:int) {
        for (j:int in 0...n:int) {
            for (k:int in 0...n:int) {
                 sum += ((i ^ j) + k) % 37;
            }
        }
    }
    return sum;
}
@hot
fn bench_sort(n:int) -> int {
    var arr:int[] = [];
    for (i:int in 0...n:int) {
        arr = push(arr, (i * 2654435761) % 1000000);
    }
    sort(arr);
    var result:int = arr[0] + arr[n / 2] + arr[n - 1];
    invalidate arr;
    return result;
}
@hot
fn bench_while(n:int) -> int {
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
@hot
fn bench_ifelse(n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        sum += classify(i % 200000);
    }
    return sum;
}
@hot
fn bench_arrindex(n:int) -> int {
    var sz:int = 10000;
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

fn add_one(x:int) -> int { return x + 1; }
fn add_two(x:int) -> int { return add_one(add_one(x)); }
fn add_four(x:int) -> int { return add_two(add_two(x)); }
fn bench_calls(n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n:int) {
        sum += add_four(i % 1000);
    }
    invalidate n;
    return sum;
    
}
@hot
fn bench_bitwise(n:int) -> int {
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
@hot
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
    total += array_reduce(filtered, |a:int, b:int| a + b, 0) + len(filtered);
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
                total += ((i ^ j) + k) % 37;
            }
        }
    }

    return total;
}
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
        default: print(0);
    }
     invalidate n;
    return 0;
   
}

OPTMAX!:
OMEOF

# ─── C SOURCE ────────────────────────────────────────────────
cat > bench.c << 'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                sum += ((i ^ j) + k) % 37;
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
            total += ((i ^ j) + k) % 37;
    }

    return total;
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
        case 14: r = bench_bitwise(n);  break;
        case 15: r = bench_combined(n); break;
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
echo "  DEBUG: Compilation Timing"
echo "────────────────────────────────────────────────────────────────"

# All max-optimization flags are enabled.  OPTMAX is set inside bench.om.
OM_FLAGS="-O3 -flto -march=native -mtune=native -ffast-math -fvectorize -funroll-loops -floop-optimize"
C_FLAGS="-O3 -march=native -mtune=native -flto -ffast-math -funroll-loops"

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
    echo -e "  ${RED}⚠ OM compilation took >10 s – compiler may be generating slow code paths${RST}"
elif [ "$OM_COMP_MS" -gt 5000 ]; then
    echo -e "  ${YEL}⚠ OM compilation took >5 s – consider profiling the compiler${RST}"
else
    echo -e "  ${GRN}✓ OM compilation completed in a reasonable time${RST}"
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

    # correctness check (single run)
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
    echo -e "${RED}ERROR: output mismatch – fix correctness before benchmarking.${RST}"
    exit 1
fi

# ─── SCALING ──────────────────────────────────────────────────
echo ""
echo "╔═══════════════════════════════════════════════════════════════════════════╗"
echo "║                    Loop Scaling  (x1  x2  x4)                           ║"
echo "╚═══════════════════════════════════════════════════════════════════════════╝"
echo ""
echo "  Tests whether OM scales linearly like C as N grows."
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

echo "==================================================================="
printf "  Aggregate:  C: %d ms   OM: %d ms   (%d%%)\n" \
       "$SUM_C" "$SUM_OM" "$OVERALL"

if [ "$OVERALL" -lt 100 ]; then
    DIFF=$(( 100 - OVERALL ))
    echo -e "  ${GRN}${BLD}OM is ${DIFF}% faster than C on average.${RST}"
elif [ "$OVERALL" -gt 100 ]; then
    DIFF=$(( OVERALL - 100 ))
    echo -e "  ${RED}${BLD}OM is ${DIFF}% slower than C on average.${RST}"
else
    echo -e "  ${GRN}${BLD}OM is on par with C on average.${RST}"
fi

echo ""
if   [ "$OVERALL" -le 120 ]; then echo -e "  Verdict:    ${GRN}${BLD}Excellent -- near-native performance${RST}"
elif [ "$OVERALL" -le 200 ]; then echo -e "  Verdict:    ${YEL}${BLD}Acceptable -- room for improvement${RST}"
else                               echo -e "  Verdict:    ${RED}${BLD}Needs work -- significant overhead${RST}"
fi

echo ""
echo "=== DONE ==="

# ─── DEBUG: TIME BREAKDOWN ────────────────────────────────────
SCRIPT_END=$(date +%s%N)
TOTAL_MS=$(( (SCRIPT_END - SCRIPT_START) / 1000000 ))
BENCH_MS=$(( TOTAL_MS - OM_COMP_MS - C_COMP_MS ))
echo ""
echo "────────────────────────────────────────────────────────────────"
echo "  DEBUG: Time Breakdown"
echo "────────────────────────────────────────────────────────────────"
printf "  OM compile:     %6d ms  (%d%%)\n" "$OM_COMP_MS" "$(( OM_COMP_MS * 100 / (TOTAL_MS > 0 ? TOTAL_MS : 1) ))"
printf "  C  compile:     %6d ms  (%d%%)\n" "$C_COMP_MS"  "$(( C_COMP_MS  * 100 / (TOTAL_MS > 0 ? TOTAL_MS : 1) ))"
printf "  Benchmarks:     %6d ms  (%d%%)\n" "$BENCH_MS"   "$(( BENCH_MS   * 100 / (TOTAL_MS > 0 ? TOTAL_MS : 1) ))"
printf "  Total:          %6d ms\n" "$TOTAL_MS"
echo ""
if [ "$OM_COMP_MS" -gt "$BENCH_MS" ]; then
    echo -e "  ${YEL}→ Most time was spent in the OM COMPILER, not running benchmarks.${RST}"
    echo -e "  ${YEL}  The compiler itself is the bottleneck.${RST}"
elif [ "$BENCH_MS" -gt $(( TOTAL_MS * 80 / 100 )) ]; then
    echo -e "  ${GRN}→ Most time was spent RUNNING benchmarks (expected).${RST}"
    echo -e "  ${GRN}  Compilation is fast; benchmark sizes drive total time.${RST}"
fi
echo "────────────────────────────────────────────────────────────────"
