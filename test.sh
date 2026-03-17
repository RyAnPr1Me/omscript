#!/usr/bin/env bash
set -e

CPU=0
ITER=5

echo "=== Generating sources ==="

# =========================
# C VERSION
# =========================
cat > bench.c << 'EOF'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

uint64_t stress_alu(uint64_t n, uint64_t seed) {
    uint64_t acc = seed;
    uint64_t x = seed | 1;

    for (uint64_t i = 0; i < n; i++) {
        x = x * 1664525ULL + 1013904223ULL;

        uint64_t a = mix(x);
        uint64_t b = a * (i + 1);
        uint64_t c = (b >> 17) ^ (b << 5);

        acc += (c ^ (acc >> 11));

        if (i & 1)
            acc ^= (a >> 3);
        else
            acc += (b << 1);

        if ((acc & 0xFF) == 123)
            acc ^= i;
    }

    return acc;
}

uint64_t stress_branch(uint64_t n, uint64_t seed) {
    uint64_t acc = seed;
    for (uint64_t i = 0; i < n; i++) {
        if ((i ^ seed) & 1)
            acc += i * 3;
        else
            acc ^= (i << 2);
    }
    return acc;
}

uint64_t stress_mem(uint64_t n) {
    uint64_t arr[1024];
    for (int i = 0; i < 1024; i++)
        arr[i] = i;

    uint64_t acc = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t idx = i & 1023;
        arr[idx] = arr[idx] * 1664525 + 1013904223;
        acc ^= arr[idx];
    }
    return acc;
}

int main(int argc, char** argv) {
    uint64_t n = (argc > 1) ? strtoull(argv[1], NULL, 10) : 10000000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 10) : 12345;

    uint64_t r1 = stress_alu(n, seed);
    uint64_t r2 = stress_branch(n, seed);
    uint64_t r3 = stress_mem(n);

    printf("%llu %llu %llu\n",
        (unsigned long long)r1,
        (unsigned long long)r2,
        (unsigned long long)r3);
}
EOF

# =========================
# OMSCRIPT VERSION
# =========================
cat > bench.om << 'EOF'
fn mix(x) {
    x = x ^ (x >> 33);
    x = x * 0xff51afd7ed558ccd;
    x = x ^ (x >> 33);
    x = x * 0xc4ceb9fe1a85ec53;
    x = x ^ (x >> 33);
    return x;
}

fn stress_alu(n, seed) {
    var acc = seed;
    var x = seed | 1;

    for (i in 0...n) {
        x = x * 1664525 + 1013904223;

        var a = mix(x);
        var b = a * (i + 1);
        var c = (b >> 17) ^ (b << 5);

        acc += (c ^ (acc >> 11));

        if (i & 1) {
            acc ^= (a >> 3);
        } else {
            acc += (b << 1);
        }

        if ((acc & 0xFF) == 123) {
            acc ^= i;
        }
    }

    return acc;
}

fn stress_branch(n, seed) {
    var acc = seed;
    for (i in 0...n) {
        if ((i ^ seed) & 1) {
            acc += i * 3;
        } else {
            acc ^= (i << 2);
        }
    }
    return acc;
}

fn stress_mem(n) {
    var arr = [0; 1024];
    for (i in 0...1024) {
        arr[i] = i;
    }

    var acc = 0;
    for (i in 0...n) {
        var idx = i & 1023;
        arr[idx] = arr[idx] * 1664525 + 1013904223;
        acc ^= arr[idx];
    }
    return acc;
}

fn main() {
    var n = 10000000;
    var seed = 12345;

    var r1 = stress_alu(n, seed);
    var r2 = stress_branch(n, seed);
    var r3 = stress_mem(n);

    print(r1);
    print(r2);
    print(r3);
    return 0;
}
EOF

echo "=== Compiling ==="

clang -O3 -march=znver4 -mtune=znver4 -flto -funroll-loops bench.c -o bench_c
omsc build bench.om -o bench_om  -O3 -march=znver4  -mtune=znver4 -flto -funroll-loops -fvectorize -ffast-math -fno-jit

echo "=== Correctness Tests ==="

C_OUT=$(./bench_c)
OM_OUT=$(./bench_om | tr '\n' ' ')

echo "C:  $C_OUT"
echo "OM: $OM_OUT"

if [ "$C_OUT" != "$OM_OUT" ]; then
    echo "❌ MISMATCH — aborting benchmark"
    exit 1
fi

echo "✅ Correctness passed"

measure() {
    exe=$1
    total=0
    for i in $(seq 1 $ITER); do
        t=$(taskset -c $CPU /usr/bin/time -f "%e" ./$exe 2>&1 > /dev/null)
        total=$(awk "BEGIN {print $total + $t}")
    done
    awk "BEGIN {print $total / $ITER}"
}

echo "=== Timing ==="

C_TIME=$(measure bench_c)
OM_TIME=$(measure bench_om)

echo "C:        $C_TIME sec"
echo "OmScript: $OM_TIME sec"

RATIO=$(awk "BEGIN {print $OM_TIME / $C_TIME}")
echo "Ratio: $RATIO x"

# =========================
# ONLY ANALYZE IF SLOWER
# =========================
if (( $(echo "$OM_TIME > $C_TIME" | bc -l) )); then
    echo ""
    echo "=== OmScript is slower → running diagnostics ==="

    perf stat -e cycles,instructions,branches,branch-misses ./bench_c 2> perf_c.txt
    perf stat -e cycles,instructions,branches,branch-misses ./bench_om 2> perf_om.txt

    get() {
        grep "$2" $1 | awk '{print $1}' | tr -d ','
    }

    C_cycles=$(get perf_c.txt cycles)
    OM_cycles=$(get perf_om.txt cycles)

    C_instr=$(get perf_c.txt instructions)
    OM_instr=$(get perf_om.txt instructions)

    C_ipc=$(awk "BEGIN {print $C_instr / $C_cycles}")
    OM_ipc=$(awk "BEGIN {print $OM_instr / $OM_cycles}")

    echo ""
    echo "=== Analysis ==="

    echo "C IPC:  $C_ipc"
    echo "OM IPC: $OM_ipc"

    if (( $(echo "$OM_instr > $C_instr * 1.2" | bc -l) )); then
        echo "⚠️ More instructions → missed optimizations"
    fi

    if (( $(echo "$OM_ipc < $C_ipc * 0.8" | bc -l) )); then
        echo "⚠️ Lower IPC → poor scheduling / dependency chains"
    fi

    C_miss=$(get perf_c.txt branch-misses)
    OM_miss=$(get perf_om.txt branch-misses)

    if (( $(echo "$OM_miss > $C_miss * 1.5" | bc -l) )); then
        echo "⚠️ More branch misses → bad control flow lowering"
    fi

    echo ""
    echo "=== Hotspots ==="

    echo "--- C ---"
    perf record -F 99 -g ./bench_c > /dev/null 2>&1
    perf report --stdio | head -15

    echo "--- OmScript ---"
    perf record -F 99 -g ./bench_om > /dev/null 2>&1
    perf report --stdio | head -15
fi

echo ""
echo "=== Done ==="
