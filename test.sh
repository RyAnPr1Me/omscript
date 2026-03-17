#!/usr/bin/env bash

# Usage:

# ./benchmark.sh program.c program.om

set -e

C_FILE="$1"
OM_FILE="$2"

C_OUT="c_prog"
OM_OUT="om_prog"

echo "=== Compiling ==="

# Compile C (max optimization)

clang -O3 -march=native "$C_FILE" -o "$C_OUT"

# Compile OmScript (max optimization)

omsc "$OM_FILE" -o "$OM_OUT" -O3

echo "=== Running Benchmarks ==="

# Run C program

echo "--- C (clang -O3) ---"
/usr/bin/time -f "Time: %e sec" ./"$C_OUT"

# Run OmScript program

echo "--- OmScript (O3) ---"
/usr/bin/time -f "Time: %e sec" ./"$OM_OUT"

echo "=== Done ==="
