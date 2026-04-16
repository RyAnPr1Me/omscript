#pragma once
// bigint_runtime.h — C API for OmScript's arbitrary-precision integer type.
//
// OmScript's `bigint` type is an opaque heap-allocated object.  All operations
// take and return `omsc_bigint_t*` pointers.  The caller is responsible for
// freeing objects it creates (or letting the GC/scope handle it).
//
// Each function that returns a new bigint allocates a fresh object; the
// arguments are not consumed.  Call `omsc_bigint_free()` to release memory.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OmscBigInt omsc_bigint_t;

// ── Construction ──────────────────────────────────────────────────────────────
/// Create a bigint from a signed 64-bit integer.
omsc_bigint_t* omsc_bigint_new_i64(long long v);

/// Create a bigint from a NUL-terminated decimal (or "0x"-prefixed hex) string.
/// Returns NULL on parse error.
omsc_bigint_t* omsc_bigint_new_str(const char* s);

/// Deep-copy an existing bigint.
omsc_bigint_t* omsc_bigint_copy(const omsc_bigint_t* a);

/// Release memory.  Safe to call with NULL.
void omsc_bigint_free(omsc_bigint_t* a);

// ── Arithmetic ────────────────────────────────────────────────────────────────
omsc_bigint_t* omsc_bigint_add(const omsc_bigint_t* a, const omsc_bigint_t* b);
omsc_bigint_t* omsc_bigint_sub(const omsc_bigint_t* a, const omsc_bigint_t* b);
omsc_bigint_t* omsc_bigint_mul(const omsc_bigint_t* a, const omsc_bigint_t* b);
/// Truncated division (toward zero, C semantics).  Aborts on division by zero.
omsc_bigint_t* omsc_bigint_div(const omsc_bigint_t* a, const omsc_bigint_t* b);
/// Remainder consistent with truncated division.  Aborts on division by zero.
omsc_bigint_t* omsc_bigint_mod(const omsc_bigint_t* a, const omsc_bigint_t* b);
omsc_bigint_t* omsc_bigint_neg(const omsc_bigint_t* a);
omsc_bigint_t* omsc_bigint_abs(const omsc_bigint_t* a);
omsc_bigint_t* omsc_bigint_pow(const omsc_bigint_t* base, const omsc_bigint_t* exp);
omsc_bigint_t* omsc_bigint_gcd(const omsc_bigint_t* a, const omsc_bigint_t* b);

// ── Bitwise ───────────────────────────────────────────────────────────────────
omsc_bigint_t* omsc_bigint_and(const omsc_bigint_t* a, const omsc_bigint_t* b);
omsc_bigint_t* omsc_bigint_or (const omsc_bigint_t* a, const omsc_bigint_t* b);
omsc_bigint_t* omsc_bigint_xor(const omsc_bigint_t* a, const omsc_bigint_t* b);
omsc_bigint_t* omsc_bigint_shl(const omsc_bigint_t* a, long long n);
omsc_bigint_t* omsc_bigint_shr(const omsc_bigint_t* a, long long n);

// ── Comparison ────────────────────────────────────────────────────────────────
/// Returns -1, 0, or 1.
int omsc_bigint_cmp(const omsc_bigint_t* a, const omsc_bigint_t* b);
int omsc_bigint_eq (const omsc_bigint_t* a, const omsc_bigint_t* b); // 1 if equal
int omsc_bigint_lt (const omsc_bigint_t* a, const omsc_bigint_t* b); // 1 if a < b
int omsc_bigint_le (const omsc_bigint_t* a, const omsc_bigint_t* b); // 1 if a <= b
int omsc_bigint_gt (const omsc_bigint_t* a, const omsc_bigint_t* b); // 1 if a > b
int omsc_bigint_ge (const omsc_bigint_t* a, const omsc_bigint_t* b); // 1 if a >= b

// ── Conversion ────────────────────────────────────────────────────────────────
/// Returns a malloc'd NUL-terminated decimal string.  Caller must free() it.
char* omsc_bigint_tostring(const omsc_bigint_t* a);
/// Returns a malloc'd "0x..."-prefixed hex string.  Caller must free() it.
char* omsc_bigint_tohexstring(const omsc_bigint_t* a);
/// Truncates to the low 64 bits (two's-complement).
long long omsc_bigint_to_i64(const omsc_bigint_t* a);
/// Returns the number of significant bits (0 for zero).
long long omsc_bigint_bit_length(const omsc_bigint_t* a);
/// Returns 1 if the value is zero.
int omsc_bigint_is_zero(const omsc_bigint_t* a);
/// Returns 1 if the value is negative.
int omsc_bigint_is_negative(const omsc_bigint_t* a);

#ifdef __cplusplus
} // extern "C"
#endif
