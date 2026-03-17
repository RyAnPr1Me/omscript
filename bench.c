#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* helpers */
static long gcd(long a, long b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { long t = b; b = a % b; a = t; }
    return a;
}
static long log2i(long n) {
    if (n <= 0) return -1;
    long r = 0;
    while (n >>= 1) r++;
    return r;
}

/*  0 ── integer math ──────────────────────────── */
static long bench_math(long n) {
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
        acc += log2i(i);
    }
    return acc;
}

/*  1 ── array push ────────────────────────────── */
static long bench_push(long n) {
    long cap = 16, len = 0;
    long *arr = malloc(cap * sizeof(long));
    for (long i = 0; i < n; i++) {
        if (len == cap) { cap *= 2; arr = realloc(arr, cap * sizeof(long)); }
        arr[len++] = (i * 3) % 12345;
    }
    free(arr);
    return len;
}

/*  2 ── higher-order (map/filter/reduce) ──────── */
static long bench_hof(long n) {
    long *arr = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) arr[i] = (i * 7) % 1000;

    long *mapped = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) mapped[i] = (arr[i] * arr[i]) % 997;

    long reduced = 0, count = 0;
    for (long i = 0; i < n; i++) {
        if (mapped[i] % 2 == 0) { reduced += mapped[i]; count++; }
    }
    free(arr); free(mapped);
    return reduced + count;
}

/*  3 ── string concat ─────────────────────────── */
static long bench_strcat(long n) {
    long cap = 16, len = 1;
    char *s = malloc(cap);
    s[0] = 'x'; s[1] = '\0';
    for (long i = 0; i < n; i++) {
        if (len + 2 > cap) { cap *= 2; s = realloc(s, cap); }
        s[len++] = 'y'; s[len] = '\0';
    }
    long r = len;
    free(s);
    return r;
}

/*  4 ── string ops ────────────────────────────── */
static long bench_strops(long n) {
    const char *unit = "abcdefghij";
    char haystack[1001];
    for (int i = 0; i < 100; i++) memcpy(haystack + i * 10, unit, 10);
    haystack[1000] = '\0';
    long count = 0;
    for (long i = 0; i < n; i++) {
        count += (strstr(haystack, "efg") != NULL);
        const char *p = strstr(haystack, "hij");
        count += (p ? (long)(p - haystack) : -1) % 100;
    }
    return count;
}

/*  5 ── struct access ─────────────────────────── */
static long bench_struct(long n) {
    long x = 1, y = 2, sum = 0;
    for (long i = 0; i < n; i++) {
        x += i; y ^= i; sum += x + y;
    }
    return sum;
}

/*  6 ── switch/branch ─────────────────────────── */
static long bench_branch(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) {
        switch (i % 4) {
            case 0: sum += i;       break;
            case 1: sum -= i;       break;
            case 2: sum ^= i;       break;
            default: sum += (i * 2);
        }
    }
    return sum;
}

/*  7 ── recursion (fibonacci) ─────────────────── */
static long fib(long n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}
static long bench_recurse(long n) { return fib(n); }

/*  8 ── nested loops ──────────────────────────── */
static long bench_nested(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++)
        for (long j = 0; j < n; j++)
            for (long k = 0; k < n; k++)
                sum += ((i ^ j) + k) % 37;
    return sum;
}

/*  9 ── sort (bubble, same as OM built-in) ────── */
static long bench_sort(long n) {
    long *arr = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) arr[i] = (i * 2654435761UL) % 1000000;
    for (long i = 0; i < n; i++)
        for (long j = 0; j < n - i - 1; j++)
            if (arr[j] > arr[j + 1]) { long t = arr[j]; arr[j] = arr[j + 1]; arr[j + 1] = t; }
    long r = arr[0] + arr[n / 2] + arr[n - 1];
    free(arr);
    return r;
}

/* 10 ── while loop ────────────────────────────── */
static long bench_while(long n) {
    long i = 0, acc = 0;
    while (i < n) {
        acc += (i * i) % 101;
        acc ^= i;
        i++;
    }
    return acc;
}

/* 11 ── if/else chain ─────────────────────────── */
static long classify(long x) {
    if (x < 10)     return 1;
    if (x < 100)    return 2;
    if (x < 1000)   return 3;
    if (x < 10000)  return 4;
    if (x < 100000) return 5;
    return 6;
}
static long bench_ifelse(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) sum += classify(i % 200000);
    return sum;
}

