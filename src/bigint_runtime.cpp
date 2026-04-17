// bigint_runtime.cpp — C API implementation for OmScript's bigint type.
// Wraps the OmBigInt arbitrary-precision library (include/bigint.h).
#include "bigint_runtime.h"
#include "bigint.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>

using namespace omscript;

// The opaque type is just OmBigInt on the heap.
struct OmscBigInt {
    OmBigInt val;
    explicit OmscBigInt(OmBigInt v) : val(std::move(v)) {}
};

// ── Construction ──────────────────────────────────────────────────────────────

extern "C" omsc_bigint_t* omsc_bigint_new_i64(long long v) {
    return new OmscBigInt(OmBigInt::from_i64(static_cast<int64_t>(v)));
}

extern "C" omsc_bigint_t* omsc_bigint_new_str(const char* s) {
    if (!s) return nullptr;
    try {
        return new OmscBigInt(OmBigInt::from_string(s));
    } catch (...) {
        return nullptr;
    }
}

extern "C" omsc_bigint_t* omsc_bigint_copy(const omsc_bigint_t* a) {
    if (!a) return nullptr;
    return new OmscBigInt(a->val);
}

extern "C" void omsc_bigint_free(omsc_bigint_t* a) {
    delete a;
}

// ── Arithmetic ────────────────────────────────────────────────────────────────

extern "C" omsc_bigint_t* omsc_bigint_add(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return new OmscBigInt(a->val + b->val);
}

extern "C" omsc_bigint_t* omsc_bigint_sub(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return new OmscBigInt(a->val - b->val);
}

extern "C" omsc_bigint_t* omsc_bigint_mul(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return new OmscBigInt(a->val * b->val);
}

extern "C" omsc_bigint_t* omsc_bigint_div(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    if (b->val.is_zero()) {
        std::fputs("Runtime error: bigint division by zero\n", stderr);
        std::abort();
    }
    return new OmscBigInt(a->val / b->val);
}

extern "C" omsc_bigint_t* omsc_bigint_mod(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    if (b->val.is_zero()) {
        std::fputs("Runtime error: bigint modulo by zero\n", stderr);
        std::abort();
    }
    return new OmscBigInt(a->val % b->val);
}

extern "C" omsc_bigint_t* omsc_bigint_neg(const omsc_bigint_t* a) {
    return new OmscBigInt(-a->val);
}

extern "C" omsc_bigint_t* omsc_bigint_abs(const omsc_bigint_t* a) {
    return new OmscBigInt(a->val.abs());
}

extern "C" omsc_bigint_t* omsc_bigint_pow(const omsc_bigint_t* base, const omsc_bigint_t* exp) {
    return new OmscBigInt(base->val.pow(exp->val));
}

extern "C" omsc_bigint_t* omsc_bigint_gcd(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return new OmscBigInt(OmBigInt::gcd(a->val, b->val));
}

// ── Bitwise ───────────────────────────────────────────────────────────────────
// Bitwise ops delegate to the unsigned magnitude (sign ignored for now — two's
// complement semantics can be added later if needed).

extern "C" omsc_bigint_t* omsc_bigint_and(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return new OmscBigInt(OmBigInt(a->val.mag & b->val.mag, false));
}

extern "C" omsc_bigint_t* omsc_bigint_or(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return new OmscBigInt(OmBigInt(a->val.mag | b->val.mag, false));
}

extern "C" omsc_bigint_t* omsc_bigint_xor(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return new OmscBigInt(OmBigInt(a->val.mag ^ b->val.mag, false));
}

extern "C" omsc_bigint_t* omsc_bigint_shl(const omsc_bigint_t* a, long long n) {
    if (n <= 0) return omsc_bigint_copy(a);
    return new OmscBigInt(OmBigInt(a->val.mag << static_cast<size_t>(n), a->val.neg));
}

extern "C" omsc_bigint_t* omsc_bigint_shr(const omsc_bigint_t* a, long long n) {
    if (n <= 0) return omsc_bigint_copy(a);
    return new OmscBigInt(OmBigInt(a->val.mag >> static_cast<size_t>(n), a->val.neg));
}

// ── Comparison ────────────────────────────────────────────────────────────────

extern "C" int omsc_bigint_cmp(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return a->val.compare(b->val);
}

extern "C" int omsc_bigint_eq(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return a->val == b->val ? 1 : 0;
}

extern "C" int omsc_bigint_lt(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return a->val < b->val ? 1 : 0;
}

extern "C" int omsc_bigint_le(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return a->val <= b->val ? 1 : 0;
}

extern "C" int omsc_bigint_gt(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return a->val > b->val ? 1 : 0;
}

extern "C" int omsc_bigint_ge(const omsc_bigint_t* a, const omsc_bigint_t* b) {
    return a->val >= b->val ? 1 : 0;
}

// ── Conversion ────────────────────────────────────────────────────────────────

extern "C" char* omsc_bigint_tostring(const omsc_bigint_t* a) {
    std::string s = a->val.to_string();
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    if (buf) std::memcpy(buf, s.data(), s.size() + 1);
    return buf;
}

extern "C" char* omsc_bigint_tohexstring(const omsc_bigint_t* a) {
    std::string s = a->val.to_string(16);
    // Prepend "0x"
    std::string result = (a->val.is_negative() ? "-0x" : "0x") + s;
    char* buf = static_cast<char*>(std::malloc(result.size() + 1));
    if (buf) std::memcpy(buf, result.data(), result.size() + 1);
    return buf;
}

extern "C" long long omsc_bigint_to_i64(const omsc_bigint_t* a) {
    return static_cast<long long>(a->val.to_i64());
}

extern "C" long long omsc_bigint_bit_length(const omsc_bigint_t* a) {
    return static_cast<long long>(a->val.bit_length());
}

extern "C" int omsc_bigint_is_zero(const omsc_bigint_t* a) {
    return a->val.is_zero() ? 1 : 0;
}

extern "C" int omsc_bigint_is_negative(const omsc_bigint_t* a) {
    return a->val.is_negative() ? 1 : 0;
}
