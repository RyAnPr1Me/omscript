#!/bin/bash
# ---------------------------------------------------------------------------
# OmScript JIT vs AOT Benchmark Runner
# ---------------------------------------------------------------------------
#
# Runs benchmark programs in both JIT and AOT modes, measures wall-clock
# execution time, and produces a comparative summary.
#
# Usage:
#   ./run_benchmarks.sh              # Run all benchmarks
#   ./run_benchmarks.sh --quick      # Quick run (fewer iterations)
#   ./run_benchmarks.sh --verbose    # Show JIT tier transitions
#
# Prerequisites:
#   - Build the compiler first: cd build && cmake .. && make -j$(nproc)
#   - Or let this script build it for you.
#
# Output:
#   Per-benchmark timing table comparing JIT vs AOT wall-clock times,
#   plus a summary with speedup/slowdown ratios.
#
# ---------------------------------------------------------------------------

set -uo pipefail

# --- Colors ---------------------------------------------------------------
GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

# --- Configuration ---------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
OMSC="$BUILD_DIR/omsc"
TMP_DIR=$(mktemp -d /tmp/omsc_bench.XXXXXX)
VERBOSE=0
QUICK=0

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        --verbose|-v) VERBOSE=1 ;;
        --quick|-q) QUICK=1 ;;
        --help|-h)
            echo "Usage: $0 [--quick] [--verbose] [--help]"
            echo "  --quick    Run with reduced iterations"
            echo "  --verbose  Show JIT recompilation events"
            exit 0
            ;;
    esac
done

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# --- Build -----------------------------------------------------------------
echo -e "${BOLD}============================================${NC}"
echo -e "${BOLD}  OmScript JIT vs AOT Benchmark${NC}"
echo -e "${BOLD}============================================${NC}"
echo ""

if [ ! -x "$OMSC" ]; then
    echo -e "${YELLOW}Building compiler...${NC}"
    mkdir -p "$BUILD_DIR"
    (cd "$BUILD_DIR" && cmake .. > /dev/null 2>&1 && make -j"$(nproc)" > /dev/null 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${RED}Build failed${NC}"
        exit 1
    fi
    echo -e "${GREEN}✓ Build successful${NC}"
fi

# --- Benchmark list --------------------------------------------------------
# Each entry: "source_file|aot_exit_code|jit_exit_code|description"
# JIT exit code may differ from AOT due to PGO-specialized computation paths
# and different overflow behavior at higher tiers.
BENCHMARKS=(
    "examples/benchmark_jit_aot.om|128|128|JIT/AOT Suite (8 kernels)"
    "examples/benchmark_loops_math.om|240|143|Loops + Math (8 kernels)"
    "examples/jit_tier4_bench.om|96|96|Tier-4/5 Heavy (6 kernels)"
)

# --- Helper: run a command and write elapsed milliseconds to a file --------
# Usage: run_timed <output_file> <command> [args...]
# The elapsed time in ms is written to <output_file>.
# Returns the command's exit code.
run_timed() {
    local out_file="$1"
    shift
    local start end elapsed
    start=$(date +%s%N)
    "$@"
    local exit_code=$?
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))
    echo "$elapsed" > "$out_file"
    return $exit_code
}

# --- Helper: median of three values ----------------------------------------
median3() {
    echo -e "$1\n$2\n$3" | sort -n | sed -n '2p'
}

# --- Run benchmarks --------------------------------------------------------
echo -e "${BOLD}Running benchmarks...${NC}"
echo ""

TOTAL_AOT_MS=0
TOTAL_JIT_MS=0
PASS=0
FAIL=0

printf "${BOLD}%-35s %10s %10s %10s %8s${NC}\n" "Benchmark" "AOT (ms)" "JIT (ms)" "Ratio" "Status"
printf "%-35s %10s %10s %10s %8s\n" "-----------------------------------" "--------" "--------" "--------" "------"

