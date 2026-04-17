#pragma once
// bigint.h — Arbitrary-precision integer library for OmScript.
//
// Design:
//   OmBigUInt: arbitrary-precision unsigned integer (little-endian 64-bit limbs)
//   OmBigInt:  arbitrary-precision signed integer (sign + magnitude)
//   No heap allocation beyond the limb vector itself.
//   No external dependencies.
//
// Backward-compatibility aliases:
//   OmUInt128 / OmInt128 / OmUInt256 / OmInt256 are provided as thin wrappers
//   around OmBigUInt/OmBigInt for code that still uses the fixed-width API.

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// OmBigUInt — arbitrary-precision unsigned integer
// ─────────────────────────────────────────────────────────────────────────────
// Limbs stored little-endian (limbs[0] = least significant).
// Invariant: no trailing zero limbs (normalised).
struct OmBigUInt {
    std::vector<uint64_t> limbs; // little-endian 64-bit limbs

    // Constructors
    OmBigUInt() = default;
    explicit OmBigUInt(uint64_t v) { if (v) limbs.push_back(v); }
    explicit OmBigUInt(std::vector<uint64_t> ls) : limbs(std::move(ls)) { normalise(); }

    static OmBigUInt from_u64(uint64_t v) { return OmBigUInt(v); }
    static OmBigUInt from_i64(int64_t v);
    static OmBigUInt from_string(const std::string& s, int base = 10);

    uint64_t to_u64() const { return limbs.empty() ? 0ULL : limbs[0]; }
    int64_t  to_i64() const { return static_cast<int64_t>(to_u64()); }
    std::string to_string(int base = 10) const;

    bool is_zero() const { return limbs.empty(); }
    bool is_one()  const { return limbs.size() == 1 && limbs[0] == 1; }
    size_t bit_length() const;
    bool test_bit(size_t n) const;
    void set_bit(size_t n);

    // Comparison
    int compare(const OmBigUInt& o) const;
    bool operator==(const OmBigUInt& o) const { return limbs == o.limbs; }
    bool operator!=(const OmBigUInt& o) const { return !(*this == o); }
    bool operator< (const OmBigUInt& o) const { return compare(o) < 0; }
    bool operator<=(const OmBigUInt& o) const { return compare(o) <= 0; }
    bool operator> (const OmBigUInt& o) const { return compare(o) > 0; }
    bool operator>=(const OmBigUInt& o) const { return compare(o) >= 0; }

    // Arithmetic
    OmBigUInt operator+(const OmBigUInt& o) const;
    OmBigUInt operator-(const OmBigUInt& o) const; // assumes *this >= o
    OmBigUInt operator*(const OmBigUInt& o) const;
    OmBigUInt operator/(const OmBigUInt& o) const;
    OmBigUInt operator%(const OmBigUInt& o) const;
    OmBigUInt& operator+=(const OmBigUInt& o) { *this = *this + o; return *this; }
    OmBigUInt& operator-=(const OmBigUInt& o) { *this = *this - o; return *this; }
    OmBigUInt& operator*=(const OmBigUInt& o) { *this = *this * o; return *this; }
    OmBigUInt& operator/=(const OmBigUInt& o) { *this = *this / o; return *this; }
    OmBigUInt& operator%=(const OmBigUInt& o) { *this = *this % o; return *this; }

    // Bitwise
    OmBigUInt operator&(const OmBigUInt& o) const;
    OmBigUInt operator|(const OmBigUInt& o) const;
    OmBigUInt operator^(const OmBigUInt& o) const;
    OmBigUInt operator~() const = delete; // not well-defined for arbitrary precision
    OmBigUInt operator<<(size_t n) const;
    OmBigUInt operator>>(size_t n) const;

    // Power / GCD
    OmBigUInt pow(const OmBigUInt& exp) const;
    static OmBigUInt gcd(const OmBigUInt& a, const OmBigUInt& b);

    // Division with remainder
    static std::pair<OmBigUInt, OmBigUInt> divmod(const OmBigUInt& a, const OmBigUInt& b);

