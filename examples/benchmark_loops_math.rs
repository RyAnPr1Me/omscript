// ---------------------------------------------------------------------------
// Large-Scale Loops + Math Benchmark — Rust reference implementation
// ---------------------------------------------------------------------------
//
// Equivalent to examples/benchmark_loops_math.om for live comparison.
//
// Compile & run:
//   rustc -O -o bench_rs examples/benchmark_loops_math.rs && time ./bench_rs
//
// ---------------------------------------------------------------------------

use std::time::Instant;

// ---------------------------------------------------------------------------
// 1. Sum of squares: sum(i^2) for i in [0, n)
// ---------------------------------------------------------------------------
fn sum_of_squares(n: i64) -> i64 {
    let mut total: i64 = 0;
    for i in 0..n {
        total += i * i;
    }
    total
}

// ---------------------------------------------------------------------------
// 2. Iterative Fibonacci: fib(n) via loop
// ---------------------------------------------------------------------------
fn fibonacci(n: i64) -> i64 {
    if n <= 1 {
        return n;
    }
    let mut a: i64 = 0;
    let mut b: i64 = 1;
    for _ in 2..n {
        let tmp = a.wrapping_add(b);
        a = b;
        b = tmp;
    }
    b
}

// ---------------------------------------------------------------------------
// 3. Collatz sequence length: steps until n reaches 1
// ---------------------------------------------------------------------------
fn collatz_length(n: i64) -> i64 {
    let mut steps: i64 = 0;
    let mut val = n;
    while val != 1 {
        if val % 2 == 0 {
            val /= 2;
        } else {
            val = val * 3 + 1;
        }
        steps += 1;
    }
    steps
}

// ---------------------------------------------------------------------------
// 4. Integer square root via Newton's method
// ---------------------------------------------------------------------------
fn isqrt(n: i64) -> i64 {
    if n <= 1 {
        return n;
    }
    let mut x = n;
    let mut y = (x + 1) / 2;
    while y < x {
        x = y;
        y = (x + n / x) / 2;
    }
    x
}

// ---------------------------------------------------------------------------
// 5. GCD via Euclidean algorithm
// ---------------------------------------------------------------------------
fn gcd(mut a: i64, mut b: i64) -> i64 {
    while b != 0 {
        let t = b;
        b = a % b;
        a = t;
    }
    a
}

// ---------------------------------------------------------------------------
// 6. Simple prime check (trial division)
// ---------------------------------------------------------------------------
fn is_prime(n: i64) -> i64 {
    if n < 2 {
        return 0;
    }
    if n < 4 {
        return 1;
    }
    if n % 2 == 0 {
        return 0;
    }
    let mut i: i64 = 3;
    while i * i <= n {
        if n % i == 0 {
            return 0;
        }
        i += 2;
    }
    1
}

// ---------------------------------------------------------------------------
// 7. Integer exponentiation by squaring
// ---------------------------------------------------------------------------
fn ipow(base: i64, exp: i64) -> i64 {
    let mut result: i64 = 1;
    let mut b = base;
    let mut e = exp;
    while e > 0 {
        if e % 2 == 1 {
            result = result.wrapping_mul(b);
        }
        b = b.wrapping_mul(b);
        e /= 2;
    }
    result
}

// ---------------------------------------------------------------------------
// 8. Nested loop matrix-multiply accumulator (NxN)
// ---------------------------------------------------------------------------
fn matrix_accum(size: i64) -> i64 {
    let mut accum: i64 = 0;
    for i in 0..size {
        for j in 0..size {
            let mut cell: i64 = 0;
            for k in 0..size {
                cell += i * k + k * j;
            }
            accum += cell;
        }
    }
    accum
}

// ===========================================================================
// Benchmark harness
// ===========================================================================

fn bench_sum_of_squares() -> i64 {
    let iters = 2000;
    let mut checksum: i64 = 0;
    let t0 = Instant::now();
    for _ in 0..iters {
        checksum += sum_of_squares(500);
    }
    let elapsed = t0.elapsed().as_secs_f64();
    println!("bench_sum_of_squares\n{:.6}", elapsed);
    checksum
}

fn bench_fibonacci() -> i64 {
    let iters = 5000;
    let mut checksum: i64 = 0;
    let t0 = Instant::now();
    for _ in 0..iters {
        checksum = checksum.wrapping_add(fibonacci(40));
    }
    let elapsed = t0.elapsed().as_secs_f64();
    println!("bench_fibonacci\n{:.6}", elapsed);
    checksum
}

fn bench_collatz() -> i64 {
    let iters = 2000;
    let mut checksum: i64 = 0;
    let t0 = Instant::now();
    for _ in 0..iters {
        checksum += collatz_length(837799);
    }
    let elapsed = t0.elapsed().as_secs_f64();
    println!("bench_collatz\n{:.6}", elapsed);
    checksum
}

fn bench_isqrt() -> i64 {
    let iters = 3000;
    let mut checksum: i64 = 0;
    let t0 = Instant::now();
    for _ in 0..iters {
        checksum += isqrt(999999937);
    }
    let elapsed = t0.elapsed().as_secs_f64();
    println!("bench_isqrt\n{:.6}", elapsed);
    checksum
}

fn bench_gcd() -> i64 {
    let iters = 5000;
    let mut checksum: i64 = 0;
    let t0 = Instant::now();
    for _ in 0..iters {
        checksum += gcd(1234567890, 987654321);
    }
    let elapsed = t0.elapsed().as_secs_f64();
    println!("bench_gcd\n{:.6}", elapsed);
    checksum
}

fn bench_primes() -> i64 {
    let iters = 1000;
    let mut checksum: i64 = 0;
    let t0 = Instant::now();
    for _ in 0..iters {
        let mut count: i64 = 0;
        for n in 2..1000_i64 {
            count += is_prime(n);
        }
        checksum += count;
    }
    let elapsed = t0.elapsed().as_secs_f64();
    println!("bench_primes\n{:.6}", elapsed);
    checksum
}

fn bench_ipow() -> i64 {
    let iters = 5000;
    let mut checksum: i64 = 0;
    let t0 = Instant::now();
    for _ in 0..iters {
        checksum += ipow(3, 20);
    }
    let elapsed = t0.elapsed().as_secs_f64();
    println!("bench_ipow\n{:.6}", elapsed);
    checksum
}

fn bench_matrix() -> i64 {
    let iters = 200;
    let mut checksum: i64 = 0;
    let t0 = Instant::now();
    for _ in 0..iters {
        checksum += matrix_accum(30);
    }
    let elapsed = t0.elapsed().as_secs_f64();
    println!("bench_matrix\n{:.6}", elapsed);
    checksum
}

fn main() {
    println!("=== Rust Loops + Math Benchmark ===");
    println!("Timings in seconds (via std::time::Instant)\n");

    let mut total: i64 = 0;

    total += bench_sum_of_squares();
    total = total.wrapping_add(bench_fibonacci());
    total += bench_collatz();
    total += bench_isqrt();
    total += bench_gcd();
    total += bench_primes();
    total += bench_ipow();
    total += bench_matrix();

    println!("\n=== All benchmarks complete ===");
    println!("Checksum: {}", total % 256);

    std::process::exit((total % 256) as i32);
}
