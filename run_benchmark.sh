#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run_benchmark.sh — OmScript JIT vs AOT Benchmark Runner
# ---------------------------------------------------------------------------
# Runs every .om benchmark file in both JIT and AOT (-O3) modes, measuring
# wall-clock time with millisecond resolution.  Each mode is run 3 times and
# the MEDIAN time is reported to reduce variance from OS scheduling noise.
#
# Usage:
#   ./run_benchmark.sh              # run all bench_suite targets
#   ./run_benchmark.sh quick        # run quick smoke-test only
#
# Output: a formatted table comparing JIT vs AOT timing and the speedup ratio.
# ---------------------------------------------------------------------------

set -euo pipefail

OMSC="${OMSC:-./build/omsc}"
RUNS=3
TMPDIR_LOCAL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

# Colour codes (suppressed when not a terminal)
if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; RESET=''
fi

# ---------------------------------------------------------------------------
# ms_now — current time in milliseconds
# ---------------------------------------------------------------------------
ms_now() { date +%s%3N; }

# ---------------------------------------------------------------------------
# time_run <cmd...> — run a command and print its wall-clock time in ms
# ---------------------------------------------------------------------------
time_run() {
    local t0 t1
    t0=$(ms_now)
    "$@" > /dev/null 2>&1
    t1=$(ms_now)
    echo $(( t1 - t0 ))
}

# ---------------------------------------------------------------------------
# median <n1> <n2> <n3> — print the median of three integers
# ---------------------------------------------------------------------------
median() {
    local a=$1 b=$2 c=$3
    # Sort three values
    if [ "$a" -gt "$b" ]; then local t=$a; a=$b; b=$t; fi
    if [ "$b" -gt "$c" ]; then local t=$b; b=$c; c=$t; fi
    if [ "$a" -gt "$b" ]; then local t=$a; a=$b; b=$t; fi
    echo "$b"
}

# ---------------------------------------------------------------------------
# bench_file <label> <src.om> — measure JIT and AOT times and print one row
# ---------------------------------------------------------------------------
bench_file() {
    local label="$1"
    local src="$2"
    local aot_bin="$TMPDIR_LOCAL/bench_aot"

    # ---- AOT build (O3) ----
    if ! "$OMSC" compile -O3 "$src" -o "$aot_bin" 2>/dev/null; then
        printf "  %-40s  %s\n" "$label" "(AOT compile failed)"
        return
    fi

    # ---- time AOT (3 runs) ----
    local a0 a1 a2
    a0=$(time_run "$aot_bin")
    a1=$(time_run "$aot_bin")
    a2=$(time_run "$aot_bin")
    local aot_ms
    aot_ms=$(median "$a0" "$a1" "$a2")

    # ---- time JIT (3 runs) ----
    local j0 j1 j2
    j0=$(time_run "$OMSC" run "$src")
    j1=$(time_run "$OMSC" run "$src")
    j2=$(time_run "$OMSC" run "$src")
    local jit_ms
    jit_ms=$(median "$j0" "$j1" "$j2")

    # ---- compute ratio (×10 fixed-point to avoid floats in bash) ----
    local ratio_x10 ratio_int ratio_frac colour
    if [ "$aot_ms" -gt 0 ]; then
        ratio_x10=$(( (jit_ms * 10) / aot_ms ))
    else
        ratio_x10=10
    fi
    ratio_int=$(( ratio_x10 / 10 ))
    ratio_frac=$(( ratio_x10 % 10 ))

    # Colour: green if JIT within 120 % of AOT, yellow within 200 %, red otherwise
    if [ "$ratio_x10" -le 12 ]; then
        colour="$GREEN"
    elif [ "$ratio_x10" -le 20 ]; then
        colour="$YELLOW"
    else
        colour="$RED"
    fi

    printf "  %-40s  %5d ms  %5d ms  ${colour}%d.%dx${RESET}\n" \
        "$label" "$aot_ms" "$jit_ms" "$ratio_int" "$ratio_frac"
}

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------
echo ""
printf "${BOLD}${CYAN}%s${RESET}\n" "================================================================"
printf "${BOLD}${CYAN}  OmScript JIT vs AOT Benchmark  (${RUNS}-run median, wall clock)${RESET}\n"
printf "${BOLD}${CYAN}%s${RESET}\n" "================================================================"
printf "  %-40s  %8s  %8s  %s\n" "Benchmark" "AOT -O3" "ORC JIT" "JIT/AOT"
printf "  %-40s  %8s  %8s  %s\n" \
    "----------------------------------------" "--------" "--------" "-------"

# ---------------------------------------------------------------------------
# Run the comprehensive suite
# ---------------------------------------------------------------------------
SUITE="examples/bench_suite.om"

printf "\n${BOLD}── Group 1: Arithmetic loops ${RESET}\n"
# Run the whole bench_suite file and capture timing via the shell
bench_file "bench_suite (all kernels)"             "$SUITE"

# Also run the individual classic benchmarks for comparison
printf "\n${BOLD}── Group 2: Classic kernels (from existing benchmarks) ${RESET}\n"
bench_file "benchmark_jit_aot.om"                  "examples/benchmark_jit_aot.om"
bench_file "benchmark_loops_math.om"               "examples/benchmark_loops_math.om"
bench_file "benchmark.om"                          "examples/benchmark.om"
bench_file "jit_tier4_bench.om"                    "examples/jit_tier4_bench.om"

printf "\n${BOLD}── Group 3: Individual program examples ${RESET}\n"
bench_file "fibonacci.om"                          "examples/fibonacci.om"
bench_file "factorial.om"                          "examples/factorial.om"
bench_file "optimization_stress_test.om"           "examples/optimization_stress_test.om"
bench_file "jit_hot_demo.om"                       "examples/jit_hot_demo.om"
bench_file "inlining_test.om"                      "examples/inlining_test.om"
bench_file "optimized_loops.om"                    "examples/optimized_loops.om"
bench_file "constant_folding.om"                   "examples/constant_folding.om"
bench_file "memory_stress_test.om"                 "examples/memory_stress_test.om"

printf "\n${BOLD}── Group 4: String / misc overhead ${RESET}\n"
bench_file "string_func_test.om"                   "examples/string_func_test.om"
bench_file "stdlib_test.om"                        "examples/stdlib_test.om"
bench_file "math_builtins_test.om"                 "examples/math_builtins_test.om"

printf "\n"
printf "${BOLD}${CYAN}%s${RESET}\n" "================================================================"
printf "${BOLD}Notes:${RESET}\n"
printf "  • JIT Tier-1 = O0 baseline (first 5 calls per function)\n"
printf "  • JIT Tier-2 = O3+PGO hot-compiled (background, hottest-first priority)\n"
printf "  • AOT        = static O3 with no profiling overhead\n"
printf "  • Ratio < 1.2x %s(green)%s  = JIT matches AOT\n"    "$GREEN" "$RESET"
printf "  • Ratio 1.2-2x %s(yellow)%s = minor JIT overhead\n" "$YELLOW" "$RESET"
printf "  • Ratio > 2x   %s(red)%s    = JIT overhead significant\n" "$RED" "$RESET"
printf "${BOLD}${CYAN}%s${RESET}\n" "================================================================"
echo ""