/* 12 ── array indexing ────────────────────────── */
static long bench_arrindex(long n) {
    long sz = 10000;
    long *arr = calloc(sz, sizeof(long));
    for (long i = 0; i < sz; i++) arr[i] = i * 3;
    long sum = 0;
    for (long i = 0; i < n; i++) {
        long idx = (i * 7 + 13) % sz;
        sum += arr[idx];
        arr[idx] = sum % 100000;
    }
    free(arr);
    return sum;
}

/* 13 ── function calls ────────────────────────── */
static long add_one(long x) { return x + 1; }
static long add_two(long x) { return add_one(add_one(x)); }
static long add_four(long x) { return add_two(add_two(x)); }
static long bench_calls(long n) {
    long sum = 0;
    for (long i = 0; i < n; i++) sum += add_four(i % 1000);
    return sum;
}

/* 14 ── bitwise ops ───────────────────────────── */
static long bench_bitwise(long n) {
    long a = 0, b = 0, c = 0;
    for (long i = 0; i < n; i++) {
        a = (a ^ (i << 3)) + (i & 255);
        b = (b | (i >> 1)) ^ (a & 65535);
        c += (a ^ b) & 1023;
    }
    return a + b + c;
}

/* 15 ── combined workload ─────────────────────── */
static long bench_combined(long n) {
    long total = 0;

    /* math */
    long acc = 0;
    for (long i = 1; i < n; i++) {
        acc += (i * i) % 97;
        acc ^= (i << 2);
        acc += gcd(i, acc);
    }
    total += acc;

    /* array push + hof */
    long hn = n / 10;
    long cap = 16, len = 0;
    long *arr = malloc(cap * sizeof(long));
    for (long i = 0; i < hn; i++) {
        if (len == cap) { cap *= 2; arr = realloc(arr, cap * sizeof(long)); }
        arr[len++] = (i * 3) % 12345;
    }
    long *mapped = malloc(len * sizeof(long));
    for (long i = 0; i < len; i++) mapped[i] = (arr[i] * arr[i]) % 997;
    long reduced = 0, count = 0;
    for (long i = 0; i < len; i++) {
        if (mapped[i] % 2 == 0) { reduced += mapped[i]; count++; }
    }
    total += reduced + count;
    free(arr); free(mapped);

    /* struct */
    { long x = 1, y = 2;
      for (long i = 0; i < n; i++) {
        x += i; y ^= i; total += x + y;
      }
    }

    /* branch */
    for (long i = 0; i < n; i++) {
        switch (i % 4) {
            case 0: total += i;       break;
            case 1: total -= i;       break;
            case 2: total ^= i;       break;
            default: total += (i * 2);
        }
    }

    /* string */
    { long sn = n / 100;
      long sc = 16; long sl = 1;
      char *s = malloc(sc);
      s[0] = 'x'; s[1] = '\0';
      for (long i = 0; i < sn; i++) {
        if (sl + 2 > sc) { sc *= 2; s = realloc(s, sc); }
        s[sl++] = 'y'; s[sl] = '\0';
      }
      total += sl;
      free(s);
    }

    /* nested loop */
    { long ns = 50;
      for (long i = 0; i < ns; i++)
        for (long j = 0; j < ns; j++)
          for (long k = 0; k < ns; k++)
            total += ((i ^ j) + k) % 37;
    }

    return total;
}

int main(void) {
    int test_id; long n;
    scanf("%d %ld", &test_id, &n);
    long r = 0;
    switch (test_id) {
        case 0:  r = bench_math(n);     break;
        case 1:  r = bench_push(n);     break;
        case 2:  r = bench_hof(n);      break;
        case 3:  r = bench_strcat(n);   break;
        case 4:  r = bench_strops(n);   break;
        case 5:  r = bench_struct(n);   break;
        case 6:  r = bench_branch(n);   break;
        case 7:  r = bench_recurse(n);  break;
        case 8:  r = bench_nested(n);   break;
        case 9:  r = bench_sort(n);     break;
        case 10: r = bench_while(n);    break;
        case 11: r = bench_ifelse(n);   break;
        case 12: r = bench_arrindex(n); break;
        case 13: r = bench_calls(n);    break;
        case 14: r = bench_bitwise(n);  break;
        case 15: r = bench_combined(n); break;
    }
    printf("%ld\n", r);
    return 0;
}