  private:
    void normalise() { while (!limbs.empty() && limbs.back() == 0) limbs.pop_back(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// OmBigInt — arbitrary-precision signed integer (sign + magnitude)
// ─────────────────────────────────────────────────────────────────────────────
struct OmBigInt {
    OmBigUInt mag; // magnitude (always non-negative)
    bool      neg; // true = negative

    OmBigInt() : neg(false) {}
    explicit OmBigInt(int64_t v)
        : mag(v < 0 ? OmBigUInt(static_cast<uint64_t>(-static_cast<__int128>(v)))
                    : OmBigUInt(static_cast<uint64_t>(v)))
        , neg(v < 0) {}
    OmBigInt(OmBigUInt m, bool negative) : mag(std::move(m)), neg(negative && !m.is_zero()) {}

    static OmBigInt from_i64(int64_t v) { return OmBigInt(v); }
    static OmBigInt from_u64(uint64_t v) { return OmBigInt(OmBigUInt(v), false); }
    static OmBigInt from_string(const std::string& s, int base = 10);

    int64_t  to_i64() const;
    uint64_t to_u64() const { return mag.to_u64(); }
    std::string to_string(int base = 10) const;

    bool is_zero()     const { return mag.is_zero(); }
    bool is_positive() const { return !neg && !is_zero(); }
    bool is_negative() const { return neg; }
    size_t bit_length() const { return mag.bit_length(); }

    OmBigInt operator-() const { return OmBigInt(mag, !neg || mag.is_zero()); }
    OmBigInt abs()        const { return OmBigInt(mag, false); }

    // Comparison
    int compare(const OmBigInt& o) const;
    bool operator==(const OmBigInt& o) const { return neg == o.neg && mag == o.mag; }
    bool operator!=(const OmBigInt& o) const { return !(*this == o); }
    bool operator< (const OmBigInt& o) const { return compare(o) < 0; }
    bool operator<=(const OmBigInt& o) const { return compare(o) <= 0; }
    bool operator> (const OmBigInt& o) const { return compare(o) > 0; }
    bool operator>=(const OmBigInt& o) const { return compare(o) >= 0; }

    // Arithmetic
    OmBigInt operator+(const OmBigInt& o) const;
    OmBigInt operator-(const OmBigInt& o) const;
    OmBigInt operator*(const OmBigInt& o) const;
    OmBigInt operator/(const OmBigInt& o) const;
    OmBigInt operator%(const OmBigInt& o) const;
    OmBigInt& operator+=(const OmBigInt& o) { *this = *this + o; return *this; }
    OmBigInt& operator-=(const OmBigInt& o) { *this = *this - o; return *this; }
    OmBigInt& operator*=(const OmBigInt& o) { *this = *this * o; return *this; }
    OmBigInt& operator/=(const OmBigInt& o) { *this = *this / o; return *this; }
    OmBigInt& operator%=(const OmBigInt& o) { *this = *this % o; return *this; }

    OmBigInt pow(const OmBigInt& exp) const;
    static OmBigInt gcd(const OmBigInt& a, const OmBigInt& b);
};

// ─────────────────────────────────────────────────────────────────────────────
// Backward-compatibility aliases for fixed-width types (OmUInt128, etc.)
// These use OmBigUInt/OmBigInt internally but expose the old fixed-width API.
// ─────────────────────────────────────────────────────────────────────────────

struct OmUInt128 {
    OmBigUInt val;

    OmUInt128() = default;
    OmUInt128(uint64_t lo, uint64_t hi) {
        if (lo) val.limbs.push_back(lo);
        if (hi) val.limbs.push_back(hi);
    }
    explicit OmUInt128(uint64_t v) : val(v) {}

    static OmUInt128 from_i64(int64_t v) {
        OmUInt128 r;
        r.val = OmBigUInt::from_i64(v);
        return r;
    }

    int64_t  to_i64() const { return val.to_i64(); }
    uint64_t to_u64() const { return val.to_u64(); }
    bool is_zero() const { return val.is_zero(); }

    OmUInt128 operator+(const OmUInt128& o) const { return wrap(val + o.val); }
    OmUInt128 operator-(const OmUInt128& o) const { return wrap(val - o.val); }
    OmUInt128 operator*(const OmUInt128& o) const { return wrap(val * o.val); }
    OmUInt128 operator/(const OmUInt128& o) const { return wrap(val / o.val); }
    OmUInt128 operator%(const OmUInt128& o) const { return wrap(val % o.val); }
    OmUInt128& operator+=(const OmUInt128& o) { val += o.val; return *this; }
    OmUInt128& operator-=(const OmUInt128& o) { val -= o.val; return *this; }
    OmUInt128& operator*=(const OmUInt128& o) { val *= o.val; return *this; }

    OmUInt128 operator&(const OmUInt128& o) const { return wrap(val & o.val); }
    OmUInt128 operator|(const OmUInt128& o) const { return wrap(val | o.val); }
    OmUInt128 operator^(const OmUInt128& o) const { return wrap(val ^ o.val); }
    OmUInt128 operator<<(int n) const { return wrap(val << static_cast<size_t>(n)); }
    OmUInt128 operator>>(int n) const { return wrap(val >> static_cast<size_t>(n)); }

    bool operator==(const OmUInt128& o) const { return val == o.val; }
    bool operator!=(const OmUInt128& o) const { return val != o.val; }
    bool operator< (const OmUInt128& o) const { return val <  o.val; }
    bool operator<=(const OmUInt128& o) const { return val <= o.val; }
    bool operator> (const OmUInt128& o) const { return val >  o.val; }
    bool operator>=(const OmUInt128& o) const { return val >= o.val; }

    std::string to_string(int base = 10) const { return val.to_string(base); }

  private:
    static OmUInt128 wrap(OmBigUInt v) {
        OmUInt128 r; r.val = std::move(v); return r;
    }
};

struct OmInt128 {
    OmBigInt val;

    OmInt128() = default;
    explicit OmInt128(int64_t v) : val(v) {}
    OmInt128(uint64_t lo, uint64_t hi) {
        val = OmBigInt(OmBigUInt(std::vector<uint64_t>{lo, hi}), false);
    }

    static OmInt128 from_i64(int64_t v) { OmInt128 r; r.val = OmBigInt(v); return r; }
    static OmInt128 from_u64(uint64_t v) { OmInt128 r; r.val = OmBigInt::from_u64(v); return r; }

    int64_t  to_i64() const { return val.to_i64(); }
    uint64_t to_u64() const { return val.to_u64(); }
    bool is_zero() const { return val.is_zero(); }

    OmInt128 operator+(const OmInt128& o) const { return wrap(val + o.val); }
    OmInt128 operator-(const OmInt128& o) const { return wrap(val - o.val); }
    OmInt128 operator*(const OmInt128& o) const { return wrap(val * o.val); }
    OmInt128 operator/(const OmInt128& o) const { return wrap(val / o.val); }
    OmInt128 operator%(const OmInt128& o) const { return wrap(val % o.val); }
    OmInt128 operator-() const { return wrap(-val); }

    bool operator==(const OmInt128& o) const { return val == o.val; }
    bool operator!=(const OmInt128& o) const { return val != o.val; }
    bool operator< (const OmInt128& o) const { return val <  o.val; }
    bool operator<=(const OmInt128& o) const { return val <= o.val; }
    bool operator> (const OmInt128& o) const { return val >  o.val; }
    bool operator>=(const OmInt128& o) const { return val >= o.val; }

    std::string to_string(int base = 10) const { return val.to_string(base); }

  private:
    static OmInt128 wrap(OmBigInt v) { OmInt128 r; r.val = std::move(v); return r; }
};

struct OmUInt256 {
    OmBigUInt val;

    OmUInt256() = default;
    explicit OmUInt256(uint64_t v) : val(v) {}
    OmUInt256(uint64_t w0, uint64_t w1, uint64_t w2, uint64_t w3) {
        val = OmBigUInt(std::vector<uint64_t>{w0, w1, w2, w3});
    }

    static OmUInt256 from_i64(int64_t v) { OmUInt256 r; r.val = OmBigUInt::from_i64(v); return r; }
    static OmUInt256 from_u64(uint64_t v) { OmUInt256 r; r.val = OmBigUInt(v); return r; }

    int64_t  to_i64() const { return val.to_i64(); }
    uint64_t to_u64() const { return val.to_u64(); }
    bool is_zero() const { return val.is_zero(); }

    OmUInt256 operator+(const OmUInt256& o) const { return wrap(val + o.val); }
    OmUInt256 operator-(const OmUInt256& o) const { return wrap(val - o.val); }
    OmUInt256 operator*(const OmUInt256& o) const { return wrap(val * o.val); }
    OmUInt256 operator/(const OmUInt256& o) const { return wrap(val / o.val); }
    OmUInt256 operator%(const OmUInt256& o) const { return wrap(val % o.val); }
    OmUInt256& operator+=(const OmUInt256& o) { val += o.val; return *this; }
    OmUInt256& operator-=(const OmUInt256& o) { val -= o.val; return *this; }

    OmUInt256 operator&(const OmUInt256& o) const { return wrap(val & o.val); }
    OmUInt256 operator|(const OmUInt256& o) const { return wrap(val | o.val); }
    OmUInt256 operator^(const OmUInt256& o) const { return wrap(val ^ o.val); }
    OmUInt256 operator<<(int n) const { return wrap(val << static_cast<size_t>(n)); }
    OmUInt256 operator>>(int n) const { return wrap(val >> static_cast<size_t>(n)); }

    bool operator==(const OmUInt256& o) const { return val == o.val; }
    bool operator!=(const OmUInt256& o) const { return val != o.val; }
    bool operator< (const OmUInt256& o) const { return val <  o.val; }
    bool operator<=(const OmUInt256& o) const { return val <= o.val; }
    bool operator> (const OmUInt256& o) const { return val >  o.val; }
    bool operator>=(const OmUInt256& o) const { return val >= o.val; }

    std::string to_string(int base = 10) const { return val.to_string(base); }

  private:
    static OmUInt256 wrap(OmBigUInt v) { OmUInt256 r; r.val = std::move(v); return r; }
};

struct OmInt256 {
    OmBigInt val;

    OmInt256() = default;
    explicit OmInt256(int64_t v) : val(v) {}

    static OmInt256 from_i64(int64_t v) { OmInt256 r; r.val = OmBigInt(v); return r; }

    int64_t  to_i64() const { return val.to_i64(); }
    uint64_t to_u64() const { return val.to_u64(); }
    bool is_zero() const { return val.is_zero(); }

    OmInt256 operator+(const OmInt256& o) const { return wrap(val + o.val); }
    OmInt256 operator-(const OmInt256& o) const { return wrap(val - o.val); }
    OmInt256 operator*(const OmInt256& o) const { return wrap(val * o.val); }
    OmInt256 operator/(const OmInt256& o) const { return wrap(val / o.val); }
    OmInt256 operator%(const OmInt256& o) const { return wrap(val % o.val); }
    OmInt256 operator-() const { return wrap(-val); }

    bool operator==(const OmInt256& o) const { return val == o.val; }
    bool operator!=(const OmInt256& o) const { return val != o.val; }
    bool operator< (const OmInt256& o) const { return val <  o.val; }
    bool operator<=(const OmInt256& o) const { return val <= o.val; }
    bool operator> (const OmInt256& o) const { return val >  o.val; }
    bool operator>=(const OmInt256& o) const { return val >= o.val; }

    std::string to_string(int base = 10) const { return val.to_string(base); }

  private:
    static OmInt256 wrap(OmBigInt v) { OmInt256 r; r.val = std::move(v); return r; }
};

} // namespace omscript
