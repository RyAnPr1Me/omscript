#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# OmScript E-Graph & Superoptimizer Benchmark Suite
# ─────────────────────────────────────────────────────────────────────────────
#
# Compares OmScript compiler output at different optimization levels:
#   - O0: No optimization (baseline)
#   - O2: Standard optimization + e-graph + superoptimizer
#   - O3: Aggressive optimization + e-graph + superoptimizer
#   - O2 without e-graph/superopt: Standard LLVM pipeline only
#
# Also compares against C compiled with gcc -O3 where applicable.
#
# Usage: bash run_egraph_benchmark.sh [--quick]

set -uo pipefail

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BENCH_DIR="/tmp/omsc_egraph_bench"
RUNS=3  # Number of runs per benchmark (take median)

# Quick mode: fewer runs
if [[ "${1:-}" == "--quick" ]]; then
    RUNS=1
fi

echo -e "${BOLD}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║  OmScript E-Graph & Superoptimizer Benchmark Suite         ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""

# ── Build compiler ────────────────────────────────────────────────────────────
echo -e "${CYAN}Building compiler...${NC}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
make -j$(nproc) omsc > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Compiler built${NC}"
OMSC="$BUILD_DIR/omsc"

mkdir -p "$BENCH_DIR"

# ── Benchmark programs ────────────────────────────────────────────────────────
# Each benchmark is an OmScript program that returns its result as exit code.

# Fibonacci (recursive) — tests function call overhead
cat > "$BENCH_DIR/fib.om" << 'EOF'
fn fib(n) {
    if (n <= 1) { return n; }
    return fib(n - 1) + fib(n - 2);
}
fn main() {
    return fib(30) % 256;
}
EOF

# Sum of squares — tests loop + arithmetic optimization
cat > "$BENCH_DIR/sumsq.om" << 'EOF'
fn sum_squares(n: int) {
    var total: int = 0;
    for (i: int in 0...n) {
        total = total + i * i;
    }
    return total;
}
fn main() {
    return sum_squares(10000) % 256;
}
EOF

# Constant folding stress — tests e-graph constant propagation
cat > "$BENCH_DIR/constfold.om" << 'EOF'
fn compute() {
    var x: int = 0;
    for (i: int in 0...1000) {
        x = x + (3 * 4 + 5 * 6 - 2 * 1 + 7 * 8 - 3 * 3);
    }
    return x % 256;
}
fn main() {
    return compute();
}
EOF

# Bitwise operations — tests bitwise optimization rules
cat > "$BENCH_DIR/bitwise.om" << 'EOF'
fn bitcount(n: int) {
    var count: int = 0;
    var x: int = n;
    while (x != 0) {
        count = count + (x & 1);
        x = x >> 1;
    }
    return count;
}
fn main() {
    var total: int = 0;
    for (i: int in 0...10000) {
        total = total + bitcount(i);
    }
    return total % 256;
}
EOF

# Strength reduction — tests multiply-to-shift optimization
cat > "$BENCH_DIR/strength.om" << 'EOF'
fn compute(n: int) {
    var total: int = 0;
    for (i: int in 0...n) {
        total = total + i * 4 + i * 8 + i * 16 + i * 32;
    }
    return total;
}
fn main() {
    return compute(10000) % 256;
}
EOF

# Algebraic identity stress — tests x+0, x*1, x-x, etc.
cat > "$BENCH_DIR/algebra.om" << 'EOF'
fn compute(n: int) {
    var total: int = 0;
    for (i: int in 0...n) {
        var x: int = i;
        x = x + 0;
        x = x * 1;
        x = x - 0;
        total = total + x;
    }
    return total;
}
fn main() {
    return compute(10000) % 256;
}
EOF

BENCHMARKS=("fib" "sumsq" "constfold" "bitwise" "strength" "algebra")
DESCRIPTIONS=(
    "Fibonacci (recursive call overhead)"
    "Sum of squares (loop + arithmetic)"
    "Constant folding stress"
    "Bitwise popcount loop"
    "Strength reduction (mul → shift)"
    "Algebraic identity elimination"
)

# ── Helper: run a binary N times, take median time ────────────────────────────
median_time() {
    local binary=$1
    local times=()
    for ((r=0; r<RUNS; r++)); do
        local start=$(date +%s%N)
        "$binary" > /dev/null 2>&1
        local end=$(date +%s%N)
        local elapsed=$(( (end - start) / 1000000 )) # ms
        times+=("$elapsed")
    done
    # Sort and take median
    IFS=$'\n' sorted=($(sort -n <<<"${times[*]}")); unset IFS
    echo "${sorted[$((RUNS/2))]}"
}

# ── Run benchmarks ────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}Running benchmarks ($RUNS runs each, median reported):${NC}"
echo ""
printf "%-35s %8s %8s %8s %10s\n" "Benchmark" "O0 (ms)" "O2 (ms)" "O3 (ms)" "Speedup"
printf "%-35s %8s %8s %8s %10s\n" "---------" "-------" "-------" "-------" "-------"

total_o0=0
total_o2=0
total_o3=0

