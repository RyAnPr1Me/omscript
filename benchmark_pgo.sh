#!/bin/bash
# ──────────────────────────────────────────────────────────────────────────────
#  PGO + LTO Compiler Benchmark
#
#  Builds the OmScript compiler three ways and compares compile-time
#  performance across the full example suite:
#
#    1. Normal   – cmake -DCMAKE_BUILD_TYPE=Release
#    2. LTO only – cmake -DCMAKE_BUILD_TYPE=Release -DLTO=ON
#    3. PGO+LTO  – Instrumented build → profile → rebuild with PGO+LTO
#
#  Usage:
#    bash benchmark_pgo.sh                # full benchmark (builds everything)
#    bash benchmark_pgo.sh --skip-build   # skip builds, use existing binaries
#
#  Requirements:
#    - GCC, LLVM 18 dev libs, libgtest-dev
#    - ~10 minutes for the full run (builds 3 compilers + benchmarks)
# ──────────────────────────────────────────────────────────────────────────────
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

RUNS=5  # median of N runs per measurement
SKIP_BUILD=false
if [[ "${1:-}" == "--skip-build" ]]; then
    SKIP_BUILD=true
fi

RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[0;33m'
BLD='\033[1m'
CYN='\033[0;36m'
RST='\033[0m'

LLVM_DIR="${LLVM_DIR:-/usr/lib/llvm-18/cmake}"

BUILD_NORMAL="build-bench-normal"
BUILD_LTO="build-bench-lto"
BUILD_PGO="build-bench-pgo"
PROFILE_DIR="$PWD/bench-pgo-profiles"

# ── Build phase ──────────────────────────────────────────────────────────────
if [ "$SKIP_BUILD" = false ]; then
    echo -e "${BLD}══════════════════════════════════════════════════════════════${RST}"
    echo -e "${BLD}  Building three compiler variants...${RST}"
    echo -e "${BLD}══════════════════════════════════════════════════════════════${RST}"
    echo ""

    # 1) Normal release build
    echo -e "${CYN}[1/4] Building: Normal (Release)${RST}"
    cmake -B "$BUILD_NORMAL" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
        -DLLVM_DIR="$LLVM_DIR" > /dev/null 2>&1
    cmake --build "$BUILD_NORMAL" --target omsc --parallel "$(nproc)" > /dev/null 2>&1
    echo "  ✓ Normal build complete"

    # 2) LTO-only build
    echo -e "${CYN}[2/4] Building: LTO only${RST}"
    cmake -B "$BUILD_LTO" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
        -DLLVM_DIR="$LLVM_DIR" \
        -DLTO=ON > /dev/null 2>&1
    cmake --build "$BUILD_LTO" --target omsc --parallel "$(nproc)" > /dev/null 2>&1
    echo "  ✓ LTO build complete"

    # 3) PGO+LTO build (instrument → profile → rebuild)
    echo -e "${CYN}[3/4] Building: PGO instrumented${RST}"
    rm -rf "$PROFILE_DIR"
    mkdir -p "$PROFILE_DIR"
    cmake -B "$BUILD_PGO" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
        -DLLVM_DIR="$LLVM_DIR" \
        -DPGO_GENERATE=ON \
        -DPGO_PROFILE_DIR="$PROFILE_DIR" > /dev/null 2>&1
    cmake --build "$BUILD_PGO" --target omsc --parallel "$(nproc)" > /dev/null 2>&1
    echo "  ✓ Instrumented build complete"

    echo -e "${CYN}[3/4] Generating PGO profiles (compiling all examples × 4 opt levels)...${RST}"
    OMSC="./$BUILD_PGO/omsc"
    PGO_COUNT=0
    for f in examples/*.om; do
        [ -f "$f" ] || continue
        for opt in -O0 -O1 -O2 -O3; do
            "$OMSC" "$f" $opt -o /tmp/bench_pgo_out > /dev/null 2>&1 && PGO_COUNT=$((PGO_COUNT + 1)) || true
        done
    done
    # Front-end paths
    for f in examples/*.om; do
        [ -f "$f" ] || continue
        "$OMSC" lex "$f" > /dev/null 2>&1 || true
        "$OMSC" parse "$f" > /dev/null 2>&1 || true
    done
    "$OMSC" --help > /dev/null 2>&1 || true

    GCDA_COUNT=$(find "$PROFILE_DIR" -name '*.gcda' 2>/dev/null | wc -l)
    echo "  ✓ Generated $GCDA_COUNT profile files from $PGO_COUNT compilations"

    echo -e "${CYN}[4/4] Rebuilding: PGO+LTO (same build dir for correct .gcda paths)${RST}"
    cmake -B "$BUILD_PGO" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
        -DLLVM_DIR="$LLVM_DIR" \
        -DLTO=ON \
        -DPGO_GENERATE=OFF \
        -DPGO_USE="$PROFILE_DIR" > /dev/null 2>&1
    cmake --build "$BUILD_PGO" --target omsc --parallel "$(nproc)" > /dev/null 2>&1
    echo "  ✓ PGO+LTO build complete"
    echo ""
fi

# Verify binaries exist
OMSC_NORMAL="./$BUILD_NORMAL/omsc"
OMSC_LTO="./$BUILD_LTO/omsc"
OMSC_PGO="./$BUILD_PGO/omsc"
for bin in "$OMSC_NORMAL" "$OMSC_LTO" "$OMSC_PGO"; do
    if [ ! -x "$bin" ]; then
        echo "ERROR: $bin not found. Run without --skip-build first." >&2
        exit 1
    fi
done

# ── Benchmark helpers ────────────────────────────────────────────────────────
time_ms() {
    local compiler="$1"; shift
    local start end
    start=$(date +%s%N)
    "$compiler" "$@" > /dev/null 2>&1 || true
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

median() {
    local -a sorted=($(printf '%s\n' "$@" | sort -n))
    echo "${sorted[$((${#sorted[@]}/2))]}"
}

bench_one() {
    local label="$1"; shift
    local -a times_n=() times_l=() times_p=()

    for ((i=0; i<RUNS; i++)); do
        times_n+=($(time_ms "$OMSC_NORMAL" "$@"))
        times_l+=($(time_ms "$OMSC_LTO" "$@"))
        times_p+=($(time_ms "$OMSC_PGO" "$@"))
    done

    local mn=$(median "${times_n[@]}")
    local ml=$(median "${times_l[@]}")
    local mp=$(median "${times_p[@]}")

    local pct_l=100 pct_p=100
    [ "$mn" -gt 0 ] && pct_l=$(( ml * 100 / mn ))
    [ "$mn" -gt 0 ] && pct_p=$(( mp * 100 / mn ))

    printf "  %-38s  %5d ms  %5d ms (%3d%%)  %5d ms (%3d%%)\n" \
        "$label" "$mn" "$ml" "$pct_l" "$mp" "$pct_p"
}

# ── Run benchmarks ───────────────────────────────────────────────────────────
echo -e "${BLD}╔════════════════════════════════════════════════════════════════════════════════════╗${RST}"
echo -e "${BLD}║  Compiler Benchmark: Normal vs LTO vs PGO+LTO  (median of $RUNS runs)               ║${RST}"
echo -e "${BLD}╚════════════════════════════════════════════════════════════════════════════════════╝${RST}"
echo ""
echo -e "  Binary sizes:  Normal $(du -h "$OMSC_NORMAL" | cut -f1)   LTO $(du -h "$OMSC_LTO" | cut -f1)   PGO+LTO $(du -h "$OMSC_PGO" | cut -f1)"
echo ""
printf "  %-38s  %9s  %16s  %16s\n" "Benchmark" "Normal" "LTO" "PGO+LTO"
printf "  %-38s  %9s  %16s  %16s\n" "─────────" "──────" "───" "───────"

echo ""
echo -e "${CYN}  ── Optimization levels ──${RST}"
bench_one "stress_test -O0" examples/optimization_stress_test.om -O0 -o /tmp/bm_out
bench_one "stress_test -O1" examples/optimization_stress_test.om -O1 -o /tmp/bm_out
bench_one "stress_test -O2" examples/optimization_stress_test.om -O2 -o /tmp/bm_out
bench_one "stress_test -O3" examples/optimization_stress_test.om -O3 -o /tmp/bm_out

echo ""
echo -e "${CYN}  ── Various programs at -O2 ──${RST}"
bench_one "factorial.om" examples/factorial.om -O2 -o /tmp/bm_out
bench_one "fibonacci.om" examples/fibonacci.om -O2 -o /tmp/bm_out
bench_one "advanced.om" examples/advanced.om -O2 -o /tmp/bm_out
bench_one "optmax.om" examples/optmax.om -O2 -o /tmp/bm_out
bench_one "string_test.om" examples/string_test.om -O2 -o /tmp/bm_out
bench_one "array_test.om" examples/array_test.om -O2 -o /tmp/bm_out
bench_one "enum_test.om" examples/enum_test.om -O2 -o /tmp/bm_out
bench_one "struct_test.om" examples/struct_test.om -O2 -o /tmp/bm_out
bench_one "lambda_test.om" examples/lambda_test.om -O2 -o /tmp/bm_out
bench_one "array_higher_order_test.om" examples/array_higher_order_test.om -O2 -o /tmp/bm_out

echo ""
echo -e "${CYN}  ── Heavy -O3 workloads ──${RST}"
bench_one "stress_test -O3" examples/optimization_stress_test.om -O3 -o /tmp/bm_out
bench_one "optmax.om -O3" examples/optmax.om -O3 -o /tmp/bm_out
bench_one "inlining_test.om -O3" examples/inlining_test.om -O3 -o /tmp/bm_out
bench_one "production_features -O3" examples/production_features_test.om -O3 -o /tmp/bm_out

echo ""
echo -e "${CYN}  ── Front-end only ──${RST}"
bench_one "lex stress_test" lex examples/optimization_stress_test.om
bench_one "parse stress_test" parse examples/optimization_stress_test.om
bench_one "emit-ir stress_test" emit-ir examples/optimization_stress_test.om

echo ""
echo -e "${CYN}  ── Batch: all ~136 examples at -O2 ──${RST}"
batch_bench() {
    local compiler="$1"
    local start=$(date +%s%N)
    for f in examples/*.om; do
        [ -f "$f" ] || continue
        "$compiler" "$f" -O2 -o /tmp/bm_out > /dev/null 2>&1 || true
    done
    local end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

declare -a bn=() bl=() bp=()
for ((i=0; i<3; i++)); do
    bn+=($(batch_bench "$OMSC_NORMAL"))
    bl+=($(batch_bench "$OMSC_LTO"))
    bp+=($(batch_bench "$OMSC_PGO"))
done
mbn=$(median "${bn[@]}")
mbl=$(median "${bl[@]}")
mbp=$(median "${bp[@]}")
pbl=100; pbp=100
[ "$mbn" -gt 0 ] && pbl=$(( mbl * 100 / mbn ))
[ "$mbn" -gt 0 ] && pbp=$(( mbp * 100 / mbn ))
printf "  %-38s  %5d ms  %5d ms (%3d%%)  %5d ms (%3d%%)\n" \
    "All examples at -O2" "$mbn" "$mbl" "$pbl" "$mbp" "$pbp"

echo ""
echo -e "${BLD}══════════════════════════════════════════════════════════════════════════════════════${RST}"
echo -e "${BLD}  Notes:${RST}"
echo -e "  • Percentages are relative to Normal (100% = same speed, lower = faster)"
echo -e "  • Most compile time is spent in LLVM library code (optimization passes,"
echo -e "    code generation, linking).  PGO only optimizes OmScript's own code"
echo -e "    (lexer, parser, codegen dispatch) which is a small fraction of total time."
echo -e "  • PGO benefits are most visible on front-end-heavy workloads (lex/parse)"
echo -e "    and may grow as the compiler's own code becomes a larger fraction."
echo -e "${BLD}══════════════════════════════════════════════════════════════════════════════════════${RST}"

# Cleanup
rm -f /tmp/bm_out /tmp/bm_out.o /tmp/bench_pgo_out /tmp/bench_pgo_out.o