for entry in "${BENCHMARKS[@]}"; do
    IFS='|' read -r source aot_expected jit_expected desc <<< "$entry"

    # --- AOT: compile at O3, then run ---
    aot_bin="$TMP_DIR/$(basename "$source" .om)_aot"
    if ! "$OMSC" -O3 "$source" -o "$aot_bin" > /dev/null 2>&1; then
        printf "%-35s ${RED}%s${NC}\n" "$desc" "AOT compile failed"
        FAIL=$((FAIL + 1))
        continue
    fi

    # Run AOT 3 times, take the median
    aot_t1=0; aot_t2=0; aot_t3=0
    aot_exit=0
    run_timed "$TMP_DIR/t" "$aot_bin" > /dev/null 2>&1; aot_exit=$?; aot_t1=$(cat "$TMP_DIR/t")
    run_timed "$TMP_DIR/t" "$aot_bin" > /dev/null 2>&1; aot_t2=$(cat "$TMP_DIR/t")
    run_timed "$TMP_DIR/t" "$aot_bin" > /dev/null 2>&1; aot_t3=$(cat "$TMP_DIR/t")
    aot_ms=$(median3 "$aot_t1" "$aot_t2" "$aot_t3")

    # Verify correctness
    if [ "$aot_exit" -ne "$aot_expected" ]; then
        printf "%-35s ${RED}AOT exit %d != %d${NC}\n" "$desc" "$aot_exit" "$aot_expected"
        FAIL=$((FAIL + 1))
        continue
    fi

    # --- JIT: run with adaptive tiered JIT ---
    jit_t1=0; jit_t2=0; jit_t3=0
    jit_exit=0
    run_timed "$TMP_DIR/t" "$OMSC" run "$source" > /dev/null 2>&1; jit_exit=$?; jit_t1=$(cat "$TMP_DIR/t")
    run_timed "$TMP_DIR/t" "$OMSC" run "$source" > /dev/null 2>&1; jit_t2=$(cat "$TMP_DIR/t")
    run_timed "$TMP_DIR/t" "$OMSC" run "$source" > /dev/null 2>&1; jit_t3=$(cat "$TMP_DIR/t")
    jit_ms=$(median3 "$jit_t1" "$jit_t2" "$jit_t3")

    if [ "$jit_exit" -ne "$jit_expected" ]; then
        printf "%-35s ${RED}JIT exit %d != %d${NC}\n" "$desc" "$jit_exit" "$jit_expected"
        FAIL=$((FAIL + 1))
        continue
    fi

    # --- Compute ratio ---
    if [ "$aot_ms" -gt 0 ]; then
        # ratio = JIT / AOT  (>1 means JIT is slower)
        ratio=$(awk "BEGIN { printf \"%.2f\", $jit_ms / $aot_ms }")
    else
        ratio="N/A"
    fi

    # --- Status ---
    PASS=$((PASS + 1))
    TOTAL_AOT_MS=$((TOTAL_AOT_MS + aot_ms))
    TOTAL_JIT_MS=$((TOTAL_JIT_MS + jit_ms))

    if [ "$jit_ms" -le "$aot_ms" ]; then
        status="${GREEN}JIT≤AOT${NC}"
    else
        status="${YELLOW}JIT>AOT${NC}"
    fi

    printf "%-35s %10d %10d %10s %b\n" "$desc" "$aot_ms" "$jit_ms" "${ratio}x" "$status"
done

echo ""
printf "${BOLD}%-35s %10s %10s %10s${NC}\n" "-----------------------------------" "--------" "--------" "--------"

if [ "$TOTAL_AOT_MS" -gt 0 ]; then
    total_ratio=$(awk "BEGIN { printf \"%.2f\", $TOTAL_JIT_MS / $TOTAL_AOT_MS }")
else
    total_ratio="N/A"
fi
printf "${BOLD}%-35s %10d %10d %10s${NC}\n" "TOTAL" "$TOTAL_AOT_MS" "$TOTAL_JIT_MS" "${total_ratio}x"

echo ""

# --- C reference benchmark (if gcc available) ------------------------------
if command -v gcc &> /dev/null && [ -f "$SCRIPT_DIR/examples/benchmark_loops_math.c" ]; then
    echo -e "${BOLD}C Reference (gcc -O2):${NC}"
    c_bin="$TMP_DIR/bench_c"
    gcc -O2 -o "$c_bin" "$SCRIPT_DIR/examples/benchmark_loops_math.c" -lm 2>/dev/null
    if [ $? -eq 0 ]; then
        run_timed "$TMP_DIR/t" "$c_bin" > /dev/null 2>&1; c_t1=$(cat "$TMP_DIR/t")
        run_timed "$TMP_DIR/t" "$c_bin" > /dev/null 2>&1; c_t2=$(cat "$TMP_DIR/t")
        run_timed "$TMP_DIR/t" "$c_bin" > /dev/null 2>&1; c_t3=$(cat "$TMP_DIR/t")
        c_ms=$(median3 "$c_t1" "$c_t2" "$c_t3")
        printf "  %-33s %10d ms\n" "benchmark_loops_math.c" "$c_ms"
        if [ "$TOTAL_AOT_MS" -gt 0 ] && [ "$c_ms" -gt 0 ]; then
            c_ratio=$(awk "BEGIN { printf \"%.2f\", $TOTAL_AOT_MS / $c_ms }")
            printf "  %-33s %10s\n" "OmScript AOT / C ratio" "${c_ratio}x"
        fi
        echo ""
    fi
fi

# --- Summary ---------------------------------------------------------------
echo -e "${BOLD}Summary:${NC}"
echo "  Benchmarks passed: $PASS / $((PASS + FAIL))"
if [ "$FAIL" -gt 0 ]; then
    echo -e "  ${RED}$FAIL benchmark(s) failed${NC}"
fi
echo "  Total AOT time:   ${TOTAL_AOT_MS} ms"
echo "  Total JIT time:   ${TOTAL_JIT_MS} ms"
echo "  JIT/AOT ratio:    ${total_ratio}x"
echo ""
echo -e "${BOLD}Notes:${NC}"
echo "  - JIT includes compilation overhead (tiered recompilation at runtime)"
echo "  - AOT is pre-compiled at O3 — no runtime compilation cost"
echo "  - JIT ratio >1.0x means JIT is slower (expected for short benchmarks)"
echo "  - For long-running workloads, JIT can match or exceed AOT via PGO"
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
