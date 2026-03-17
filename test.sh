#!/usr/bin/env bash
set -e

ITERATIONS=7
N=100000000
CPU_CORE=0

echo "=== Generating source files ==="

# ---- C version (64-bit) ----
cat > stress.c << 'EOF'
#include <stdint.h>
#include <stdio.h>

uint64_t stress(uint64_t n) {
    uint64_t acc = 0;
    uint64_t x = 1;

    for (uint64_t i = 0; i < n; i++) {
        x = x * 1664525ull + 1013904223ull;

        uint64_t a = x ^ (x >> 13);
        uint64_t b = a * 1274126177ull;
        uint64_t c = (b >> 16) + (b << 3);

        acc += (c ^ (acc >> 7));
        acc += (i & 1) ? (a >> 3) : (b << 1);
    }

    return acc;
}

int main() {
    uint64_t r = stress(100000000);
    printf("%llu\n", (unsigned long long)r);
}
EOF

# ---- OmScript version ----
cat > stress.om << 'EOF'
fn stress(n) {
    var acc = 0;
    var x = 1;

    for (i in 0...n) {
        x = x * 1664525 + 1013904223;

        var a = x ^ (x >> 13);
        var b = a * 1274126177;
        var c = (b >> 16) + (b << 3);

        acc += (c ^ (acc >> 7));
        acc += (i & 1) ? (a >> 3) : (b << 1);
    }

    return acc;
}

fn main() {
    var r = stress(100000000);
    print(r);
    return 0;
}
EOF

echo "=== Compiling ==="

clang -O3 -march=native -flto -funroll-loops stress.c -o stress_c
omsc build stress.om -O3 -march=native -o stress_om

echo "=== CPU Info ==="
lscpu | grep "Model name"

echo "=== Warmup ==="
taskset -c $CPU_CORE ./stress_c > /dev/null
taskset -c $CPU_CORE ./stress_om > /dev/null

echo "=== Verifying correctness ==="
C_OUT=$(taskset -c $CPU_CORE ./stress_c)
OM_OUT=$(taskset -c $CPU_CORE ./stress_om)

echo "C output:  $C_OUT"
echo "OM output: $OM_OUT"

if [ "$C_OUT" != "$OM_OUT" ]; then
    echo "❌ Mismatch! Benchmark invalid."
    exit 1
fi

echo "✅ Outputs match"

# ---- timing helper ----
measure_times() {
    local exe=$1
    local times=()

    for i in $(seq 1 $ITERATIONS); do
        t=$(taskset -c $CPU_CORE /usr/bin/time -f "%e" ./$exe 2>&1 > /dev/null)
        times+=($t)
    done

    printf "%s\n" "${times[@]}" | sort -n | awk '
    {
        a[NR]=$1
    }
    END {
        mid=int(NR/2)+1
        print a[mid]
    }'
}

echo "=== Running benchmark ($ITERATIONS runs, median) ==="

C_TIME=$(measure_times stress_c)
OM_TIME=$(measure_times stress_om)

echo ""
echo "===== TIMING ====="
echo "C (clang):     $C_TIME sec"
echo "OmScript:      $OM_TIME sec"

ratio=$(awk "BEGIN {print $OM_TIME / $C_TIME}")
echo "Ratio (OM/C):  $ratio x"

echo ""
echo "=== perf stats (single run) ==="

echo "--- C ---"
taskset -c $CPU_CORE perf stat -e cycles,instructions,branches,branch-misses ./stress_c

echo "--- OmScript ---"
taskset -c $CPU_CORE perf stat -e cycles,instructions,branches,branch-misses ./stress_om
