// ---------------------------------------------------------------------------
// Large-Scale Loops + Math Benchmark — C reference implementation
// ---------------------------------------------------------------------------
//
// Equivalent to examples/benchmark_loops_math.om for live comparison.
//
// Compile & run:
//   gcc -O2 -o bench_c examples/benchmark_loops_math.c && time ./bench_c
//   clang -O2 -o bench_c examples/benchmark_loops_math.c && time ./bench_c
//
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <time.h>

// ---------------------------------------------------------------------------
// 1. Sum of squares: sum(i^2) for i in [0, n)
// ---------------------------------------------------------------------------
static int64_t sum_of_squares(int64_t n) {
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        total += i * i;
    }
    return total;
}

// ---------------------------------------------------------------------------
// 2. Iterative Fibonacci: fib(n) via loop
// ---------------------------------------------------------------------------
static int64_t fibonacci(int64_t n) {
    if (n <= 1) return n;
    int64_t a = 0, b = 1;
    for (int64_t i = 2; i < n; i++) {
        int64_t tmp = a + b;
        a = b;
        b = tmp;
    }
    return b;
}

// ---------------------------------------------------------------------------
// 3. Collatz sequence length: steps until n reaches 1
// ---------------------------------------------------------------------------
static int64_t collatz_length(int64_t n) {
    int64_t steps = 0;
    int64_t val = n;
    while (val != 1) {
        if (val % 2 == 0) {
            val /= 2;
        } else {
            val = val * 3 + 1;
        }
        steps++;
    }
    return steps;
}

// ---------------------------------------------------------------------------
// 4. Integer square root via Newton's method
// ---------------------------------------------------------------------------
static int64_t isqrt(int64_t n) {
    if (n <= 1) return n;
    int64_t x = n;
    int64_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// ---------------------------------------------------------------------------
// 5. GCD via Euclidean algorithm
// ---------------------------------------------------------------------------
static int64_t gcd(int64_t a, int64_t b) {
    while (b != 0) {
        int64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

// ---------------------------------------------------------------------------
// 6. Simple prime check (trial division)
// ---------------------------------------------------------------------------
static int64_t is_prime(int64_t n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0) return 0;
    for (int64_t i = 3; i * i <= n; i += 2) {
        if (n % i == 0) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 7. Integer exponentiation by squaring
// ---------------------------------------------------------------------------
static int64_t ipow(int64_t base, int64_t exp) {
    int64_t result = 1;
    int64_t b = base;
    int64_t e = exp;
    while (e > 0) {
        if (e % 2 == 1) {
            result *= b;
        }
        b *= b;
        e /= 2;
    }
    return result;
}

// ---------------------------------------------------------------------------
// 8. Nested loop matrix-multiply accumulator (NxN)
// ---------------------------------------------------------------------------
static int64_t matrix_accum(int64_t size) {
    int64_t accum = 0;
    for (int64_t i = 0; i < size; i++) {
        for (int64_t j = 0; j < size; j++) {
            int64_t cell = 0;
            for (int64_t k = 0; k < size; k++) {
                cell += i * k + k * j;
            }
            accum += cell;
        }
    }
    return accum;
}

// ===========================================================================
// Benchmark harness
// ===========================================================================

static int64_t bench_sum_of_squares(void) {
    const int iters = 2000;
    int64_t checksum = 0;
    clock_t t0 = clock();
    for (int i = 0; i < iters; i++) {
        checksum += sum_of_squares(500);
    }
    clock_t t1 = clock();
    printf("bench_sum_of_squares\n%.6f\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC);
    return checksum;
}

static int64_t bench_fibonacci(void) {
    const int iters = 5000;
    int64_t checksum = 0;
    clock_t t0 = clock();
    for (int i = 0; i < iters; i++) {
        checksum += fibonacci(40);
    }
    clock_t t1 = clock();
    printf("bench_fibonacci\n%.6f\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC);
    return checksum;
}

static int64_t bench_collatz(void) {
    const int iters = 2000;
    int64_t checksum = 0;
    clock_t t0 = clock();
    for (int i = 0; i < iters; i++) {
        checksum += collatz_length(837799);
    }
    clock_t t1 = clock();
    printf("bench_collatz\n%.6f\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC);
    return checksum;
}

static int64_t bench_isqrt(void) {
    const int iters = 3000;
    int64_t checksum = 0;
    clock_t t0 = clock();
    for (int i = 0; i < iters; i++) {
        checksum += isqrt(999999937);
    }
    clock_t t1 = clock();
    printf("bench_isqrt\n%.6f\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC);
    return checksum;
}

static int64_t bench_gcd(void) {
    const int iters = 5000;
    int64_t checksum = 0;
    clock_t t0 = clock();
    for (int i = 0; i < iters; i++) {
        checksum += gcd(1234567890, 987654321);
    }
    clock_t t1 = clock();
    printf("bench_gcd\n%.6f\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC);
    return checksum;
}

static int64_t bench_primes(void) {
    const int iters = 1000;
    int64_t checksum = 0;
    clock_t t0 = clock();
    for (int i = 0; i < iters; i++) {
        int64_t count = 0;
        for (int64_t n = 2; n < 1000; n++) {
            count += is_prime(n);
        }
        checksum += count;
    }
    clock_t t1 = clock();
    printf("bench_primes\n%.6f\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC);
    return checksum;
}

static int64_t bench_ipow(void) {
    const int iters = 5000;
    int64_t checksum = 0;
    clock_t t0 = clock();
    for (int i = 0; i < iters; i++) {
        checksum += ipow(3, 20);
    }
    clock_t t1 = clock();
    printf("bench_ipow\n%.6f\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC);
    return checksum;
}

static int64_t bench_matrix(void) {
    const int iters = 200;
    int64_t checksum = 0;
    clock_t t0 = clock();
    for (int i = 0; i < iters; i++) {
        checksum += matrix_accum(30);
    }
    clock_t t1 = clock();
    printf("bench_matrix\n%.6f\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC);
    return checksum;
}

int main(void) {
    printf("=== C Loops + Math Benchmark ===\n");
    printf("Timings in seconds (via clock())\n\n");

    int64_t total = 0;

    total += bench_sum_of_squares();
    total += bench_fibonacci();
    total += bench_collatz();
    total += bench_isqrt();
    total += bench_gcd();
    total += bench_primes();
    total += bench_ipow();
    total += bench_matrix();

    printf("\n=== All benchmarks complete ===\n");
    printf("Checksum: %lld\n", (long long)(total % 256));

    return (int)(total % 256);
}
