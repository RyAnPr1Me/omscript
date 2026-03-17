#!/bin/bash

set -e

echo "=== Generating source files ==="

# ---------------- OM SCRIPT ----------------
cat > bench.om << 'EOF'
OPTMAX=:

struct Point { x, y }

fn heavy_math(n:int) {
    var acc:int = 0;
    for (i:int in 1...n) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
        acc += log2(i);
    }
    return acc;
}

fn array_work(n:int) -> int {
    var arr:int[] = [0];
    for (i:int in 0...n) {
        arr = push(arr, (i * 3) % 12345);
    }

    var mapped:int[] = array_map(arr, |x:int| (x * x) % 1000);
    var filtered:int[] = array_filter(mapped, |x:int| x % 2 == 0);
    var reduced:int = array_reduce(filtered, |a:int, b:int| a + b, 0);

    return reduced + len(filtered);
}

fn string_work(n:int) -> int {
    var s:str = "bench";
    for (i:int in 0...n) {
        s = str_concat(s, to_string(i % 10));
    }
    return str_len(s);
}

fn struct_work(n:int) -> int {
    var p:ptr = Point { x: 1, y: 2 };
    var sum:int = 0;
    for (i:int in 0...n) {
        p.x += i;
        p.y ^= i;
        sum += p.x + p.y;
    }
    return sum;
}

fn branching(n:int) -> int {
    var sum:int = 0;
    for (i:int in 0...n) {
        switch (i % 4) {
            case 0: sum += i; break;
            case 1: sum -= i; break;
            case 2: sum ^= i; break;
            default: sum += (i * 2);
        }
    }
    return sum;
}

fn edge_cases() -> int {
    assert(gcd(0, 5) == 5);
    assert(log2(1) == 0);
    assert(is_even(4) == 1);
    assert(is_odd(5) == 1);
    assert(clamp(10, 0, 5) == 5);
    return 1;
}

fn main() -> int {
    var n:int = input();

    var total:int =
        heavy_math(n) +
        array_work(n / 10) +
        string_work(n / 50) +
        struct_work(n) +
        branching(n) +
        edge_cases();

    print(total);
    return 0;
}

OPTMAX!:
EOF

# ---------------- C ----------------
cat > bench.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>

long gcd(long a, long b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        long t = b;
        b = a % b;
        a = t;
    }
    return a;
}

long log2_int(long n) {
    if (n <= 0) return -1;
    long r = 0;
    while (n >>= 1) r++;
    return r;
}

long heavy_math(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
        acc += log2_int(i);
    }
    return acc;
}

long array_work(long n) {
    long *arr = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) {
        arr[i] = (i * 3) % 12345;
    }

    long reduced = 0, count = 0;

    for (long i = 0; i < n; i++) {
        long x = (arr[i] * arr[i]) % 1000;
        if (x % 2 == 0) {
            reduced += x;
            count++;
        }
    }

    free(arr);
    return reduced + count;
}

long string_work(long n) {
    long len = 5;
    for (long i = 0; i < n; i++) len++;
    return len;
}

long struct_work(long n) {
    long x = 1, y = 2;
    long sum = 0;
    for (long i = 0; i < n; i++) {
        x += i;
        y ^= i;
        sum += x + y;
    }
    return sum;
}

long branching(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        switch (i % 4) {
            case 0: sum += i; break;
            case 1: sum -= i; break;
            case 2: sum ^= i; break;
            default: sum += (i * 2);
        }
    }
    return sum;
}

int main() {
    long n;
    scanf("%ld", &n);

    long total =
        heavy_math(n) +
        array_work(n / 10) +
        string_work(n / 50) +
        struct_work(n) +
        branching(n) +
        1;

    printf("%ld\n", total);
    return 0;
}
EOF

# ---------------- COMPILE ----------------
echo "=== Compiling (MAX OPT) ==="

omsc bench.om -O3 -march=native -o bench_om
gcc bench.c -O3 -march=native -funroll-loops -flto -o bench_c

# ---------------- TESTS ----------------
echo "=== Running Benchmarks ==="

TESTS=(10000 50000 100000 200000)

for N in "${TESTS[@]}"; do
    echo ""
    echo "---- Input: $N ----"

    C_START=$(date +%s%N)
    C_OUT=$(echo $N | ./bench_c)
    C_END=$(date +%s%N)

    OM_START=$(date +%s%N)
    OM_OUT=$(echo $N | ./bench_om)
    OM_END=$(date +%s%N)

    C_TIME=$(( (C_END - C_START)/1000000 ))
    OM_TIME=$(( (OM_END - OM_START)/1000000 ))

    echo "C:  $C_OUT (${C_TIME} ms)"
    echo "OM: $OM_OUT (${OM_TIME} ms)"

    if [ "$C_OUT" != "$OM_OUT" ]; then
        echo "❌ OUTPUT MISMATCH"
        exit 1
    fi

    if [ $OM_TIME -gt $C_TIME ]; then
        RATIO=$((OM_TIME * 100 / (C_TIME + 1)))
        echo "⚠️ OM slower (${RATIO}% of C)"

        if [ $RATIO -gt 150 ]; then
            echo "Reason:"
            echo "- array_map/filter/reduce overhead"
            echo "- lambda not fully inlined"
        fi

        if [ $RATIO -gt 250 ]; then
            echo "Severe reason:"
            echo "- string concatenation allocations"
            echo "- bounds checks"
            echo "- missed vectorization"
        fi
    else
        echo "🚀 OM is as fast or faster"
    fi
done

echo ""
echo "=== DONE ==="