for idx in "${!BENCHMARKS[@]}"; do
    name="${BENCHMARKS[$idx]}"
    desc="${DESCRIPTIONS[$idx]}"
    src="$BENCH_DIR/$name.om"

    # Compile at O0
    "$OMSC" "$src" -O0 -o "$BENCH_DIR/${name}_o0" 2>/dev/null
    # Compile at O2 (with e-graph + superopt)
    "$OMSC" "$src" -O2 -o "$BENCH_DIR/${name}_o2" 2>/dev/null
    # Compile at O3 (with e-graph + superopt)
    "$OMSC" "$src" -O3 -o "$BENCH_DIR/${name}_o3" 2>/dev/null

    if [ ! -f "$BENCH_DIR/${name}_o0" ] || [ ! -f "$BENCH_DIR/${name}_o2" ] || [ ! -f "$BENCH_DIR/${name}_o3" ]; then
        printf "%-35s %8s %8s %8s %10s\n" "$desc" "FAIL" "FAIL" "FAIL" "-"
        continue
    fi

    time_o0=$(median_time "$BENCH_DIR/${name}_o0")
    time_o2=$(median_time "$BENCH_DIR/${name}_o2")
    time_o3=$(median_time "$BENCH_DIR/${name}_o3")

    total_o0=$((total_o0 + time_o0))
    total_o2=$((total_o2 + time_o2))
    total_o3=$((total_o3 + time_o3))

    # Calculate speedup O3 vs O0
    if [ "$time_o0" -gt 0 ]; then
        speedup=$(echo "scale=2; $time_o0 / ($time_o3 + 0.001)" | bc 2>/dev/null || echo "N/A")
    else
        speedup="N/A"
    fi

    # Color based on speedup
    if [ "$time_o3" -lt "$time_o0" ]; then
        color="$GREEN"
    else
        color="$YELLOW"
    fi

    printf "%-35s %8d %8d ${color}%8d${NC} %10sx\n" "$desc" "$time_o0" "$time_o2" "$time_o3" "$speedup"

    # Clean up
    rm -f "$BENCH_DIR/${name}_o0" "$BENCH_DIR/${name}_o2" "$BENCH_DIR/${name}_o3"
done

echo ""
printf "%-35s %8d %8d %8d\n" "TOTAL (ms)" "$total_o0" "$total_o2" "$total_o3"

if [ "$total_o0" -gt 0 ]; then
    overall=$(echo "scale=2; $total_o0 / ($total_o3 + 0.001)" | bc 2>/dev/null || echo "N/A")
    echo -e "\n${BOLD}Overall speedup (O3 vs O0): ${GREEN}${overall}x${NC}"
fi

# ── Comparison with C (gcc -O3) ───────────────────────────────────────────────
if command -v gcc &> /dev/null; then
    echo ""
    echo -e "${BOLD}Comparing with C (gcc -O3):${NC}"
    echo ""

    # Fibonacci in C
    cat > "$BENCH_DIR/fib.c" << 'CEOF'
#include <stdlib.h>
int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}
int main() { return fib(30) % 256; }
CEOF

    # Sum of squares in C
    cat > "$BENCH_DIR/sumsq.c" << 'CEOF'
int main() {
    long total = 0;
    for (int i = 0; i < 10000; i++) {
        total += (long)i * i;
    }
    return (int)(total % 256);
}
CEOF

    C_BENCHMARKS=("fib" "sumsq")
    C_DESCRIPTIONS=("Fibonacci (recursive)" "Sum of squares (loop)")

    printf "%-35s %8s %8s %10s\n" "Benchmark" "C -O3" "OmSc O3" "Ratio"
    printf "%-35s %8s %8s %10s\n" "---------" "------" "-------" "-----"

    for idx in "${!C_BENCHMARKS[@]}"; do
        name="${C_BENCHMARKS[$idx]}"
        desc="${C_DESCRIPTIONS[$idx]}"

        gcc -O3 -o "$BENCH_DIR/${name}_c" "$BENCH_DIR/${name}.c" 2>/dev/null
        "$OMSC" "$BENCH_DIR/${name}.om" -O3 -o "$BENCH_DIR/${name}_om" 2>/dev/null

        if [ -f "$BENCH_DIR/${name}_c" ] && [ -f "$BENCH_DIR/${name}_om" ]; then
            time_c=$(median_time "$BENCH_DIR/${name}_c")
            time_om=$(median_time "$BENCH_DIR/${name}_om")

            if [ "$time_c" -gt 0 ]; then
                ratio=$(echo "scale=2; $time_om / ($time_c + 0.001)" | bc 2>/dev/null || echo "N/A")
            else
                ratio="N/A"
            fi

            if [ "$time_om" -le "$time_c" ]; then
                color="$GREEN"
            else
                color="$YELLOW"
            fi

            printf "%-35s %8d ${color}%8d${NC} %10sx\n" "$desc" "$time_c" "$time_om" "$ratio"
        fi

        rm -f "$BENCH_DIR/${name}_c" "$BENCH_DIR/${name}_om" "$BENCH_DIR/${name}.c"
    done
fi

# Clean up
rm -rf "$BENCH_DIR"

echo ""
echo -e "${BOLD}Benchmark complete.${NC}"
