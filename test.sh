#!/usr/bin/env bash

set -e

echo "=== Generating source files ==="

# ---- C version ----
cat > stress.c << 'EOF'
#include <stdint.h>
#include <stdio.h>

int stress(int n) {
    int acc = 0;
    uint32_t x = 1;

    for (int i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;

        uint32_t a = x ^ (x >> 13);
        uint32_t b = a * 1274126177u;
        uint32_t c = (b >> 16) + (b << 3);

        acc += (c ^ (acc >> 7));
        acc += (i & 1) ? (a >> 3) : (b << 1);
    }

    return acc;
}

int main() {
    int r = stress(100000000);
    printf("%d\n", r);
}
EOF

# ---- OmScript version ----
cat > stress.om << 'EOF'
fn stress(n) {
    var acc = 0;
    var x = 1;

    for (i in 0...n) {
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF;

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

# Compile C with max optimizations
clang -O3 -march=znver4 -mtune=znver4 -flto -funroll-loops stress.c -o stress_c

# Compile OmScript with max optimizations
omsc build stress.om  -o stress_om -march=znver4 -mtune=znver4 -O3 -flto -funroll-loops -fvectorize 

echo "=== Running benchmarks ==="

echo "--- C (clang -O3) ---"
/usr/bin/time -f "Time: %e sec" ./stress_c

echo "--- OmScript (omsc -O3) ---"
/usr/bin/time -f "Time: %e sec" ./stress_om

echo "=== Done ==="
